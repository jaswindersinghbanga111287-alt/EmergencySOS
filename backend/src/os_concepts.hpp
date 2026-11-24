// EmergencySOS — Operating System concepts, implemented in C++ and served to the React
// frontend through the /api/os/* routes in main.cpp.
//
// Every demo below runs with REAL std::thread objects inside the backend process when
// the user presses "Run" on the OS Concepts page. Nothing is simulated in JavaScript.
//
//   Concept                         Where it is in this file           Used live by the hospital app
//   ------------------------------  ---------------------------------  -------------------------------------------
//   Threads / thread pool           ServerMetrics                      cpp-httplib worker threads serve every request
//   Race condition                  raceCondition()                    why counters below use std::atomic / mutex
//   Mutex / critical section        EventLog, ServerMetrics            connection pool free-list (db_pool.hpp)
//   Counting semaphore (P / V)      Semaphore, producerConsumer()      connection pool limits DB connections
//   Producer-consumer               producerConsumer()                 ambulances -> triage bay -> doctors
//   Readers-writers lock            ServerMetrics, readersWriters()    route statistics read by /api/os/live
//   Deadlock (4 conditions)         deadlockDemo()                     /api/allocations lock ordering
//   Deadlock avoidance (Banker's)   bankers()                          beds / staff / equipment safety check
//   Dining philosophers             diningPhilosophers()               surgeons sharing instrument trays
//   Starvation / bounded waiting    TicketLock, starvationDemo()       pool waits time out after 10 s
//   CPU scheduling + aging          scheduling()                       emergency queue = ready queue
//   Background (daemon) thread      TimerThreadStats                   severity auto-release thread in main.cpp
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <regex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"

#ifdef _WIN32
#include <process.h>
#define OSLAB_GETPID _getpid
#else
#include <unistd.h>
#define OSLAB_GETPID getpid
#endif

namespace oslab {

using json = nlohmann::ordered_json;  // keeps keys in insertion order (SQL column order)
using Clock = std::chrono::steady_clock;
using std::string;
using namespace std::chrono_literals;

struct BadInput {
    string message;
};

// Reads an integer parameter sent by the frontend and keeps it inside safe limits.
inline int intParam(const json& p, const string& key, int def, int lo, int hi) {
    if (!p.is_object() || !p.contains(key) || p[key].is_null()) return def;
    long long v = def;
    if (p[key].is_number()) v = p[key].get<long long>();
    else if (p[key].is_string()) {
        try { v = std::stoll(p[key].get<string>()); } catch (...) { throw BadInput{key + " must be a number."}; }
    }
    if (v < lo || v > hi) throw BadInput{key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) + "."};
    return static_cast<int>(v);
}
inline string strParam(const json& p, const string& key, const string& def) {
    if (!p.is_object() || !p.contains(key) || !p[key].is_string()) return def;
    string s = p[key].get<string>();
    return s.empty() ? def : s;
}

inline long long epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// =====================================================================================
// Thread-safe event log (mutual exclusion around a shared vector)
// =====================================================================================
class EventLog {
public:
    void add(const string& who, const string& what) {
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
        std::lock_guard<std::mutex> lock(m_);  // critical section: many threads append here
        events_.push_back({{"t_ms", ms}, {"thread", who}, {"event", what}});
    }
    json toJson() const {
        std::lock_guard<std::mutex> lock(m_);
        return events_;
    }
private:
    Clock::time_point start_ = Clock::now();
    mutable std::mutex m_;
    json events_ = json::array();
};

// =====================================================================================
// Counting semaphore built from a mutex + condition variable (C++17 has none built in)
// =====================================================================================
class Semaphore {
public:
    explicit Semaphore(int initial) : count_(initial) {}
    void wait() {  // P() / down()
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return count_ > 0; });
        --count_;
    }
    bool tryWait() {
        std::lock_guard<std::mutex> lock(m_);
        if (count_ <= 0) return false;
        --count_;
        return true;
    }
    void signal() {  // V() / up()
        {
            std::lock_guard<std::mutex> lock(m_);
            ++count_;
        }
        cv_.notify_one();
    }
    int value() const {
        std::lock_guard<std::mutex> lock(m_);
        return count_;
    }
private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    int count_;
};

// =====================================================================================
// Ticket lock: FIFO lock that guarantees bounded waiting (no starvation)
// =====================================================================================
class TicketLock {
public:
    int lock() {
        int my = next_.fetch_add(1);                      // take a ticket (atomic)
        while (serving_.load() != my) std::this_thread::yield();
        return my;
    }
    void unlock() { serving_.fetch_add(1); }
private:
    std::atomic<int> next_{0}, serving_{0};
};

