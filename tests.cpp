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

task<int, false> nested_multi_delay_leaf(std::vector<std::string>& trace) {
    trace.push_back("leaf-start");
    co_await delay(1);
    trace.push_back("leaf-after-delay-1");
    co_await delay(2);
    trace.push_back("leaf-after-delay-2");
    co_return 10;
}

task<int, false> nested_multi_delay_parent(std::vector<std::string>& trace) {
    trace.push_back("parent-start");
    co_await delay(1);
    trace.push_back("parent-after-delay");
    auto v = co_await nested_multi_delay_leaf(trace);
    trace.push_back("parent-after-await");
    co_return v + 5;
}

void test_nested_multiple_delays_in_body() {
    test_scheduler sched;
    std::vector<std::string> trace;
    int result = 0;

    auto t = spawn(nested_multi_delay_parent(trace), [&](int value) { result = value; });
    t.start(sched);
    sched.run_all();

    assert(result == 15);
    assert((trace == std::vector<std::string>{
        "parent-start",
        "parent-after-delay",
        "leaf-start",
        "leaf-after-delay-1",
        "leaf-after-delay-2",
        "parent-after-await"
    }));
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

task<int, false> nested_throw_after_delays(std::vector<std::string>& trace) {
    trace.push_back("nested-throw-start");
    co_await delay(1);
    trace.push_back("nested-throw-after-delay-1");
    co_await delay(1);
    trace.push_back("nested-throw-after-delay-2");
    throw std::runtime_error("nested-after-delays");
}

void test_nested_exception_after_multiple_delays() {
    test_scheduler sched;
    std::vector<std::string> trace;
    bool callback_called = false;

    auto t = spawn(nested_throw_after_delays(trace), [&](int) { callback_called = true; });
    t.start(sched);
    sched.run_all();

    assert(!callback_called);
    assert((trace == std::vector<std::string>{
        "nested-throw-start",
        "nested-throw-after-delay-1",
        "nested-throw-after-delay-2"
    }));
    assert(static_cast<bool>(sched.fatal_exception));

    try {
        std::rethrow_exception(sched.fatal_exception);
        assert(false && "expected exception from scheduler");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "nested-after-delays");
    } catch (...) {
        assert(false && "expected std::runtime_error");
    }
}

root_task root_body(bool& flag) {
    flag = true;
    co_return;
}

root_task root_throw_after_multiple_delays(std::vector<std::string>& trace) {
    trace.push_back("root-start");
    co_await delay(1);
    trace.push_back("root-after-delay-1");
    co_await delay(2);
    trace.push_back("root-after-delay-2");
    throw std::runtime_error("root-after-delays");
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

void test_root_exception_after_multiple_delays() {
    test_scheduler sched;
    std::vector<std::string> trace;

    auto t = root_throw_after_multiple_delays(trace);
    t.start(sched);
    sched.run_all();

    assert((trace == std::vector<std::string>{
        "root-start",
        "root-after-delay-1",
        "root-after-delay-2"
    }));
    assert(static_cast<bool>(sched.fatal_exception));

    try {
        std::rethrow_exception(sched.fatal_exception);
        assert(false && "expected exception from scheduler");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "root-after-delays");
    } catch (...) {
        assert(false && "expected std::runtime_error");
    }
}



task<int, false> always_failing_child(const std::string& marker, std::vector<std::string>& trace) {
    trace.push_back("child-start-" + marker);
    co_await delay(1);
    trace.push_back("child-throw-" + marker);
    throw std::runtime_error("child-fail-" + marker);
}

task<int, false> parent_catches_children_then_throws(std::vector<std::string>& trace) {
    int recovered_sum = 0;

    try {
        co_await always_failing_child("1", trace);
        assert(false && "child 1 must throw");
    } catch (const std::runtime_error& e) {
        trace.push_back("parent-caught-1");
        assert(std::string(e.what()) == "child-fail-1");
        recovered_sum += 10;
    }

    co_await delay(1);
    trace.push_back("parent-middle-delay");

    try {
        co_await always_failing_child("2", trace);
        assert(false && "child 2 must throw");
    } catch (const std::runtime_error& e) {
        trace.push_back("parent-caught-2");
        assert(std::string(e.what()) == "child-fail-2");
        recovered_sum += 20;
    }

    co_await delay(1);
    trace.push_back("parent-finished-trials");
    assert(recovered_sum == 30);

    throw std::runtime_error("parent-final-uncaught");
}

void test_parent_catches_multiple_child_exceptions_then_fails_to_root() {
    test_scheduler sched;
    std::vector<std::string> trace;
    bool callback_called = false;

    auto t = spawn(parent_catches_children_then_throws(trace), [&](int) { callback_called = true; });
    t.start(sched);
    sched.run_all();

    assert(!callback_called);
    assert((trace == std::vector<std::string>{
        "child-start-1",
        "child-throw-1",
        "parent-caught-1",
        "parent-middle-delay",
        "child-start-2",
        "child-throw-2",
        "parent-caught-2",
        "parent-finished-trials"
    }));

    assert(static_cast<bool>(sched.fatal_exception));
    try {
        std::rethrow_exception(sched.fatal_exception);
        assert(false && "expected exception from scheduler");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "parent-final-uncaught");
    } catch (...) {
        assert(false && "expected std::runtime_error");
    }
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
    run_test("nested multiple delays in body", test_nested_multiple_delays_in_body);
    run_test("exception to root", test_exception_to_root);
    run_test("nested exception after multiple delays", test_nested_exception_after_multiple_delays);
    run_test("root start with delay", test_root_start_with_delay);
    run_test("root start without delay", test_root_start_without_delay);
    run_test("root exception after multiple delays", test_root_exception_after_multiple_delays);
    run_test("parent catches child exceptions then fails to root", test_parent_catches_multiple_child_exceptions_then_fails_to_root);
    run_test("exception to root", test_exception_to_root);
    run_test("root start with delay", test_root_start_with_delay);
    run_test("root start without delay", test_root_start_without_delay);
    run_test("idle called for waiters only", test_idle_called_for_waiters_only);
    return 0;
}
