#include "jcoro.hpp"
// --- ТЕСТЫ ---

void_task worker(std::string name, uint64_t wait) {
    std::cout << "[" << name << "] Жду " << wait << " тиков...\n";
    co_await delay(wait);
    auto sched = co_await current_scheduler();
    std::cout << "[" << name << "] Проснулся на тике " << sched->ticks_count << "\n";
}

int main() {

    std::cout << "Планировщик: Ready Queue + Priority Waiters\n";
    manual_scheduler g_sched;

    spawn(worker("A", 100)).start(g_sched);
    spawn(worker("B", 10)).start(g_sched);

    g_sched.run_all();

    std::cout << "Всего тиков: " << g_sched.ticks_count << "\n";

    return 0;
}