// =====================================================================================
// Live server metrics: thread pool, in-flight requests, per-route statistics.
// Writers (every finished request) take an exclusive lock; readers (/api/os/live) take a
// shared lock, so many dashboards can read at the same time — the readers-writers problem.
// =====================================================================================
class ServerMetrics {
public:
    void requestStarted() {
        ++total_;
        long long now = ++inFlight_;
        long long peak = peak_.load();
        while (now > peak && !peak_.compare_exchange_weak(peak, now)) {}
    }
    void requestFinished(const string& method, const string& path, int status, double ms) {
        --inFlight_;
        string key = method + " " + std::regex_replace(path, std::regex("/\\d+"), "/:id");
        std::unique_lock<std::shared_mutex> lock(rw_);  // writer: exclusive access
        auto& r = routes_[key];
        r.count++;
        r.totalMs += ms;
        r.maxMs = std::max(r.maxMs, ms);
        if (status >= 400) r.errors++;
        threads_[std::this_thread::get_id()]++;
    }
    json snapshot() const {
        std::shared_lock<std::shared_mutex> lock(rw_);  // reader: shared access
        json routes = json::array();
        for (const auto& [k, r] : routes_)
            routes.push_back({{"route", k}, {"count", r.count}, {"errors", r.errors},
                              {"avg_ms", r.count ? r.totalMs / r.count : 0.0}, {"max_ms", r.maxMs}});
        std::sort(routes.begin(), routes.end(), [](const json& a, const json& b) { return a["count"] > b["count"]; });
        json workers = json::array();
        int n = 1;
        for (const auto& [id, count] : threads_) {
            std::ostringstream os;
            os << id;
            workers.push_back({{"worker", "Worker " + std::to_string(n++)}, {"native_id", os.str()}, {"requests_served", count}});
        }
        return {{"total_requests", total_.load()}, {"in_flight", inFlight_.load()}, {"peak_in_flight", peak_.load()},
                {"routes", routes}, {"worker_threads_seen", threads_.size()}, {"workers", workers}};
    }
private:
    struct RouteStat { long long count = 0, errors = 0; double totalMs = 0, maxMs = 0; };
    std::atomic<long long> total_{0}, inFlight_{0}, peak_{0};
    mutable std::shared_mutex rw_;
    std::map<string, RouteStat> routes_;
    std::map<std::thread::id, long long> threads_;
};
inline ServerMetrics& metrics() {
    static ServerMetrics m;
    return m;
}

// Status of the background auto-release thread (a daemon-style worker in main.cpp).
struct TimerThreadStats {
    std::atomic<bool> running{false};
    std::atomic<long long> wakeups{0}, autoReleases{0}, lastWakeEpochMs{0}, startedEpochMs{0};
    std::atomic<int> intervalSeconds{15};
};
inline TimerThreadStats& timerStats() {
    static TimerThreadStats s;
    return s;
}

inline long long processId() { return static_cast<long long>(OSLAB_GETPID()); }
inline long long& startEpochMs() {
    static long long t = epochMs();
    return t;
}

// =====================================================================================
// 1. Race condition: unsynchronised vs mutex vs atomic counters
// =====================================================================================
inline json raceCondition(const json& p) {
    int threads = intParam(p, "threads", 4, 2, 16);
    int increments = intParam(p, "increments", 50000, 1000, 200000);
    long long expected = 1LL * threads * increments;
    EventLog log;

    auto runPhase = [&](std::function<void()> body) {
        auto t0 = Clock::now();
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i) pool.emplace_back(body);
        for (auto& t : pool) t.join();
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };

    // (a) No synchronisation: read, then write back +1. Two threads can read the same
    //     value, so one update is lost. (Done on an atomic with separate load/store so the
    //     lost updates are real but the program has no undefined behaviour.)
    std::atomic<long long> unsafeCounter{0};
    double unsafeMs = runPhase([&] {
        for (int i = 0; i < increments; ++i) {
            long long v = unsafeCounter.load(std::memory_order_relaxed);
            if ((i & 63) == 0) std::this_thread::yield();  // widen the race window
            unsafeCounter.store(v + 1, std::memory_order_relaxed);
        }
    });
    log.add("main", "Unsynchronised: expected " + std::to_string(expected) + ", got " + std::to_string(unsafeCounter.load()));

    // (b) Mutex: the read-modify-write becomes a critical section.
    long long mutexCounter = 0;
    std::mutex m;
    double mutexMs = runPhase([&] {
        for (int i = 0; i < increments; ++i) {
            std::lock_guard<std::mutex> lock(m);
            ++mutexCounter;
        }
    });
    log.add("main", "std::mutex: got " + std::to_string(mutexCounter));

    // (c) Atomic hardware instruction (fetch_add): no lock needed.
    std::atomic<long long> atomicCounter{0};
    double atomicMs = runPhase([&] {
        for (int i = 0; i < increments; ++i) atomicCounter.fetch_add(1);
    });
    log.add("main", "std::atomic fetch_add: got " + std::to_string(atomicCounter.load()));

    json rows = json::array({
        {{"method", "No synchronisation"}, {"expected", expected}, {"actual", unsafeCounter.load()},
         {"lost_updates", expected - unsafeCounter.load()}, {"time_ms", unsafeMs}},
        {{"method", "std::mutex (critical section)"}, {"expected", expected}, {"actual", mutexCounter},
         {"lost_updates", expected - mutexCounter}, {"time_ms", mutexMs}},
        {{"method", "std::atomic (fetch_add)"}, {"expected", expected}, {"actual", atomicCounter.load()},
         {"lost_updates", expected - atomicCounter.load()}, {"time_ms", atomicMs}},
    });
    return {{"summary", {{"threads", threads}, {"increments_per_thread", increments}, {"expected", expected},
                         {"lost_updates_without_lock", expected - unsafeCounter.load()}}},
            {"tables", json::array({{{"title", "Counter results"}, {"rows", rows}}})},
            {"log", log.toJson()}};
}

