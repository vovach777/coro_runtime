#include "jcoro.hpp"
// --- ТЕСТЫ ---

task<void> worker(std::string name, uint64_t wait) {
    std::cout << "[" << name << "] Жду " << wait << " тиков...\n";
    co_await delay(wait);
    std::cout << "[" << name << "] Проснулся на тике " << g_sched.ticks_count << "\n";
}

int main() {
    std::cout << "Планировщик: Ready Queue + Priority Waiters\n";

    auto t1 = worker("A", 100);
    auto t2 = worker("B", 10); 

    t1.start();
    t2.start();

    g_sched.run_all();

    std::cout << "Всего тиков: " << g_sched.ticks_count << "\n";

    return 0;
}
