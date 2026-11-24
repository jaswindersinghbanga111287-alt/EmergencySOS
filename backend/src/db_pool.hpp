// A small, thread-safe MySQL connection pool + transaction runner.
//
// OS concepts used here (all visible live on the OS Concepts page):
//  - Mutual exclusion: a std::mutex guards the list of free connections (critical section).
//  - Synchronization: a std::condition_variable makes request threads sleep
//    (instead of busy-waiting) until a connection is returned.
//  - Bounded resources: at most `size` connections exist, like a counting semaphore.
//  - Bounded waiting: a waiting thread gives up after 10 s instead of starving forever.
//  - Each connection is used by only one thread at a time, because
//    sql::Connection objects are not safe to share between threads.
//  - Atomic counters record pool and transaction activity without extra locks.
//
// DBMS concepts used here:
//  - Transactions (COMMIT / ROLLBACK), atomicity.
//  - Deadlock recovery: InnoDB picks a victim (error 1213); we roll back and retry.
#pragma once
#include <mysql/jdbc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"

class ConnectionPool {
public:
    struct Stats {
        int size = 0, free = 0, inUse = 0, waiting = 0;
        long long acquires = 0, waits = 0, timeouts = 0, reconnects = 0, maxWaitMs = 0;
    };

    explicit ConnectionPool(const Config& cfg) : cfg_(cfg) {
        driver_ = sql::mysql::get_mysql_driver_instance();
        for (int i = 0; i < cfg_.dbPoolSize; ++i) free_.push_back(connect());
    }

    // RAII handle: the connection goes back to the pool automatically.
    class Lease {
    public:
        Lease(ConnectionPool* pool, std::unique_ptr<sql::Connection> con) : pool_(pool), con_(std::move(con)) {}
        Lease(Lease&& other) noexcept = default;
        ~Lease() { if (con_) pool_->release(std::move(con_)); }
        sql::Connection* operator->() { return con_.get(); }
        sql::Connection* get() { return con_.get(); }
    private:
        ConnectionPool* pool_;
        std::unique_ptr<sql::Connection> con_;
    };

    Lease acquire() {
        std::unique_ptr<sql::Connection> con;
        auto started = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (free_.empty()) {
                // No free connection: this thread blocks on the condition variable (no busy-waiting).
                ++waits_;
                ++waiting_;
                // Bounded waiting: give up after 10 seconds instead of blocking forever.
                bool ok = available_.wait_for(lock, std::chrono::seconds(10), [&] { return !free_.empty(); });
                --waiting_;
                if (!ok) {
                    ++timeouts_;
                    throw std::runtime_error("All database connections are busy. Try again.");
                }
            }
            con = std::move(free_.back());
            free_.pop_back();
            ++acquires_;
        }
        long long waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        long long prev = maxWaitMs_.load();
        while (waitedMs > prev && !maxWaitMs_.compare_exchange_weak(prev, waitedMs)) {}
        // Reconnect outside the lock so other threads are not held up.
        if (!con || con->isClosed() || !con->isValid()) {
            try { con = connect(); ++reconnects_; }
            catch (...) { release(nullptr); throw; }
        }
        return Lease(this, std::move(con));
    }

    Stats stats() const {
        Stats s;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s.free = static_cast<int>(free_.size());
        }
        s.size = cfg_.dbPoolSize;
        s.inUse = s.size - s.free;
        s.waiting = waiting_.load();
        s.acquires = acquires_.load();
        s.waits = waits_.load();
        s.timeouts = timeouts_.load();
        s.reconnects = reconnects_.load();
        s.maxWaitMs = maxWaitMs_.load();
        return s;
    }

    int size() const { return cfg_.dbPoolSize; }

private:
    std::unique_ptr<sql::Connection> connect() {
        std::string url = "tcp://" + cfg_.dbHost + ":" + std::to_string(cfg_.dbPort);
        std::unique_ptr<sql::Connection> con(driver_->connect(url, cfg_.dbUser, cfg_.dbPassword));
        con->setSchema(cfg_.dbName);
        std::unique_ptr<sql::Statement> st(con->createStatement());
        st->execute("SET NAMES utf8mb4");
        return con;
    }

    void release(std::unique_ptr<sql::Connection> con) {
        if (con) {
            try {
                if (!con->getAutoCommit()) { con->rollback(); con->setAutoCommit(true); }
            } catch (...) { con.reset(); }  // broken connection: replaced on next acquire
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(std::move(con));
        }
        available_.notify_one();  // wake exactly one waiting thread
    }

    Config cfg_;
    sql::mysql::MySQL_Driver* driver_;
    std::vector<std::unique_ptr<sql::Connection>> free_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::atomic<int> waiting_{0};
    std::atomic<long long> acquires_{0}, waits_{0}, timeouts_{0}, reconnects_{0}, maxWaitMs_{0};
};

// MySQL error codes for lock problems that are safe to retry.
constexpr int ER_LOCK_DEADLOCK = 1213;
constexpr int ER_LOCK_WAIT_TIMEOUT = 1205;

// Transaction statistics, shown live on the OS / DBMS pages.
struct TransactionStats {
    std::atomic<long long> started{0}, commits{0}, rollbacks{0}, deadlockRetries{0}, lockTimeoutRetries{0};
};
inline TransactionStats& txStats() {
    static TransactionStats s;
    return s;
}

// Runs `work` inside a transaction. On a deadlock or lock timeout the whole
// transaction is rolled back and retried (deadlock recovery), up to `attempts` times.
template <typename Fn>
auto withTransaction(ConnectionPool& pool, Fn work, int attempts = 3) -> decltype(work(std::declval<sql::Connection*>())) {
    for (int attempt = 1;; ++attempt) {
        auto con = pool.acquire();
        con->setAutoCommit(false);  // START TRANSACTION
        ++txStats().started;
        try {
            auto result = work(con.get());
            con->commit();
            ++txStats().commits;
            con->setAutoCommit(true);
            return result;
        } catch (sql::SQLException& e) {
            try { con->rollback(); con->setAutoCommit(true); } catch (...) {}
            ++txStats().rollbacks;
            bool deadlock = e.getErrorCode() == ER_LOCK_DEADLOCK;
            bool timeout = e.getErrorCode() == ER_LOCK_WAIT_TIMEOUT;
            if (!(deadlock || timeout) || attempt >= attempts) throw;
            if (deadlock) ++txStats().deadlockRetries;
            else ++txStats().lockTimeoutRetries;
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));  // small back-off
        } catch (...) {
            try { con->rollback(); con->setAutoCommit(true); } catch (...) {}
            ++txStats().rollbacks;
            throw;
        }
    }
}