// =====================================================================================
// 2. Deadlock: hold-and-wait broken with try_lock, circular wait broken with lock ordering
// =====================================================================================
inline json deadlockDemo(const json&) {
    EventLog log;
    std::mutex bed, ventilator;
    std::atomic<int> backoffs{0};

    // Part A — opposite lock order (the classic deadlock recipe). try_lock means a thread
    // that cannot get its second resource RELEASES the first one (breaks hold-and-wait).
    auto opposite = [&](const string& name, std::mutex& first, const string& firstName, std::mutex& second,
                        const string& secondName, int delayMs) {
        for (int attempt = 1; attempt <= 10; ++attempt) {
            std::unique_lock<std::mutex> a(first);
            log.add(name, "locked " + firstName + " (attempt " + std::to_string(attempt) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            std::unique_lock<std::mutex> b(second, std::defer_lock);
            if (b.try_lock()) {
                log.add(name, "locked " + secondName + " too -> treating patient, then releasing both");
                std::this_thread::sleep_for(10ms);
                return;
            }
            ++backoffs;
            log.add(name, secondName + " is held by the other thread -> release " + firstName + " and back off");
            a.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(15 * attempt + delayMs));  // back-off
        }
        log.add(name, "gave up after 10 attempts");
    };
    log.add("main", "Part A: Thread 1 locks Bed then Ventilator, Thread 2 locks Ventilator then Bed");
    std::thread t1(opposite, "Thread 1", std::ref(bed), "Bed", std::ref(ventilator), "Ventilator", 40);
    std::thread t2(opposite, "Thread 2", std::ref(ventilator), "Ventilator", std::ref(bed), "Bed", 40);
    t1.join();
    t2.join();

    // Part B — global lock ordering (std::scoped_lock uses a deadlock-avoidance algorithm,
    // and /api/allocations always locks patient -> bed -> staff -> equipment).
    log.add("main", "Part B: both threads use the same global order (std::scoped_lock)");
    auto ordered = [&](const string& name) {
        std::scoped_lock lock(bed, ventilator);
        log.add(name, "acquired Bed + Ventilator together, no circular wait possible");
        std::this_thread::sleep_for(20ms);
    };
    std::thread t3(ordered, "Thread 3"), t4(ordered, "Thread 4");
    t3.join();
    t4.join();

    json conditions = json::array({
        {{"condition", "Mutual exclusion"}, {"in_hospital", "A bed / staff member serves one patient"}, {"how_we_handle_it", "Unavoidable (resource is non-shareable)"}},
        {{"condition", "Hold and wait"}, {"in_hospital", "Holding a bed while waiting for a ventilator"}, {"how_we_handle_it", "try_lock: release what you hold and retry (Part A)"}},
        {{"condition", "No preemption"}, {"in_hospital", "Cannot take a bed away mid-treatment"}, {"how_we_handle_it", "Timer thread releases resources when the severity timer expires"}},
        {{"condition", "Circular wait"}, {"in_hospital", "T1 waits for T2's ventilator, T2 waits for T1's bed"}, {"how_we_handle_it", "Global lock order patient -> bed -> staff -> equipment (Part B + /api/allocations)"}},
    });
    return {{"summary", {{"backoffs_in_part_a", backoffs.load()}, {"deadlocks", 0}, {"all_threads_finished", true}}},
            {"tables", json::array({{{"title", "Coffman conditions"}, {"rows", conditions}}})},
            {"log", log.toJson()}};
}

// =====================================================================================
// 3. Starvation and bounded waiting with a ticket lock (FIFO fairness)
// =====================================================================================
inline json starvationDemo(const json& p) {
    int workers = intParam(p, "workers", 5, 2, 12);
    EventLog log;
    TicketLock lock;
    std::mutex orderMutex;
    std::vector<json> order;
    std::vector<std::thread> pool;
    for (int i = 1; i <= workers; ++i) {
        pool.emplace_back([&, i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(3 * i));  // arrive in order
            log.add("Nurse " + std::to_string(i), "requests the triage desk");
            int ticket = lock.lock();
            log.add("Nurse " + std::to_string(i), "served with ticket " + std::to_string(ticket));
            {
                std::lock_guard<std::mutex> g(orderMutex);
                order.push_back({{"turn", order.size() + 1}, {"thread", "Nurse " + std::to_string(i)}, {"ticket", ticket}});
            }
            std::this_thread::sleep_for(15ms);
            lock.unlock();
        });
    }
    for (auto& t : pool) t.join();
    return {{"summary", {{"threads", workers}, {"all_served", static_cast<int>(order.size()) == workers},
                         {"max_threads_ahead_of_anyone", workers - 1}}},
            {"tables", json::array({{{"title", "Service order (FIFO by ticket)"}, {"rows", order}}})},
            {"log", log.toJson()}};
}

