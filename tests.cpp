#include "jcoro.hpp"

#include <cassert>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct test_scheduler : manual_scheduler {
    std::exception_ptr fatal_exception;
    uint64_t idle_calls = 0;

    void idle() override {
        ++idle_calls;
        manual_scheduler::idle();
    }

    void on_fatal_exception(std::exception_ptr ep) override {
        fatal_exception = ep;
    }
};

task<void, false> record_after(std::vector<std::string>& events, std::string name, uint64_t ticks) {
    co_await delay(ticks);
    events.push_back(std::move(name));
}

void test_delay_order() {
    test_scheduler sched;
    std::vector<std::string> events;

    auto a = spawn(record_after(events, "A", 3));
    auto b = spawn(record_after(events, "B", 1));
    auto c = spawn(record_after(events, "C", 0));

    a.start(sched);
    b.start(sched);
    c.start(sched);

    sched.run_all();

    assert((events == std::vector<std::string>{"C", "B", "A"}));
}

task<int, false> nested_value_task() {
    co_await delay(2);
    co_return 41;
}

task<int, false> nested_parent_task() {
    auto v = co_await nested_value_task();
    co_return v + 1;
}

void test_nested_result_propagation() {
    test_scheduler sched;
    int result = 0;

    auto t = spawn(nested_parent_task(), [&](int value) { result = value; });
    t.start(sched);
    sched.run_all();

    assert(result == 42);
}

task<int, false> failing_task() {
    co_await delay(1);
    throw std::runtime_error("boom");
}

void test_exception_to_root() {
    test_scheduler sched;
    bool callback_called = false;

    auto t = spawn(failing_task(), [&](int) { callback_called = true; });
    t.start(sched);
    sched.run_all();

    assert(!callback_called);
    assert(static_cast<bool>(sched.fatal_exception));

    try {
        std::rethrow_exception(sched.fatal_exception);
        assert(false && "expected exception from scheduler");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "boom");
    } catch (...) {
        assert(false && "expected std::runtime_error");
    }
}

root_task root_body(bool& flag) {
    flag = true;
    co_return;
}

void test_root_start_with_delay() {
    test_scheduler sched;
    bool flag = false;

    auto t = root_body(flag);
    t.start(sched, 5);
    sched.run_all();

    assert(flag);
    assert(sched.ticks_count == 6);
}

void test_root_start_without_delay() {
    test_scheduler sched;
    bool flag = false;

    auto t = root_body(flag);
    t.start(sched);
    sched.run_all();

    assert(flag);
    assert(sched.ticks_count == 1);
}

void test_idle_called_for_waiters_only() {
    test_scheduler sched;
    bool flag = false;

    auto t = root_body(flag);
    t.start(sched, 3);
    sched.run_all();

    assert(flag);
    assert(sched.idle_calls > 0);
}

void run_test(const char* name, const std::function<void()>& test_fn) {
    test_fn();
    std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
    run_test("delay order", test_delay_order);
    run_test("nested result propagation", test_nested_result_propagation);
    run_test("exception to root", test_exception_to_root);
    run_test("root start with delay", test_root_start_with_delay);
    run_test("root start without delay", test_root_start_without_delay);
    run_test("idle called for waiters only", test_idle_called_for_waiters_only);
    return 0;
}
