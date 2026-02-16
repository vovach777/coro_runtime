/*
https://godbolt.org/z/nqfes9eYr
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

struct promise_base {
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    uint64_t wake_up_tick = 0; 
};

struct TimerComparator {
    bool operator()(std::coroutine_handle<promise_base> a, std::coroutine_handle<promise_base> b) const {
        return a.promise().wake_up_tick > b.promise().wake_up_tick;
    }
};

struct scheduler_interface {
    uint64_t ticks_count = 0;
    virtual void post(std::coroutine_handle<promise_base> h) = 0;
    virtual void idle() = 0;
    virtual void on_fatal_exception(std::exception_ptr ep) = 0;
    virtual ~scheduler_interface() = default;
};

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
            std::cerr << "[FATAL] Root Task error: " << e.what() << std::endl;
        }
        std::terminate();
    }

    void run_all() {
        while (!ready_queue.empty() || !waiters_queue.empty()) {
            while (!waiters_queue.empty() && waiters_queue.top().promise().wake_up_tick <= ticks_count) {
                ready_queue.push_back(waiters_queue.top());
                waiters_queue.pop();
            }
            if (ready_queue.empty()) { idle(); ticks_count++; continue; }
            auto h = ready_queue.front();
            ready_queue.pop_front();
            if (!h.done()) h.resume();
            ticks_count++;
        }
    }
};

inline manual_scheduler g_sched;

struct yield_awaiter {
    uint64_t delay_ticks;
    bool await_ready() const noexcept { return delay_ticks == 0; }
    template<typename P>
    void await_suspend(std::coroutine_handle<P> h) const noexcept {
        h.promise().wake_up_tick = g_sched.ticks_count + delay_ticks;
        g_sched.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
    }
    void await_resume() const noexcept {}
};

inline yield_awaiter delay(uint64_t ticks) { return {ticks}; }

// --- УНИФИЦИРОВАННЫЙ ОБРАБОТЧИК ВОЗВРАТА ---

template<typename T, typename Derived>
struct return_handler {
    void return_value(T v) { static_cast<Derived*>(this)->set_result(std::move(v)); }
};

template<typename Derived>
struct return_handler<void, Derived> {
    void return_void() { static_cast<Derived*>(this)->set_result(); }
};

// --- ЕДИНЫЙ ШАБЛОН TASK И PROMISE ---

template<typename T = void, bool IsRoot = false>
struct task;

template<typename T, bool IsRoot>
struct task_promise : promise_base, return_handler<T, task_promise<T, IsRoot>> {
    using storage_type = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    std::variant<std::monostate, storage_type, std::exception_ptr> result;

    // ТО САМОЕ МЕСТО: возвращаем объект нашей задачи
    task<T, IsRoot> get_return_object() {
        return task<T, IsRoot>{std::coroutine_handle<task_promise>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct final_awaiter {
            // Если Root -> возвращаем true (await_ready), чтобы не засыпать и удалиться
            bool await_ready() noexcept { return IsRoot; } 
            std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        return final_awaiter{};
    }

    void set_result(storage_type v = {}) { result.template emplace<1>(std::move(v)); }
    
    void unhandled_exception() noexcept {
        if constexpr (IsRoot) g_sched.on_fatal_exception(std::current_exception());
        else result.template emplace<2>(std::current_exception());
    }

    template<typename U, bool R>
    auto await_transform(task<U, R>&& t) { return std::move(t); }
    auto await_transform(yield_awaiter y) { return y; }
};

/**
 * Унифицированный Task класс.
 */
template<typename T, bool IsRoot>
struct [[nodiscard]] task {
    using promise_type = task_promise<T, IsRoot>;
    std::coroutine_handle<promise_type> h;

    explicit task(std::coroutine_handle<promise_type> handle) : h(handle) {}
    task(task&& o) noexcept : h(std::exchange(o.h, {})) {}
    task(const task&) = delete;

    ~task() { if (h) h.destroy(); }

    void start() requires (IsRoot) {
        if (h) {
            g_sched.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
            h = {}; 
        }
    }

    void start(uint64_t delay_ticks) requires (IsRoot) {
        if (h) {
            h.promise().wake_up_tick = g_sched.ticks_count + delay_ticks;
            g_sched.post(std::coroutine_handle<promise_base>::from_promise(h.promise()));
            h = {}; 
        }
    }

    struct awaiter {
        std::coroutine_handle<promise_type> handle;
        bool await_ready() { return !handle || handle.done(); }
        auto await_suspend(std::coroutine_handle<> cont) {
            handle.promise().continuation = cont;
            return handle;
        }
        T await_resume() {
            auto& p = handle.promise();
            if (p.result.index() == 2) std::rethrow_exception(std::get<2>(p.result));
            if constexpr (!std::is_void_v<T>) return std::move(std::get<1>(p.result));
        }
    };

    auto operator co_await() && requires (!IsRoot) { 
        return awaiter{std::exchange(h, {})}; 
    }

    auto as_root() && requires (std::is_void_v<T> && !IsRoot) {
        // Мы просто меняем политику (IsRoot=true) для того же адреса хендла
        return task<void, true>{std::coroutine_handle<task_promise<void, true>>::from_address(std::exchange(h, {}).address())};
    }
};