// =====================================================================================
// 4. Producer-consumer (bounded buffer) with semaphores
// =====================================================================================
inline json producerConsumer(const json& p) {
    int producers = intParam(p, "producers", 2, 1, 6);
    int consumers = intParam(p, "consumers", 2, 1, 6);
    int capacity = intParam(p, "capacity", 3, 1, 10);
    int items = intParam(p, "patients", 10, 1, 40);
    EventLog log;
    Semaphore emptySlots(capacity), fullSlots(0);
    std::mutex bufferMutex;
    std::deque<int> bay;  // the triage bay (bounded buffer)
    int maxOccupancy = 0;
    std::atomic<int> producerWaits{0}, consumerWaits{0};
    std::map<string, int> treated;

    auto put = [&](int value, const string& who) {
        if (!emptySlots.tryWait()) {
            ++producerWaits;
            log.add(who, "triage bay FULL -> waiting on semaphore 'empty'");
            emptySlots.wait();
        }
        {
            std::lock_guard<std::mutex> g(bufferMutex);
            bay.push_back(value);
            maxOccupancy = std::max<int>(maxOccupancy, static_cast<int>(bay.size()));
            if (value > 0) log.add(who, "drops patient #" + std::to_string(value) + " (bay " + std::to_string(bay.size()) + "/" + std::to_string(capacity) + ")");
        }
        fullSlots.signal();
    };

    std::vector<std::thread> threads;
    std::atomic<int> nextPatient{1};
    for (int i = 1; i <= producers; ++i)
        threads.emplace_back([&, i] {
            string who = "Ambulance " + std::to_string(i);
            for (;;) {
                int id = nextPatient.fetch_add(1);
                if (id > items) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5 + 7 * i));
                put(id, who);
            }
        });
    for (int i = 1; i <= consumers; ++i)
        threads.emplace_back([&, i] {
            string who = "Doctor " + std::to_string(i);
            for (;;) {
                if (!fullSlots.tryWait()) {
                    ++consumerWaits;
                    fullSlots.wait();  // sleeps until a producer signals
                }
                int value;
                {
                    std::lock_guard<std::mutex> g(bufferMutex);
                    value = bay.front();
                    bay.pop_front();
                }
                emptySlots.signal();
                if (value < 0) break;  // poison pill: no more patients
                log.add(who, "treats patient #" + std::to_string(value));
                {
                    std::lock_guard<std::mutex> g(bufferMutex);
                    treated[who]++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25 + 10 * i));
            }
        });
    for (int i = 0; i < producers; ++i) threads[i].join();
    for (int i = 0; i < consumers; ++i) put(-1, "main");  // one poison pill per doctor
    for (size_t i = producers; i < threads.size(); ++i) threads[i].join();

    json rows = json::array();
    int total = 0;
    for (const auto& [who, n] : treated) { rows.push_back({{"doctor", who}, {"patients_treated", n}}); total += n; }
    return {{"summary", {{"patients", items}, {"patients_treated", total}, {"bay_capacity", capacity},
                         {"max_bay_occupancy", maxOccupancy}, {"capacity_never_exceeded", maxOccupancy <= capacity},
                         {"producer_waits_bay_full", producerWaits.load()}, {"consumer_waits_bay_empty", consumerWaits.load()}}},
            {"tables", json::array({{{"title", "Work done by consumer threads"}, {"rows", rows}}})},
            {"log", log.toJson()}};
}

// =====================================================================================
// 5. Readers-writers with std::shared_mutex
// =====================================================================================
inline json readersWriters(const json& p) {
    int readers = intParam(p, "readers", 4, 1, 10);
    int writers = intParam(p, "writers", 2, 1, 5);
    EventLog log;
    std::shared_mutex rw;
    std::atomic<int> activeReaders{0}, activeWriters{0}, maxReaders{0};
    std::atomic<int> violations{0}, reads{0}, writes{0};
    int bedsAvailable = 20;  // the shared data

    std::vector<std::thread> threads;
    for (int i = 1; i <= readers; ++i)
        threads.emplace_back([&, i] {
            string who = "Dashboard " + std::to_string(i);
            for (int k = 0; k < 3; ++k) {
                std::shared_lock<std::shared_mutex> lock(rw);  // many readers at once
                int now = ++activeReaders;
                int m = maxReaders.load();
                while (now > m && !maxReaders.compare_exchange_weak(m, now)) {}
                if (activeWriters.load() != 0) ++violations;
                log.add(who, "reads beds_available=" + std::to_string(bedsAvailable) + " (" + std::to_string(now) + " readers inside)");
                ++reads;
                std::this_thread::sleep_for(25ms);
                --activeReaders;
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5 * i));
            }
        });
    for (int i = 1; i <= writers; ++i)
        threads.emplace_back([&, i] {
            string who = "Clerk " + std::to_string(i);
            for (int k = 0; k < 2; ++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20 * i + 10 * k));
                std::unique_lock<std::shared_mutex> lock(rw);  // exclusive
                ++activeWriters;
                if (activeReaders.load() != 0 || activeWriters.load() != 1) ++violations;
                bedsAvailable -= 1;
                log.add(who, "WRITES beds_available=" + std::to_string(bedsAvailable) + " (exclusive, 0 readers inside)");
                ++writes;
                std::this_thread::sleep_for(15ms);
                --activeWriters;
            }
        });
    for (auto& t : threads) t.join();
    return {{"summary", {{"reads", reads.load()}, {"writes", writes.load()}, {"max_concurrent_readers", maxReaders.load()},
                         {"exclusion_violations", violations.load()}, {"final_beds_available", bedsAvailable}}},
            {"log", log.toJson()}};
}

