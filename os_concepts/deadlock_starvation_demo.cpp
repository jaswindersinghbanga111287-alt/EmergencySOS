// Console runner for the OS concepts. The concepts themselves are NOT duplicated here:
// this program calls the exact same C++ functions the backend runs for the React
// OS Concepts page (backend/src/os_concepts.hpp). No MySQL needed.
//
// Build (from this folder):
//   g++ -std=c++17 -pthread -I../backend/src -I../backend/third_party deadlock_starvation_demo.cpp -o os_demo
#include <iostream>

#include "os_concepts.hpp"

static void print(const char* title, const oslab::json& result) {
    std::cout << "\n==== " << title << " ====\n";
    for (const auto& e : result["log"])
        std::cout << "  [" << e["t_ms"].get<long long>() << " ms] " << e["thread"].get<std::string>() << ": "
                  << e["event"].get<std::string>() << "\n";
    if (result.contains("summary")) std::cout << "  summary: " << result["summary"].dump() << "\n";
}

int main() {
    std::cout << "EmergencySOS OS Concepts Lab (same code as the backend)\n";
    oslab::json none = oslab::json::object();
    print("Race condition", oslab::raceCondition(none));
    print("Deadlock prevention", oslab::deadlockDemo(none));
    print("Starvation / bounded waiting", oslab::starvationDemo(none));
    print("Producer-consumer", oslab::producerConsumer(none));
    print("Readers-writers", oslab::readersWriters(none));
    print("Dining philosophers", oslab::diningPhilosophers(none));
    print("Banker's algorithm", oslab::bankers(oslab::sampleBankers(), {{"request_process", 1}, {"request", "1,0,2"}}));
    auto sched = oslab::scheduling(oslab::sampleProcesses(), "Starvation sample", none);
    std::cout << "\n==== CPU scheduling ====\n";
    for (const auto& a : sched["schedules"])
        std::cout << "  " << a["name"].get<std::string>() << ": avg wait " << a["avg_waiting"].get<double>()
                  << ", lowest-priority wait " << a["lowest_priority_wait"].get<int>() << "\n";
    return 0;
}
