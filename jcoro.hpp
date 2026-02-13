/*
    https://godbolt.org/z/fT6Mzjrr7
*/
#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <variant>
#include <vector>
#include <iostream>
#include <optional>
#include <cassert>
#include <string>
#include <deque>
#include <cstdint>
#include <thread>
#include <queue> // Для std::priority_queue

// Кросс-компиляторный импорт для инструкций процессора
#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <immintrin.h>
#endif

// Базовая структура для всех promise_type
struct promise_base {
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    uint64_t wake_up_tick = 0; 
};

// Компаратор для Priority Queue (min-heap по времени пробуждения)
struct TimerComparator {
    bool operator()(std::coroutine_handle<promise_base> a, std::coroutine_handle<promise_base> b) const {
        return a.promise().wake_up_tick > b.promise().wake_up_tick;
    }
};

struct scheduler {
    uint64_t ticks_count = 0; 
    virtual void post(std::coroutine_handle<promise_base> h) = 0;
    virtual void idle() = 0;
    virtual ~scheduler() = default;
};

// Планировщик с разделением на Ready Queue и Waiters Queue
struct manual_scheduler : scheduler {
    // FIFO очередь для готовых задач. O(1) вставка/удаление.
    std::deque<std::coroutine_handle<promise_base>> ready_queue;

    // Очередь таймеров (Waiters). 
    // Текущая реализация: Бинарная куча на std::vector.
    std::priority_queue<std::coroutine_handle<promise_base>, 
                        std::vector<std::coroutine_handle<promise_base>>, 
                        TimerComparator> waiters_queue;

    void post(std::coroutine_handle<promise_base> h) override {
        if (!h) return;
        // Если задача не ждет — сразу в готовую очередь, иначе — в список ожидания
        if (h.promise().wake_up_tick <= ticks_count) {
            ready_queue.push_back(h);
        } else {
            waiters_queue.push(h);
        }
    }

    void idle() override {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("yield");
#else
        std::this_thread::yield();
#endif
    }

    void run_all() {
        while (!ready_queue.empty() || !waiters_queue.empty()) {
            
            // 1. Проверяем "ждунов": не пора ли кому-то в Ready Queue?
            while (!waiters_queue.empty() && waiters_queue.top().promise().wake_up_tick <= ticks_count) {
                ready_queue.push_back(waiters_queue.top());
                waiters_queue.pop();
            }

            // 2. Если работать не над чем — уходим в idle
            if (ready_queue.empty()) {
                idle();
                ticks_count++; 
                continue;
            }

            // 3. Работаем с готовой задачей (FIFO)
            auto h = ready_queue.front();
            ready_queue.pop_front();

            if (!h.done()) {
                // Возобновляем корутину.
                // Если она должна продолжиться, она сама (или её авейтеры) 
                // вызовут post() в процессе выполнения.
                h.resume();
            }
            
            ticks_count++;
        }
    }
};

inline manual_scheduler g_sched;

struct yield_awaiter {
    uint64_t delay_ticks;
    bool await_ready() { return delay_ticks == 0; }
    
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) {
        // Устанавливаем абсолютное время пробуждения.
        h.promise().wake_up_tick = g_sched.ticks_count + delay_ticks;
        
        // Сами кладем корутину в планировщик. 
        // Если delay_ticks > 0, она попадет в waiters_queue.
        g_sched.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
    }
    
    void await_resume() {}
};

template<typename T>
struct task {
    struct promise_type : promise_base {
        std::variant<std::monostate, T, std::exception_ptr, std::coroutine_handle<promise_type>> result;

        task get_return_object() { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }

        ~promise_type() {
            if (result.index() == 3) {
                auto nested_h = std::get<3>(result);
                if (nested_h) nested_h.destroy();
            }
        }

        auto final_suspend() noexcept {
            struct awaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto& p = h.promise();
                    if (p.result.index() == 3) {
                        auto nested_h = std::get<3>(p.result);
                        nested_h.promise().continuation = p.continuation;
                        return nested_h; 
                    }
                    // Возвращаем хендл продолжения для симметричной передачи
                    return p.continuation;
                }
                void await_resume() noexcept {}
            };
            return awaiter{};
        }

        void return_value(T v) { result.template emplace<1>(std::move(v)); }
        void return_value(task<T>&& t) { result.template emplace<3>(std::exchange(t.h, {})); }
        void unhandled_exception() noexcept { result.template emplace<2>(std::current_exception()); }

        template<typename U>
        auto await_transform(task<U> t) { return std::move(t); }

        auto await_transform(yield_awaiter y) { return y; }
    };

    std::coroutine_handle<promise_type> h;
    explicit task(std::coroutine_handle<promise_type> h) : h(h) {}
    task(task&& other) noexcept : h(std::exchange(other.h, {})) {}
    ~task() { if (h) h.destroy(); }

    std::coroutine_handle<promise_base> as_base() const {
        return std::coroutine_handle<promise_base>::from_promise(h.promise());
    }

    void start() {
        if (h) {
            g_sched.post(as_base());
        }
    }

    bool await_ready() const noexcept { return !h || h.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        h.promise().continuation = awaiting;
        // Мы НЕ ставим родителя в очередь здесь.
        // Он проснется сам, когда дочерняя задача вызовет continuation в final_suspend.
        return h;
    }

    T await_resume() {
        assert(h && "Empty task");
        auto current_h = h;
        T final_result;
        while (true) {
            auto& p = current_h.promise();
            if (p.result.index() == 2) std::rethrow_exception(std::get<2>(p.result));
            if (p.result.index() == 1) {
                final_result = std::move(std::get<1>(p.result));
                break; 
            }
            if (p.result.index() == 3) {
                auto next_h = std::get<3>(p.result);
                p.result.template emplace<0>(); 
                if (current_h != h) current_h.destroy();
                current_h = next_h;
                continue;
            }
            throw std::runtime_error("No result");
        }
        return final_result;
    }
};

template<>
struct task<void> {
    struct promise_type : promise_base {
        std::variant<std::monostate, std::monostate, std::exception_ptr> result;
        task<void> get_return_object() { return task<void>{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        auto final_suspend() noexcept {
            struct awaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    return h.promise().continuation;
                }
                void await_resume() noexcept {}
            };
            return awaiter{};
        }
        void return_void() { result.template emplace<1>(); }
        void unhandled_exception() noexcept { result.template emplace<2>(std::current_exception()); }
        
        template<typename U> auto await_transform(task<U> t) { return std::move(t); }
        auto await_transform(yield_awaiter y) { return y; }
    };

    std::coroutine_handle<promise_type> h;
    explicit task(std::coroutine_handle<promise_type> h) : h(h) {}
    task(task&& other) noexcept : h(std::exchange(other.h, {})) {}
    ~task() { if (h) h.destroy(); }

    std::coroutine_handle<promise_base> as_base() const {
        return std::coroutine_handle<promise_base>::from_promise(h.promise());
    }

    void start() {
        if (h) {
            g_sched.post(as_base());
        }
    }

    bool await_ready() const noexcept { return !h || h.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        h.promise().continuation = awaiting;
        return h;
    }
    void await_resume() {
        assert(h);
        auto& p = h.promise();
        if (p.result.index() == 2) std::rethrow_exception(std::get<2>(p.result));
    }
};

yield_awaiter delay(uint64_t n) { return {n}; }