// =====================================================================================
// 6. Dining philosophers (surgeons sharing instrument trays)
// =====================================================================================
inline json diningPhilosophers(const json& p) {
    int n = intParam(p, "surgeons", 5, 3, 8);
    int meals = intParam(p, "operations", 2, 1, 5);
    EventLog log;
    std::vector<std::timed_mutex> trays(n);
    std::atomic<int> stuck{0};

    // Naive attempt: everyone picks up the LEFT tray, then waits for the RIGHT one.
    // A barrier makes sure all surgeons hold their left tray before anyone reaches right,
    // and a second barrier keeps the trays held until everyone has tried. Timed locks
    // let us DETECT the circular wait instead of hanging forever.
    log.add("main", "Naive: every surgeon takes the left tray first");
    {
        std::mutex bm;
        std::condition_variable bcv;
        int holding = 0, tried = 0;
        auto barrier = [&](int& counter) {
            std::unique_lock<std::mutex> lk(bm);
            if (++counter == n) bcv.notify_all();
            else bcv.wait(lk, [&] { return counter == n; });
        };
        std::vector<std::thread> ts;
        for (int i = 0; i < n; ++i)
            ts.emplace_back([&, i] {
                string who = "Surgeon " + std::to_string(i + 1);
                int left = i, right = (i + 1) % n;
                std::unique_lock<std::timed_mutex> a(trays[left]);
                log.add(who, "holds tray " + std::to_string(left + 1));
                barrier(holding);  // everyone now holds exactly one tray
                std::unique_lock<std::timed_mutex> b(trays[right], std::defer_lock);
                bool got = b.try_lock_for(200ms);
                if (!got) {
                    ++stuck;
                    log.add(who, "waited 200 ms for tray " + std::to_string(right + 1) + " (held by Surgeon " +
                                     std::to_string(right + 1) + ") -> circular wait");
                } else {
                    log.add(who, "got both trays");
                }
                barrier(tried);
                if (got) b.unlock();
                a.unlock();
            });
        for (auto& t : ts) t.join();
    }
    bool deadlockDetected = stuck.load() == n;
    log.add("main", deadlockDetected ? "DEADLOCK detected: every surgeon holds one tray and waits for the next (cycle) -> all release (recovery)"
                                     : "No full cycle this time");

    // Fix: resource hierarchy — always pick the LOWER-numbered tray first.
    log.add("main", "Fixed: every surgeon takes the lower-numbered tray first");
    std::vector<int> done(n, 0);
    {
        std::vector<std::thread> ts;
        for (int i = 0; i < n; ++i)
            ts.emplace_back([&, i] {
                string who = "Surgeon " + std::to_string(i + 1);
                int first = std::min(i, (i + 1) % n), second = std::max(i, (i + 1) % n);
                for (int k = 0; k < meals; ++k) {
                    std::unique_lock<std::timed_mutex> a(trays[first]);
                    std::unique_lock<std::timed_mutex> b(trays[second]);
                    log.add(who, "operates with trays " + std::to_string(first + 1) + " + " + std::to_string(second + 1));
                    std::this_thread::sleep_for(15ms);
                    done[i]++;
                }
            });
        for (auto& t : ts) t.join();
    }
    json rows = json::array();
    for (int i = 0; i < n; ++i) rows.push_back({{"surgeon", "Surgeon " + std::to_string(i + 1)}, {"operations_done", done[i]}});
    return {{"summary", {{"surgeons", n}, {"naive_deadlock_detected", deadlockDetected}, {"surgeons_stuck_in_naive", stuck.load()},
                         {"operations_after_fix", std::accumulate(done.begin(), done.end(), 0)}}},
            {"tables", json::array({{{"title", "Resource-hierarchy solution"}, {"rows", rows}}})},
            {"log", log.toJson()}};
}

// =====================================================================================
// 7. Banker's algorithm (deadlock avoidance): safety + resource-request algorithm
// =====================================================================================
struct BankersInput {
    std::vector<string> resources;
    std::vector<string> processes;
    std::vector<int> available;
    std::vector<std::vector<int>> max, allocation;
    string source;
};

inline BankersInput sampleBankers() {
    return {{"Beds", "Staff", "Equipment"},
            {"P0", "P1", "P2", "P3", "P4"},
            {3, 3, 2},
            {{7, 5, 3}, {3, 2, 2}, {9, 0, 2}, {2, 2, 2}, {4, 3, 3}},
            {{0, 1, 0}, {2, 0, 0}, {3, 0, 2}, {2, 1, 1}, {0, 0, 2}},
            "Textbook sample (Silberschatz)"};
}

inline string vecStr(const std::vector<int>& v) {
    string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}

// Safety algorithm. Returns the safe sequence (empty if unsafe) and writes steps to log.
inline std::vector<int> safetyCheck(const std::vector<int>& available, const std::vector<std::vector<int>>& need,
                                    const std::vector<std::vector<int>>& alloc, const std::vector<string>& names,
                                    EventLog& log) {
    size_t n = need.size(), m = available.size();
    std::vector<int> work = available, seq;
    std::vector<bool> finish(n, false);
    log.add("Banker", "Work = Available = " + vecStr(work));
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < n; ++i) {
            if (finish[i]) continue;
            bool fits = true;
            for (size_t j = 0; j < m; ++j) fits = fits && need[i][j] <= work[j];
            if (!fits) continue;
            for (size_t j = 0; j < m; ++j) work[j] += alloc[i][j];
            finish[i] = true;
            seq.push_back(static_cast<int>(i));
            progress = true;
            log.add("Banker", names[i] + " Need " + vecStr(need[i]) + " <= Work -> runs, returns " + vecStr(alloc[i]) + ", Work = " + vecStr(work));
        }
    }
    if (seq.size() != n) {
        string stuck;
        for (size_t i = 0; i < n; ++i) if (!finish[i]) stuck += (stuck.empty() ? "" : ", ") + names[i];
        log.add("Banker", "UNSAFE: " + stuck + " can never finish with the remaining resources");
        return {};
    }
    return seq;
}

