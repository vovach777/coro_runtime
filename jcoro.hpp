/*
https://godbolt.org/z/vM9xqn4Eh
*/
#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <variant>
#include <vector>
#include <iostream>
#include <cassert>
#include <string>
#include <deque>
#include <cstdint>
#include <thread>
#include <queue>
#include <type_traits>

#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <immintrin.h>
#endif

// --- ИНФРАСТРУКТУРА ПЛАНИРОВЩИКА ---

struct promise_base;

/**
 * Интерфейс планировщика.
 * Задачи привязаны к экземпляру, а не к глобальной переменной.
 */
struct scheduler_interface {
    uint64_t ticks_count = 0;
    virtual void post(std::coroutine_handle<promise_base> h) = 0;
    virtual void idle() = 0;
    virtual void on_fatal_exception(std::exception_ptr ep) = 0;
    virtual ~scheduler_interface() = default;
};

// --- БАЗОВЫЙ ПРОМИС ---

struct promise_base {
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    uint64_t wake_up_tick = 0; 
    scheduler_interface* sched = nullptr; // Контекст планировщика
};

struct TimerComparator {
    bool operator()(std::coroutine_handle<promise_base> a, std::coroutine_handle<promise_base> b) const {
        return a.promise().wake_up_tick > b.promise().wake_up_tick;
    }
};

// --- КОНКРЕТНЫЙ ПЛАНИРОВЩИК ---

struct manual_scheduler : scheduler_interface {
    std::deque<std::coroutine_handle<promise_base>> ready_queue;
    std::priority_queue<std::coroutine_handle<promise_base>, 
                        std::vector<std::coroutine_handle<promise_base>>, 
                        TimerComparator> waiters_queue;

    void post(std::coroutine_handle<promise_base> h) override {
        if (!h) return;
        if (h.promise().wake_up_tick <= ticks_count) {
            ready_queue.push_back(h);
        } else {
            waiters_queue.push(h);
        }
    }

    void idle() override {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    void on_fatal_exception(std::exception_ptr ep) override {
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::cerr << "\n[SCHEDULER] Критическая ошибка: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "\n[SCHEDULER] Неизвестная ошибка!" << std::endl;
        }
    }

    void run_all() {
        while (!ready_queue.empty() || !waiters_queue.empty()) {
            while (!waiters_queue.empty() && waiters_queue.top().promise().wake_up_tick <= ticks_count) {
                ready_queue.push_back(waiters_queue.top());
                waiters_queue.pop();
            }
            if (ready_queue.empty()) { 
                if (waiters_queue.empty()) break;
                idle(); 
                ticks_count++; 
                continue; 
            }
            auto h = ready_queue.front();
            ready_queue.pop_front();
            
            if (!h.done()) {
                h.resume();
            }
            ticks_count++;
        }
    }
};

// --- МЕХАНИЗМ ЗАДЕРЖКИ ---

struct yield_awaiter {
    uint64_t delay_ticks;
    bool await_ready() const noexcept { return delay_ticks == 0; }

    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const noexcept {
        auto& promise = h.promise();
        if (promise.sched) {
            promise.wake_up_tick = promise.sched->ticks_count + delay_ticks;
            promise.sched->post(std::coroutine_handle<promise_base>::from_promise(promise));
        }
    }
    void await_resume() const noexcept {}
};

inline yield_awaiter delay(uint64_t ticks) { return {ticks}; }

// --- УНИФИЦИРОВАННЫЙ ПРОМИС ---

template<typename T, typename Derived>
struct promise_return_handler {
    void return_value(T v) { static_cast<Derived*>(this)->set_result(std::move(v)); }
};

template<typename Derived>
struct promise_return_handler<void, Derived> {
    void return_void() { static_cast<Derived*>(this)->set_result(); }
};

template<typename T = void, bool IsRoot = false>
struct task;