inline json bankers(const BankersInput& in, const json& p) {
    EventLog log;
    size_t n = in.processes.size(), m = in.resources.size();
    std::vector<std::vector<int>> need(n, std::vector<int>(m));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < m; ++j) need[i][j] = std::max(0, in.max[i][j] - in.allocation[i][j]);

    auto matrixRows = [&](const std::vector<std::vector<int>>& mat) {
        json rows = json::array();
        for (size_t i = 0; i < n; ++i) {
            json r = {{"process", in.processes[i]}};
            for (size_t j = 0; j < m; ++j) r[in.resources[j]] = mat[i][j];
            rows.push_back(r);
        }
        return rows;
    };
    json availRow = json::object();
    for (size_t j = 0; j < m; ++j) availRow[in.resources[j]] = in.available[j];

    log.add("Banker", "Data source: " + in.source);
    std::vector<int> seq = n ? safetyCheck(in.available, need, in.allocation, in.processes, log) : std::vector<int>{};
    bool safe = n == 0 || !seq.empty();
    json seqNames = json::array();
    for (int i : seq) seqNames.push_back(in.processes[i]);

    json summary = {{"source", in.source}, {"processes", n}, {"state", safe ? "SAFE" : "UNSAFE"},
                    {"safe_sequence", seqNames}};

    // Optional resource request: "request_process" (index) + "request" ("1,0,2").
    string req = strParam(p, "request", "");
    if (!req.empty() && n > 0) {
        int pi = intParam(p, "request_process", 0, 0, static_cast<int>(n) - 1);
        std::vector<int> r;
        std::stringstream ss(req);
        string part;
        while (std::getline(ss, part, ',')) {
            try { r.push_back(std::stoi(part)); } catch (...) { throw BadInput{"Request must look like 1,0,2"}; }
        }
        if (r.size() != m) throw BadInput{"Request needs " + std::to_string(m) + " numbers (one per resource)."};
        log.add("Banker", "Request from " + in.processes[pi] + ": " + vecStr(r));
        string verdict;
        bool ok = true;
        for (size_t j = 0; j < m && ok; ++j) if (r[j] > need[pi][j]) { ok = false; verdict = "DENIED: request exceeds the process's declared maximum need"; }
        for (size_t j = 0; j < m && ok; ++j) if (r[j] > in.available[j]) { ok = false; verdict = "WAIT: not enough resources available right now"; }
        if (ok) {
            auto avail = in.available;
            auto alloc = in.allocation;
            auto need2 = need;
            for (size_t j = 0; j < m; ++j) { avail[j] -= r[j]; alloc[pi][j] += r[j]; need2[pi][j] -= r[j]; }
            log.add("Banker", "Pretend to grant -> Available = " + vecStr(avail) + ", re-running safety check");
            auto s2 = safetyCheck(avail, need2, alloc, in.processes, log);
            verdict = s2.empty() ? "DENIED: granting would leave the system UNSAFE (possible deadlock)"
                                 : "GRANTED: the system stays in a safe state";
        }
        log.add("Banker", verdict);
        summary["request_result"] = verdict;
    }

    return {{"summary", summary},
            {"tables", json::array({
                {{"title", "Available"}, {"rows", json::array({availRow})}},
                {{"title", "Allocation"}, {"rows", matrixRows(in.allocation)}},
                {{"title", "Max"}, {"rows", matrixRows(in.max)}},
                {{"title", "Need = Max - Allocation"}, {"rows", matrixRows(need)}},
            })},
            {"log", log.toJson()}};
}

// =====================================================================================
// 8. CPU scheduling on the emergency queue: FCFS, SJF, Priority, Round Robin, Priority+Aging
// =====================================================================================
struct Proc {
    string id, name;
    int arrival = 0, burst = 1, priority = 4;  // priority 1 = Critical (highest)
};

inline std::vector<Proc> sampleProcesses() {
    // A Low-priority patient arrives early while Critical / High patients keep arriving:
    // Priority-only scheduling starves them; aging fixes it.
    return {{"P1", "Critical A", 0, 6, 1}, {"P2", "Low B", 1, 3, 4}, {"P3", "High C", 2, 5, 2},
            {"P4", "Medium D", 3, 2, 3},   {"P5", "Critical E", 5, 4, 1}, {"P6", "High F", 8, 5, 2},
            {"P7", "Critical G", 12, 4, 1}, {"P8", "High H", 16, 3, 2}};
}

inline json schedulingResult(const string& name, const string& how, const std::vector<Proc>& procs,
                             std::vector<json> gantt) {
    // Merge neighbouring slices of the same process (for Round Robin).
    std::vector<json> merged;
    for (auto& g : gantt) {
        if (!merged.empty() && merged.back()["id"] == g["id"] && merged.back()["end"] == g["start"]) merged.back()["end"] = g["end"];
        else merged.push_back(g);
    }
    std::map<string, int> first, completion;
    for (auto& g : merged) {
        string id = g["id"];
        if (id == "idle") continue;
        if (!first.count(id)) first[id] = g["start"];
        completion[id] = g["end"];
    }
    json rows = json::array();
    double totalWait = 0, totalTat = 0;
    int maxWait = 0, lowWait = -1, lowestPriority = 0;
    for (auto& pr : procs) lowestPriority = std::max(lowestPriority, pr.priority);
    for (auto& pr : procs) {
        int tat = completion[pr.id] - pr.arrival;
        int wait = tat - pr.burst;
        totalWait += wait;
        totalTat += tat;
        maxWait = std::max(maxWait, wait);
        if (pr.priority == lowestPriority) lowWait = std::max(lowWait, wait);
        rows.push_back({{"process", pr.id}, {"patient", pr.name}, {"priority", pr.priority}, {"arrival", pr.arrival},
                        {"burst", pr.burst}, {"start", first[pr.id]}, {"completion", completion[pr.id]},
                        {"waiting", wait}, {"turnaround", tat}, {"response", first[pr.id] - pr.arrival}});
    }
    double n = procs.empty() ? 1 : static_cast<double>(procs.size());
    return {{"name", name}, {"how", how}, {"gantt", merged}, {"rows", rows},
            {"avg_waiting", totalWait / n}, {"avg_turnaround", totalTat / n}, {"max_waiting", maxWait},
            {"lowest_priority_wait", lowWait}};
}

// Non-preemptive scheduler; `better(a, b, now)` decides which ready process runs first.
inline std::vector<json> runNonPreemptive(const std::vector<Proc>& procs,
                                          std::function<bool(const Proc&, const Proc&, int)> better) {
    std::vector<json> gantt;
    std::vector<bool> done(procs.size(), false);
    int t = 0;
    for (size_t finished = 0; finished < procs.size();) {
        int pick = -1;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (done[i] || procs[i].arrival > t) continue;
            if (pick < 0 || better(procs[i], procs[pick], t)) pick = static_cast<int>(i);
        }
        if (pick < 0) {  // CPU idle until the next arrival
            int next = INT32_MAX;
            for (size_t i = 0; i < procs.size(); ++i) if (!done[i]) next = std::min(next, procs[i].arrival);
            gantt.push_back({{"id", "idle"}, {"name", "Idle"}, {"start", t}, {"end", next}});
            t = next;
            continue;
        }
        gantt.push_back({{"id", procs[pick].id}, {"name", procs[pick].name}, {"start", t}, {"end", t + procs[pick].burst}});
        t += procs[pick].burst;
        done[pick] = true;
        ++finished;
    }
    return gantt;
}

inline std::vector<json> runRoundRobin(const std::vector<Proc>& procsIn, int quantum) {
    std::vector<Proc> procs = procsIn;
    std::vector<size_t> order(procs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return procs[a].arrival < procs[b].arrival; });
    std::vector<int> remaining(procs.size());
    for (size_t i = 0; i < procs.size(); ++i) remaining[i] = procs[i].burst;
    std::deque<size_t> ready;
    std::vector<json> gantt;
    size_t next = 0;
    int t = 0;
    auto admit = [&](int upTo) { while (next < order.size() && procs[order[next]].arrival <= upTo) ready.push_back(order[next++]); };
    admit(t);
    while (!ready.empty() || next < order.size()) {
        if (ready.empty()) {
            int arr = procs[order[next]].arrival;
            gantt.push_back({{"id", "idle"}, {"name", "Idle"}, {"start", t}, {"end", arr}});
            t = arr;
            admit(t);
            continue;
        }
        size_t i = ready.front();
        ready.pop_front();
        int run = std::min(quantum, remaining[i]);
        gantt.push_back({{"id", procs[i].id}, {"name", procs[i].name}, {"start", t}, {"end", t + run}});
        t += run;
        remaining[i] -= run;
        admit(t);  // new arrivals join before the preempted process
        if (remaining[i] > 0) ready.push_back(i);
    }
    return gantt;
}

inline json scheduling(std::vector<Proc> procs, const string& source, const json& p) {
    int quantum = intParam(p, "quantum", 4, 1, 500);
    int aging = intParam(p, "aging_interval", 5, 1, 500);
    if (procs.empty()) throw BadInput{"No processes to schedule."};

    auto byArrival = [](const Proc& a, const Proc& b) { return a.arrival != b.arrival ? a.arrival < b.arrival : a.id < b.id; };
    json algos = json::array();
    algos.push_back(schedulingResult("FCFS", "First come, first served (non-preemptive)", procs,
        runNonPreemptive(procs, [&](const Proc& a, const Proc& b, int) { return byArrival(a, b); })));
    algos.push_back(schedulingResult("SJF", "Shortest job first (non-preemptive)", procs,
        runNonPreemptive(procs, [&](const Proc& a, const Proc& b, int) { return a.burst != b.burst ? a.burst < b.burst : byArrival(a, b); })));
    algos.push_back(schedulingResult("Priority", "Highest clinical priority first (non-preemptive) — same order as GET /api/queue", procs,
        runNonPreemptive(procs, [&](const Proc& a, const Proc& b, int) { return a.priority != b.priority ? a.priority < b.priority : byArrival(a, b); })));
    algos.push_back(schedulingResult("Round Robin", "Time quantum " + std::to_string(quantum) + " (preemptive)", procs, runRoundRobin(procs, quantum)));
    algos.push_back(schedulingResult("Priority + Aging", "Priority improves by 1 for every " + std::to_string(aging) + " time units waited — prevents starvation", procs,
        runNonPreemptive(procs, [&](const Proc& a, const Proc& b, int now) {
            int ea = a.priority - (now - a.arrival) / aging, eb = b.priority - (now - b.arrival) / aging;
            return ea != eb ? ea < eb : byArrival(a, b);
        })));

    json best = algos[0];
    for (auto& a : algos) if (a["avg_waiting"].get<double>() < best["avg_waiting"].get<double>()) best = a;
    return {{"summary", {{"source", source}, {"processes", procs.size()}, {"quantum", quantum}, {"aging_interval", aging},
                         {"lowest_avg_waiting", best["name"]},
                         {"low_priority_wait_priority_only", algos[2]["lowest_priority_wait"]},
                         {"low_priority_wait_with_aging", algos[4]["lowest_priority_wait"]}}},
            {"schedules", algos}};
}

// =====================================================================================
// Catalog shown on the OS Concepts page (parameters become input fields in React)
// =====================================================================================
inline json numberParam(const string& key, const string& label, int def, int lo, int hi) {
    return {{"key", key}, {"label", label}, {"type", "number"}, {"default", def}, {"min", lo}, {"max", hi}};
}