template<typename T, bool IsRoot>
struct task_promise : promise_base, promise_return_handler<T, task_promise<T, IsRoot>> {
    using storage_type = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    std::variant<std::monostate, storage_type, std::exception_ptr> result;

    task<T, IsRoot> get_return_object() {
        return task<T, IsRoot>{std::coroutine_handle<task_promise>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    
    auto final_suspend() noexcept {
        struct final_awaiter {
            bool await_ready() noexcept { return false; } 
            std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise> h) noexcept {
                return h.promise().continuation; 
            }
            void await_resume() noexcept {}
        };

        if constexpr (IsRoot) return std::suspend_never{};
        else return final_awaiter{};
    }

    void set_result(storage_type v = {}) { result.template emplace<1>(std::move(v)); }
    
    void unhandled_exception() noexcept {
        if constexpr (IsRoot) {
            if (this->sched) this->sched->on_fatal_exception(std::current_exception());
        } else {
            result.template emplace<2>(std::current_exception());
        }
    }

    /**
     * КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: Проброс контекста через await_transform.
     * Теперь родитель (this) передает свой планировщик ребенку (t) 
     * до начала ожидания. Это избавляет от небезопасных кастов в await_suspend.
     */
    template<typename U, bool R>
    auto await_transform(task<U, R>&& t) {
        if (t.h) {
            t.h.promise().sched = this->sched;
        }
        return std::move(t);
    }

    // Разрешаем ожидание delay (yield_awaiter)
    auto await_transform(yield_awaiter y) { return y; }
};

// --- ОБЕРТКА TASK ---

template<typename T, bool IsRoot>
struct [[nodiscard]] task {
    using promise_type = task_promise<T, IsRoot>;
    std::coroutine_handle<promise_type> h;

    explicit task(std::coroutine_handle<promise_type> handle) : h(handle) {}
    
    task(task&& o) noexcept : h(std::exchange(o.h, {})) {}
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    ~task() { 
        if (h) h.destroy(); 
    }

    struct awaiter {
        std::coroutine_handle<promise_type> handle;
        bool await_ready() { return !handle || handle.done(); }

        auto await_suspend(std::coroutine_handle<> cont) {
            // Контекст (sched) уже проброшен через await_transform родителя.
            // Нам остается только связать цепочку возобновления.
            handle.promise().continuation = cont;
            return handle; 
        }

        T await_resume() {
            auto& p = handle.promise();
            bool has_exc = (p.result.index() == 2);
            std::exception_ptr exc;
            if (has_exc) exc = std::get<2>(p.result);

            using storage_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
            storage_t res;
            if constexpr (!std::is_void_v<T>) {
                if (!has_exc) res = std::move(std::get<1>(p.result));
            }

            handle.destroy();
            handle = {};

            if (has_exc) std::rethrow_exception(exc);
            if constexpr (!std::is_void_v<T>) return std::move(res);
        }
    };

    auto operator co_await() && requires (!IsRoot) { 
        return awaiter{std::exchange(h, {})}; 
    }

    // Запуск корневой задачи в планировщике
    void start(scheduler_interface& s) requires (IsRoot) {
        if (h) {
            h.promise().sched = &s; 
            s.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
            h = {}; 
        }
    }

    void start(scheduler_interface& s, uint64_t delay_ticks) requires (IsRoot) {
        if (h) {
            h.promise().sched = &s;
            h.promise().wake_up_tick = s.ticks_count + delay_ticks;
            s.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
            h = {}; 
        }
    }
};

/**
 * Псевдонимы типов.
 */
using root_task = task<void, true>;

/**
 * Запуск фоновой задачи.
 */
root_task spawn(task<void, false> t) {
    co_await std::move(t);
}

template<typename T, typename F>
root_task spawn(task<T, false> t, F callback) {
    static_assert(!std::is_void_v<T>, "Используйте spawn(task) для void задач.");
    auto result = co_await std::move(t);
    callback(std::move(result));
}