inline json catalog() {
    return json::array({
        {{"id", "race_condition"}, {"title", "Race condition & critical section"}, {"category", "Process synchronization"},
         {"description", "Several threads increment one shared counter. Without a lock, updates are lost. A std::mutex (critical section) or a std::atomic instruction fixes it."},
         {"where_used", "All live counters in this server are std::atomic; the connection pool's free list is guarded by a std::mutex."},
         {"params", json::array({numberParam("threads", "Threads", 4, 2, 16), numberParam("increments", "Increments per thread", 50000, 1000, 200000)})}},
        {{"id", "producer_consumer"}, {"title", "Producer-consumer with semaphores"}, {"category", "Process synchronization"},
         {"description", "Ambulance threads (producers) drop patients into a bounded triage bay; doctor threads (consumers) take them out. Counting semaphores 'empty' and 'full' block threads when the bay is full or empty."},
         {"where_used", "ConnectionPool works the same way: requests block when all DB connections are in use."},
         {"params", json::array({numberParam("producers", "Ambulances", 2, 1, 6), numberParam("consumers", "Doctors", 2, 1, 6),
                                 numberParam("capacity", "Bay capacity", 3, 1, 10), numberParam("patients", "Patients", 10, 1, 40)})}},
        {{"id", "readers_writers"}, {"title", "Readers-writers lock"}, {"category", "Process synchronization"},
         {"description", "Dashboards (readers) can read bed counts at the same time; clerks (writers) need exclusive access. Implemented with std::shared_mutex."},
         {"where_used", "ServerMetrics: every request writes route stats, /api/os/live reads them under a shared lock."},
         {"params", json::array({numberParam("readers", "Readers", 4, 1, 10), numberParam("writers", "Writers", 2, 1, 5)})}},
        {{"id", "deadlock"}, {"title", "Deadlock prevention"}, {"category", "Deadlocks"},
         {"description", "Two threads lock Bed and Ventilator in opposite order. try_lock breaks hold-and-wait; a global lock order (std::scoped_lock) breaks circular wait."},
         {"where_used", "POST /api/allocations locks patient -> bed -> staff -> equipment; releases update beds, staff, equipment in id order."},
         {"params", json::array()}},
        {{"id", "bankers"}, {"title", "Banker's algorithm (deadlock avoidance)"}, {"category", "Deadlocks"},
         {"description", "Checks whether the hospital is in a SAFE state: can every patient eventually get the beds, staff and equipment they may need? Optionally tests a new request before granting it."},
         {"where_used", "'Live hospital data' builds the Allocation and Available matrices from MySQL (resource_allocations, beds, staff, equipment)."},
         {"params", json::array({
             {{"key", "mode"}, {"label", "Data"}, {"type", "select"}, {"default", "live"},
              {"options", json::array({{{"value", "live"}, {"label", "Live hospital data"}}, {{"value", "sample"}, {"label", "Textbook sample"}}})}},
             numberParam("request_process", "Request from process #", 1, 0, 50),
             {{"key", "request"}, {"label", "Request (e.g. 1,0,2)"}, {"type", "text"}, {"default", ""}}})}},
        {{"id", "dining_philosophers"}, {"title", "Dining philosophers"}, {"category", "Deadlocks"},
         {"description", "Surgeons share instrument trays placed between them. If everyone grabs the left tray first, all wait forever (detected with timed locks). Taking the lower-numbered tray first fixes it."},
         {"where_used", "Same resource-hierarchy rule as the allocation lock order."},
         {"params", json::array({numberParam("surgeons", "Surgeons", 5, 3, 8), numberParam("operations", "Operations each", 2, 1, 5)})}},
        {{"id", "starvation"}, {"title", "Starvation & bounded waiting"}, {"category", "Scheduling"},
         {"description", "A ticket lock serves threads strictly in arrival order, so no thread can be overtaken forever (bounded waiting)."},
         {"where_used", "Connection pool waits time out after 10 s; the emergency queue breaks ties by arrival time."},
         {"params", json::array({numberParam("workers", "Threads", 5, 2, 12)})}},
        {{"id", "scheduling"}, {"title", "CPU scheduling on the emergency queue"}, {"category", "Scheduling"},
         {"description", "Treats each queued patient as a process (arrival = minutes since first arrival, burst = severity release timer). Compares FCFS, SJF, Priority, Round Robin and Priority with aging."},
         {"where_used", "GET /api/queue uses Priority scheduling (priority ASC, arrival ASC). Aging shows how low-priority patients avoid starvation."},
         {"params", json::array({
             {{"key", "mode"}, {"label", "Data"}, {"type", "select"}, {"default", "live"},
              {"options", json::array({{{"value", "live"}, {"label", "Live emergency queue"}}, {{"value", "sample"}, {"label", "Starvation sample"}}})}},
             numberParam("quantum", "RR quantum", 4, 1, 500), numberParam("aging_interval", "Aging interval", 5, 1, 500)})}},
        {{"id", "connection_pool"}, {"title", "Bounded resource pool (mutex + condition variable)"}, {"category", "Threads & resources"},
         {"description", "Starts more threads than there are MySQL connections. Extra threads sleep on a condition variable until a connection is returned."},
         {"where_used", "ConnectionPool::acquire() in db_pool.hpp — used by every API request."},
         {"params", json::array({numberParam("extra_threads", "Threads beyond pool size", 3, 1, 5)})}},
        {{"id", "timer_thread"}, {"title", "Background timer thread"}, {"category", "Threads & resources"},
         {"description", "A daemon-style std::thread wakes up periodically, finds allocations whose severity timer has expired and releases them. Stopped cleanly with a std::atomic<bool> flag."},
         {"where_used", "Auto-release thread in main.cpp; release_due_at column in resource_allocations."},
         {"params", json::array()}},
    });
}

}  // namespace oslab
