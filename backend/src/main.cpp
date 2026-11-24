// EmergencySOS backend: a pure C++ REST API on top of MySQL.
//
//   HTTP server : cpp-httplib (third_party/httplib.h, header only)
//   JSON        : nlohmann/json (third_party/json.hpp, header only)
//   Database    : MySQL Connector/C++ (JDBC API, same one used by cpp_console/)
//
//   OS concepts : os_concepts.hpp   (threads, locks, semaphores, deadlock, scheduling ...)
//   DBMS concepts: dbms_concepts.hpp (views, triggers, procedures, transactions, locking ...)
//
// The React frontend talks to these /api routes. The OS Concepts and DBMS Concepts pages
// call /api/os/* and /api/dbms/*, which run the concepts live inside this process.
#include <mysql/jdbc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "db_helpers.hpp"
#include "db_pool.hpp"
#include "dbms_concepts.hpp"
#include "httplib.h"
#include "json.hpp"
#include "os_concepts.hpp"

using json = nlohmann::ordered_json;  // keeps keys in insertion order (SQL column order)
using std::string;

// Shared DB helpers (ApiError, rowsToJson, query, execute ...) live in db_helpers.hpp.

// ---------------------------------------------------------------- request helpers

static json parseBody(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) throw ApiError{400, "Request body must be valid JSON."};
    return body;
}

// Reads an integer from JSON that may arrive as a number or a numeric string.
static std::optional<long long> optInt(const json& body, const string& key) {
    if (!body.contains(key) || body[key].is_null()) return std::nullopt;
    const json& v = body[key];
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (d != static_cast<long long>(d)) throw ApiError{400, key + " must be a whole number."};
        return static_cast<long long>(d);
    }
    if (v.is_string()) {
        string s = trim(v.get<string>());
        if (s.empty()) return std::nullopt;
        if (!std::regex_match(s, std::regex("-?\\d{1,15}"))) throw ApiError{400, key + " must be a whole number."};
        return std::stoll(s);
    }
    throw ApiError{400, key + " must be a whole number."};
}

static string optStr(const json& body, const string& key) {
    if (!body.contains(key) || body[key].is_null()) return "";
    if (body[key].is_string()) return trim(body[key].get<string>());
    if (body[key].is_number()) return body[key].dump();
    throw ApiError{400, key + " must be text."};
}

static long long parseIdParam(const string& raw, const string& what) {
    if (!std::regex_match(raw, std::regex("\\d{1,15}")) || std::stoll(raw) <= 0)
        throw ApiError{400, "Invalid " + what + "."};
    return std::stoll(raw);
}

// Treats missing, empty and 0 as "not selected".
static std::optional<long long> optionalResource(const json& body, const string& key, const string& label) {
    auto v = optInt(body, key);
    if (!v || *v == 0) return std::nullopt;
    if (*v < 0) throw ApiError{400, label + " ID must be zero or a positive number."};
    return v;
}

static void setOptionalInt(sql::PreparedStatement* st, int idx, const std::optional<long long>& v) {
    if (v) st->setInt64(idx, *v);
    else st->setNull(idx, sql::DataType::INTEGER);
}
static void setOptionalString(sql::PreparedStatement* st, int idx, const string& v) {
    if (v.empty()) st->setNull(idx, sql::DataType::VARCHAR);
    else st->setString(idx, v);
}

static void sendJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Wraps a route so every error becomes a clean JSON message for the frontend.
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
static Handler safe(Handler fn) {
    return [fn](const httplib::Request& req, httplib::Response& res) {
        try {
            fn(req, res);
        } catch (const ApiError& e) {
            sendJson(res, {{"message", e.message}}, e.status);
        } catch (const oslab::BadInput& e) {
            sendJson(res, {{"message", e.message}}, 400);
        } catch (const sql::SQLException& e) {
            std::cerr << "[DB error " << e.getErrorCode() << "] " << e.what() << "\n";
            if (e.getErrorCode() == 1062)
                sendJson(res, {{"message", "This patient identifier already exists."}}, 409);
            else if (e.getErrorCode() == 1452)
                sendJson(res, {{"message", "Referenced patient or resource does not exist."}}, 400);
            else
                sendJson(res, {{"message", "Server/database error. Check the backend terminal and database settings."}}, 500);
        } catch (const std::exception& e) {
            std::cerr << "[Server error] " << e.what() << "\n";
            sendJson(res, {{"message", e.what()}}, 500);
        }
    };
}

static json withPriorityLabels(json rows) {
    for (auto& r : rows) r["priority_label"] = priorityLabel(r["priority"].get<long long>());
    return rows;
}

// ---------------------------------------------------------------- release helpers

// Adds the release-timer columns to databases created before this feature existed.
static void ensureAllocationTimerColumns(ConnectionPool& db) {
    auto con = db.acquire();
    struct Column { const char* name; const char* ddl; };
    const Column columns[] = {
        {"priority", "ALTER TABLE resource_allocations ADD COLUMN priority INT NULL AFTER equipment_id"},
        {"release_due_at", "ALTER TABLE resource_allocations ADD COLUMN release_due_at TIMESTAMP NULL AFTER allocation_time"},
        {"release_type", "ALTER TABLE resource_allocations ADD COLUMN release_type VARCHAR(20) NULL AFTER release_time"},
    };
    for (const auto& c : columns) {
        bool exists = !query(con.get(),
            "SELECT 1 AS ok FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME='resource_allocations' AND COLUMN_NAME=?",
            [&](auto* st) { st->setString(1, c.name); }).empty();
        if (exists) continue;
        std::unique_ptr<sql::Statement> st(con->createStatement());
        st->execute(c.ddl);
        std::cout << "Database upgraded: added resource_allocations." << c.name << std::endl;
    }
}

// Returns the resources held by `allocs` and marks those allocations Released.
// Deadlock prevention: resources are always updated in the same global order —
// all beds (by id), then all staff (by id), then all equipment (by id).
static void returnResources(sql::Connection* con, const json& allocs, const string& releaseType) {
    std::vector<long long> beds, staff, equipment;
    for (const auto& a : allocs) {
        if (!a["bed_id"].is_null()) beds.push_back(a["bed_id"].get<long long>());
        if (!a["staff_id"].is_null()) staff.push_back(a["staff_id"].get<long long>());
        if (!a["equipment_id"].is_null()) equipment.push_back(a["equipment_id"].get<long long>());
    }
    std::sort(beds.begin(), beds.end());
    std::sort(staff.begin(), staff.end());
    std::sort(equipment.begin(), equipment.end());
    for (long long id : beds)
        execute(con, "UPDATE beds SET bed_status='Available' WHERE bed_id=? AND bed_status='Occupied'",
                [&](auto* st) { st->setInt64(1, id); });
    for (long long id : staff)
        execute(con, "UPDATE staff SET staff_status='Available' WHERE staff_id=? AND staff_status='Busy'",
                [&](auto* st) { st->setInt64(1, id); });
    for (long long id : equipment)
        execute(con, "UPDATE equipment SET available_quantity=LEAST(total_quantity, available_quantity+1), "
                     "equipment_status=IF(equipment_status='Maintenance','Maintenance','Available') WHERE equipment_id=?",
                [&](auto* st) { st->setInt64(1, id); });
    for (const auto& a : allocs)
        execute(con, "UPDATE resource_allocations SET allocation_status='Released', release_time=NOW(), release_type=? "
                     "WHERE allocation_id=?",
                [&](auto* st) {
                    st->setString(1, releaseType);
                    st->setInt64(2, a["allocation_id"].get<long long>());
                });
}

// Marks the patient's open queue entry as Completed. Returns false if there was none.
static bool completeActiveQueueEntry(sql::Connection* con, long long patientId) {
    json open = query(con, "SELECT queue_id FROM emergency_queue WHERE patient_id=? AND queue_status IN ('Waiting','In Treatment') FOR UPDATE",
                      [&](auto* st) { st->setInt64(1, patientId); });
    if (open.empty()) return false;
    execute(con, "UPDATE emergency_queue SET queue_status='Completed' WHERE patient_id=? AND queue_status IN ('Waiting','In Treatment')",
            [&](auto* st) { st->setInt64(1, patientId); });
    return true;
}

// Releases one allocation inside the caller's transaction and returns the patient ID.
// With `completeQueueIfLast`, the patient's queue entry is closed once nothing else is held.
static long long releaseOneAllocation(sql::Connection* con, long long allocationId, const string& releaseType,
                                      bool completeQueueIfLast) {
    json lookup = query(con, "SELECT patient_id FROM resource_allocations WHERE allocation_id=?",
                        [&](auto* st) { st->setInt64(1, allocationId); });
    if (lookup.empty()) throw ApiError{404, "Allocation not found."};
    long long patientId = lookup[0]["patient_id"];
    // Same lock order as allocation: patient first, then the allocation's resources.
    query(con, "SELECT patient_id FROM patients WHERE patient_id=? FOR UPDATE", [&](auto* st) { st->setInt64(1, patientId); });
    json rows = query(con, "SELECT allocation_id, bed_id, staff_id, equipment_id FROM resource_allocations "
                           "WHERE allocation_id=? AND allocation_status='Active' FOR UPDATE",
                      [&](auto* st) { st->setInt64(1, allocationId); });
    if (rows.empty()) throw ApiError{409, "Allocation is already released."};
    returnResources(con, rows, releaseType);
    if (completeQueueIfLast &&
        query(con, "SELECT allocation_id FROM resource_allocations WHERE patient_id=? AND allocation_status='Active' LIMIT 1",
              [&](auto* st) { st->setInt64(1, patientId); }).empty())
        completeActiveQueueEntry(con, patientId);
    return patientId;
}

static const char* ACTIVE_QUEUE_SQL =
    "SELECT q.queue_id, q.patient_id, p.name, q.priority, "
    "DATE_FORMAT(q.arrival_time, '%Y-%m-%d %H:%i:%s') AS arrival_time, q.queue_status "
    "FROM emergency_queue q JOIN patients p ON p.patient_id = q.patient_id "
    "WHERE q.queue_status IN ('Waiting','In Treatment') "
    "ORDER BY q.priority ASC, q.arrival_time ASC, q.queue_id ASC";

// ---------------------------------------------------------------- live OS concept inputs
// These build the OS demos' inputs from the real hospital data in MySQL.

// Banker's algorithm: processes = patients in the queue or holding resources,
// resources = beds, staff, equipment units. Max need comes from the patient's severity.
static oslab::BankersInput liveBankersInput(ConnectionPool& db) {
    auto con = db.acquire();
    json procs = query(con.get(),
        "SELECT p.patient_id, p.name, "
        "COALESCE((SELECT MIN(q.priority) FROM emergency_queue q WHERE q.patient_id = p.patient_id AND q.queue_status IN ('Waiting','In Treatment')), "
        "         (SELECT MIN(a.priority) FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active'), 4) AS priority, "
        "(SELECT COUNT(*) FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active' AND a.bed_id IS NOT NULL) AS beds_held, "
        "(SELECT COUNT(*) FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active' AND a.staff_id IS NOT NULL) AS staff_held, "
        "(SELECT COUNT(*) FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active' AND a.equipment_id IS NOT NULL) AS equipment_held "
        "FROM patients p WHERE EXISTS (SELECT 1 FROM emergency_queue q WHERE q.patient_id = p.patient_id AND q.queue_status IN ('Waiting','In Treatment')) "
        "OR EXISTS (SELECT 1 FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active') "
        "ORDER BY priority, p.patient_id LIMIT 15");
    if (procs.empty()) {
        auto in = oslab::sampleBankers();
        in.source = "Textbook sample (no patients are queued or holding resources right now)";
        return in;
    }
    oslab::BankersInput in;
    in.resources = {"Beds", "Staff", "Equipment"};
    in.available = {
        static_cast<int>(countOf(con.get(), "SELECT COUNT(*) AS n FROM beds WHERE bed_status = 'Available'")),
        static_cast<int>(countOf(con.get(), "SELECT COUNT(*) AS n FROM staff WHERE staff_status = 'Available'")),
        static_cast<int>(countOf(con.get(), "SELECT CAST(COALESCE(SUM(available_quantity), 0) AS SIGNED) AS n FROM equipment WHERE equipment_status <> 'Maintenance'")),
    };
    // Maximum claim per severity: {beds, staff, equipment}.
    auto maxFor = [](long long pr) -> std::vector<int> {
        switch (pr) {
            case 1: return {1, 2, 2};
            case 2: return {1, 1, 1};
            case 3: return {1, 1, 0};
            default: return {0, 1, 0};
        }
    };
    int i = 0;
    for (const auto& r : procs) {
        string name = r["name"].is_null() ? "Unnamed" : r["name"].get<string>();
        long long pr = r["priority"].get<long long>();
        in.processes.push_back("P" + std::to_string(i++) + " #" + std::to_string(r["patient_id"].get<long long>()) + " " + name +
                               " (" + priorityLabel(pr) + ")");
        std::vector<int> alloc = {static_cast<int>(r["beds_held"].get<long long>()), static_cast<int>(r["staff_held"].get<long long>()),
                                  static_cast<int>(r["equipment_held"].get<long long>())};
        std::vector<int> mx = maxFor(pr);
        for (size_t j = 0; j < mx.size(); ++j) mx[j] = std::max(mx[j], alloc[j]);
        in.allocation.push_back(alloc);
        in.max.push_back(mx);
    }
    in.source = "Live hospital data from MySQL (max claim by severity: Critical 1/2/2, High 1/1/1, Medium 1/1/0, Low 0/1/0)";
    return in;
}

// CPU scheduling: every active queue entry is a process. Arrival = minutes after the first
// arrival, burst = the severity release timer (how long the patient holds resources).
static std::vector<oslab::Proc> liveProcesses(ConnectionPool& db, const Config& cfg) {
    auto con = db.acquire();
    json rows = query(con.get(),
        "SELECT q.queue_id, q.patient_id, p.name, q.priority, "
        "TIMESTAMPDIFF(MINUTE, (SELECT MIN(q2.arrival_time) FROM emergency_queue q2 WHERE q2.queue_status IN ('Waiting','In Treatment')), q.arrival_time) AS arrival_min "
        "FROM emergency_queue q JOIN patients p ON p.patient_id = q.patient_id "
        "WHERE q.queue_status IN ('Waiting','In Treatment') ORDER BY q.arrival_time, q.queue_id LIMIT 30");
    std::vector<oslab::Proc> out;
    for (const auto& r : rows) {
        long long pr = r["priority"].get<long long>();
        string name = r["name"].is_null() ? "Unnamed" : r["name"].get<string>();
        out.push_back({"Q" + std::to_string(r["queue_id"].get<long long>()), "#" + std::to_string(r["patient_id"].get<long long>()) + " " + name,
                       static_cast<int>(r["arrival_min"].is_null() ? 0 : r["arrival_min"].get<long long>()),
                       std::max(1, cfg.releaseMinutesFor(pr)), static_cast<int>(pr)});
    }
    return out;
}

// Starts more threads than there are pooled connections, so the extra ones must wait.
static json connectionPoolDemo(ConnectionPool& db, const json& p) {
    int extra = oslab::intParam(p, "extra_threads", 3, 1, 5);
    int n = db.size() + extra;
    oslab::EventLog log;
    std::mutex rowsMutex;
    json rows = json::array();
    std::atomic<int> waited{0}, maxWaiting{0};
    std::atomic<bool> sampling{true};
    std::thread sampler([&] {
        while (sampling) {
            int w = db.stats().waiting;
            int m = maxWaiting.load();
            while (w > m && !maxWaiting.compare_exchange_weak(m, w)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    log.add("main", "Pool size " + std::to_string(db.size()) + ", starting " + std::to_string(n) + " threads");
    std::vector<std::thread> threads;
    for (int i = 1; i <= n; ++i)
        threads.emplace_back([&, i] {
            string who = "Thread " + std::to_string(i);
            auto t0 = std::chrono::steady_clock::now();
            try {
                auto con = db.acquire();
                long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (ms >= 50) ++waited;
                log.add(who, ms >= 50 ? "slept on the condition variable for " + std::to_string(ms) + " ms, then got a connection"
                                      : "got a connection immediately");
                queryRaw(con.get(), "SELECT SLEEP(0.25) AS slept");  // hold it like a slow request
                log.add(who, "returns the connection -> notify_one() wakes a waiting thread");
                std::lock_guard<std::mutex> g(rowsMutex);
                rows.push_back({{"thread", who}, {"waited_ms", ms}, {"had_to_wait", ms >= 50}});
            } catch (const std::exception& e) {
                log.add(who, string("failed: ") + e.what());
            }
        });
    for (auto& t : threads) t.join();
    sampling = false;
    sampler.join();
    std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a["waited_ms"] < b["waited_ms"]; });
    auto st = db.stats();
    return {{"summary", {{"pool_size", db.size()}, {"threads", n}, {"threads_that_waited", waited.load()},
                         {"max_threads_waiting_at_once", maxWaiting.load()}, {"pool_free_now", st.free}, {"pool_timeouts_total", st.timeouts}}},
            {"tables", json::array({{{"title", "Wait time per thread"}, {"rows", rows}}})},
            {"log", log.toJson()}};
}

static json timerThreadInfo(ConnectionPool& db, const Config& cfg) {
    auto& t = oslab::timerStats();
    auto con = db.acquire();
    json upcoming = query(con.get(),
        "SELECT a.allocation_id, a.patient_id, p.name, a.priority, DATE_FORMAT(a.release_due_at, '%Y-%m-%d %H:%i:%s') AS release_due_at, "
        "GREATEST(0, TIMESTAMPDIFF(SECOND, NOW(), a.release_due_at)) AS seconds_left "
        "FROM resource_allocations a JOIN patients p ON p.patient_id = a.patient_id "
        "WHERE a.allocation_status = 'Active' AND a.release_due_at IS NOT NULL ORDER BY a.release_due_at LIMIT 10");
    json recent = query(con.get(),
        "SELECT allocation_id, patient_id, release_type, DATE_FORMAT(release_time, '%Y-%m-%d %H:%i:%s') AS release_time "
        "FROM resource_allocations WHERE allocation_status = 'Released' ORDER BY release_time DESC, allocation_id DESC LIMIT 10");
    long long last = t.lastWakeEpochMs.load();
    json policy = json::array();
    for (int pr = 1; pr <= 4; ++pr) policy.push_back({{"severity", priorityLabel(pr)}, {"timer_minutes", cfg.releaseMinutesFor(pr)}});
    return {{"summary", {{"thread_running", t.running.load()}, {"wakes_every_seconds", t.intervalSeconds.load()},
                         {"wakeups_since_start", t.wakeups.load()}, {"auto_releases_since_start", t.autoReleases.load()},
                         {"seconds_since_last_wake", last ? (oslab::epochMs() - last) / 1000 : -1}}},
            {"tables", json::array({{{"title", "Next timers to expire (sleeping until due)"}, {"rows", upcoming}},
                                    {{"title", "Recently released"}, {"rows", recent}},
                                    {{"title", "Severity timer policy"}, {"rows", policy}}})},
            {"log", json::array()}};
}

// ---------------------------------------------------------------- main

int main() {
    Config cfg = loadConfig();
    std::unique_ptr<ConnectionPool> pool;
    try {
        pool = std::make_unique<ConnectionPool>(cfg);
    } catch (const sql::SQLException& e) {
        std::cerr << "Could not connect to MySQL at " << cfg.dbHost << ":" << cfg.dbPort << " as '" << cfg.dbUser
                  << "' (database '" << cfg.dbName << "').\n"
                  << e.what() << "\n"
                  << "Check that MySQL is running, the schema is imported, and backend/.env is correct.\n";
        return 1;
    }
    ConnectionPool& db = *pool;
    try {
        ensureAllocationTimerColumns(db);
    } catch (const sql::SQLException& e) {
        std::cerr << "Could not add the release-timer columns to resource_allocations: " << e.what() << "\n";
        return 1;
    }
    try {
        // Views, stored routines, triggers, audit table and indexes (dbms_concepts.hpp).
        dbmslab::installDbmsObjects(db);
    } catch (const std::exception& e) {
        std::cerr << "Could not install DBMS concept objects: " << e.what() << "\n";
    }
    oslab::startEpochMs();

    httplib::Server svr;

    // OS concept, live: every request runs on one of cpp-httplib's worker threads.
    // These hooks record per-route statistics (see ServerMetrics in os_concepts.hpp).
    static thread_local std::chrono::steady_clock::time_point requestStart;
    static thread_local bool requestTracked = false;
    svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response&) {
        requestStart = std::chrono::steady_clock::now();
        requestTracked = true;
        oslab::metrics().requestStarted();
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        if (!requestTracked) return;
        requestTracked = false;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestStart).count();
        oslab::metrics().requestFinished(req.method, req.path, res.status, ms);
    });

    // Allow the React dev server (or any local page) to call the API directly.
    svr.set_default_headers({{"Access-Control-Allow-Origin", "*"},
                             {"Access-Control-Allow-Headers", "Content-Type"},
                             {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"}});
    svr.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    svr.Get("/api/health", safe([&](const httplib::Request&, httplib::Response& res) {
        auto con = db.acquire();
        query(con.get(), "SELECT 1 AS ok");
        sendJson(res, {{"status", "ok"}, {"database", "connected"}, {"backend", "C++"}});
    }));

    svr.Get("/api/dashboard", safe([&](const httplib::Request&, httplib::Response& res) {
        auto con = db.acquire();
        json queue = query(con.get(), string(ACTIVE_QUEUE_SQL) + " LIMIT 10");
        sendJson(res, {
            {"totalPatients", countOf(con.get(), "SELECT COUNT(*) AS n FROM patients")},
            {"waitingPatients", countOf(con.get(), "SELECT COUNT(*) AS n FROM emergency_queue WHERE queue_status IN ('Waiting','In Treatment')")},
            {"availableBeds", countOf(con.get(), "SELECT COUNT(*) AS n FROM beds WHERE bed_status='Available'")},
            {"availableStaff", countOf(con.get(), "SELECT COUNT(*) AS n FROM staff WHERE staff_status='Available'")},
            {"queue", withPriorityLabels(queue)},
        });
    }));

    svr.Get("/api/queue", safe([&](const httplib::Request&, httplib::Response& res) {
        auto con = db.acquire();
        sendJson(res, withPriorityLabels(query(con.get(), ACTIVE_QUEUE_SQL)));
    }));

    svr.Post("/api/queue", safe([&](const httplib::Request& req, httplib::Response& res) {
        json body = parseBody(req);
        auto patientId = optInt(body, "patient_id");
        auto priority = optInt(body, "priority");
        if (!patientId || *patientId <= 0 || !priority || *priority < 1 || *priority > 4)
            throw ApiError{400, "Enter a valid patient ID and priority from 1 to 4."};

        json result = withTransaction(db, [&](sql::Connection* con) {
            // Lock the patient row first so two clerks cannot queue the same patient twice.
            if (query(con, "SELECT patient_id FROM patients WHERE patient_id=? FOR UPDATE",
                      [&](auto* st) { st->setInt64(1, *patientId); }).empty())
                throw ApiError{404, "Patient not found. Register the patient first."};
            if (!query(con, "SELECT queue_id FROM emergency_queue WHERE patient_id=? AND queue_status IN ('Waiting','In Treatment') LIMIT 1 FOR UPDATE",
                       [&](auto* st) { st->setInt64(1, *patientId); }).empty())
                throw ApiError{409, "Patient is already in the emergency queue."};
            execute(con, "INSERT INTO emergency_queue (patient_id, priority) VALUES (?, ?)", [&](auto* st) {
                st->setInt64(1, *patientId);
                st->setInt(2, static_cast<int>(*priority));
            });
            return json{{"queue_id", lastInsertId(con)}, {"patient_id", *patientId}, {"priority", *priority},
                        {"priority_label", priorityLabel(*priority)}, {"queue_status", "Waiting"}};
        });
        sendJson(res, result, 201);
    }));

    // Move a queue entry forward: Waiting -> In Treatment -> Completed (or Cancelled).
    svr.Post(R"(/api/queue/([^/]+)/status)", safe([&](const httplib::Request& req, httplib::Response& res) {
        long long queueId = parseIdParam(req.matches[1], "queue ID");
        string status = optStr(parseBody(req), "status");
        if (status != "In Treatment" && status != "Completed" && status != "Cancelled")
            throw ApiError{400, "Status must be In Treatment, Completed or Cancelled."};
        withTransaction(db, [&](sql::Connection* con) {
            json rows = query(con, "SELECT queue_status FROM emergency_queue WHERE queue_id=? FOR UPDATE",
                              [&](auto* st) { st->setInt64(1, queueId); });
            if (rows.empty()) throw ApiError{404, "Queue entry not found."};
            string current = rows[0]["queue_status"];
            if (current == "Completed" || current == "Cancelled") throw ApiError{409, "This queue entry is already closed."};
            execute(con, "UPDATE emergency_queue SET queue_status=? WHERE queue_id=?", [&](auto* st) {
                st->setString(1, status);
                st->setInt64(2, queueId);
            });
            return 0;
        });
        sendJson(res, {{"queue_id", queueId}, {"queue_status", status}, {"message", "Queue status updated."}});
    }));

    svr.Get("/api/resources", safe([&](const httplib::Request&, httplib::Response& res) {
        auto con = db.acquire();
        // seconds_remaining is computed by MySQL so the browser's clock/time zone does not matter.
        json allocations = query(con.get(),
            "SELECT a.allocation_id, a.patient_id, p.name AS patient_name, a.bed_id, b.bed_number, a.staff_id, s.staff_name, "
            "a.equipment_id, e.equipment_name, DATE_FORMAT(a.allocation_time, '%Y-%m-%d %H:%i:%s') AS allocation_time, a.allocation_status, "
            "a.priority, DATE_FORMAT(a.release_due_at, '%Y-%m-%d %H:%i:%s') AS release_due_at, "
            "CAST(GREATEST(0, TIMESTAMPDIFF(SECOND, NOW(), a.release_due_at)) AS SIGNED) AS seconds_remaining "
            "FROM resource_allocations a JOIN patients p ON p.patient_id=a.patient_id "
            "LEFT JOIN beds b ON b.bed_id=a.bed_id LEFT JOIN staff s ON s.staff_id=a.staff_id "
            "LEFT JOIN equipment e ON e.equipment_id=a.equipment_id "
            "WHERE a.allocation_status='Active' ORDER BY a.allocation_time DESC, a.allocation_id DESC");
        for (auto& a : allocations)
            a["priority_label"] = a["priority"].is_null() ? json(nullptr) : json(priorityLabel(a["priority"].get<long long>()));
        json releasePolicy = json::array();
        for (int p = 1; p <= 4; ++p)
            releasePolicy.push_back({{"priority", p}, {"label", priorityLabel(p)}, {"minutes", cfg.releaseMinutesFor(p)}});
        sendJson(res, {
            {"beds", query(con.get(), "SELECT bed_id, bed_number, ward, bed_type, bed_status FROM beds ORDER BY bed_id")},
            {"staff", query(con.get(), "SELECT staff_id, staff_name, role, specialization, staff_status FROM staff ORDER BY staff_id")},
            {"equipment", query(con.get(), "SELECT equipment_id, equipment_name, total_quantity, available_quantity, equipment_status FROM equipment ORDER BY equipment_id")},
            {"allocations", allocations},
            {"release_policy", releasePolicy},
        });
    }));

    svr.Post("/api/patients", safe([&](const httplib::Request& req, httplib::Response& res) {
        json body = parseBody(req);
        string name = optStr(body, "name");
        auto age = optInt(body, "age");
        string gender = optStr(body, "gender");
        string contact = optStr(body, "contact");
        string aadhaar = optStr(body, "aadhaar");
        string address = optStr(body, "address");
        if (name.empty() || name.size() > 100) throw ApiError{400, "Patient name is required and must be under 100 characters."};
        if (age && (*age < 0 || *age > 130)) throw ApiError{400, "Age must be between 0 and 130."};
        if (!aadhaar.empty() && !std::regex_match(aadhaar, std::regex("\\d{12}")))
            throw ApiError{400, "Patient identifier must be exactly 12 digits when provided."};
        if (contact.size() > 15) throw ApiError{400, "Contact number must be 15 characters or fewer."};
        if (address.size() > 255) throw ApiError{400, "Address must be 255 characters or fewer."};
        if (gender.size() > 20) throw ApiError{400, "Gender must be 20 characters or fewer."};

        auto con = db.acquire();
        execute(con.get(), "INSERT INTO patients (name, age, gender, contact, aadhaar, address) VALUES (?, ?, ?, ?, ?, ?)",
                [&](auto* st) {
                    st->setString(1, name);
                    setOptionalInt(st, 2, age);
                    setOptionalString(st, 3, gender);
                    setOptionalString(st, 4, contact);
                    setOptionalString(st, 5, aadhaar);
                    setOptionalString(st, 6, address);
                });
        sendJson(res, {{"patient_id", lastInsertId(con.get())}, {"message", "Patient registered."}}, 201);
    }));

    svr.Get(R"(/api/patients/([^/]+))", safe([&](const httplib::Request& req, httplib::Response& res) {
        long long id = parseIdParam(req.matches[1], "patient ID");
        auto con = db.acquire();
        json rows = query(con.get(),
            "SELECT patient_id, name, age, gender, contact, address, "
            "DATE_FORMAT(registration_time, '%Y-%m-%d %H:%i:%s') AS registration_time FROM patients WHERE patient_id=?",
            [&](auto* st) { st->setInt64(1, id); });
        if (rows.empty()) throw ApiError{404, "Patient not found."};
        sendJson(res, rows[0]);
    }));

    // Manually release a patient by patient ID: returns every active allocation's
    // resources and closes the patient's queue entry.
    svr.Post(R"(/api/patients/([^/]+)/release)", safe([&](const httplib::Request& req, httplib::Response& res) {
        long long id = parseIdParam(req.matches[1], "patient ID");
        json result = withTransaction(db, [&](sql::Connection* con) {
            json patient = query(con, "SELECT patient_id, name FROM patients WHERE patient_id=? FOR UPDATE",
                                 [&](auto* st) { st->setInt64(1, id); });
            if (patient.empty()) throw ApiError{404, "Patient not found."};
            json allocs = query(con, "SELECT allocation_id, bed_id, staff_id, equipment_id FROM resource_allocations "
                                     "WHERE patient_id=? AND allocation_status='Active' ORDER BY allocation_id FOR UPDATE",
                                [&](auto* st) { st->setInt64(1, id); });
            returnResources(con, allocs, "Manual");
            bool queueCompleted = completeActiveQueueEntry(con, id);
            if (allocs.empty() && !queueCompleted)
                throw ApiError{409, "Patient #" + std::to_string(id) + " has no active allocations or queue entry to release."};
            string message = "Patient #" + std::to_string(id) + " released: " + std::to_string(allocs.size()) +
                             " allocation(s) returned" + (queueCompleted ? ", queue entry marked Completed." : ".");
            return json{{"patient_id", id}, {"name", patient[0]["name"]}, {"allocations_released", allocs.size()},
                        {"queue_completed", queueCompleted}, {"message", message}};
        });
        sendJson(res, result);
    }));

    svr.Post("/api/records", safe([&](const httplib::Request& req, httplib::Response& res) {
        json body = parseBody(req);
        auto patientId = optInt(body, "patient_id");
        string symptoms = optStr(body, "symptoms");
        string diagnosis = optStr(body, "diagnosis");
        string treatment = optStr(body, "treatment");
        string status = optStr(body, "admission_status");
        if (status.empty()) status = "Waiting";
        if (!patientId || *patientId <= 0) throw ApiError{400, "Enter a valid patient ID."};
        if (symptoms.empty()) throw ApiError{400, "Symptoms are required."};
        if (status != "Waiting" && status != "Admitted" && status != "Under Treatment" && status != "Discharged")
            throw ApiError{400, "Invalid admission status."};

        auto con = db.acquire();
        if (query(con.get(), "SELECT patient_id FROM patients WHERE patient_id=?", [&](auto* st) { st->setInt64(1, *patientId); }).empty())
            throw ApiError{404, "Patient not found."};
        execute(con.get(), "INSERT INTO patient_records (patient_id, symptoms, diagnosis, treatment, admission_status) VALUES (?, ?, ?, ?, ?)",
                [&](auto* st) {
                    st->setInt64(1, *patientId);
                    st->setString(2, symptoms);
                    st->setString(3, diagnosis);
                    st->setString(4, treatment);
                    st->setString(5, status);
                });
        sendJson(res, {{"record_id", lastInsertId(con.get())}, {"message", "Medical record saved."}}, 201);
    }));

    svr.Get(R"(/api/records/([^/]+))", safe([&](const httplib::Request& req, httplib::Response& res) {
        long long id = parseIdParam(req.matches[1], "patient ID");
        auto con = db.acquire();
        sendJson(res, query(con.get(),
            "SELECT record_id, patient_id, symptoms, diagnosis, treatment, admission_status, "
            "DATE_FORMAT(record_date, '%Y-%m-%d %H:%i:%s') AS record_date FROM patient_records "
            "WHERE patient_id=? ORDER BY record_date DESC, record_id DESC",
            [&](auto* st) { st->setInt64(1, id); }));
    }));

    svr.Post("/api/allocations", safe([&](const httplib::Request& req, httplib::Response& res) {
        json body = parseBody(req);
        auto patientId = optInt(body, "patient_id");
        if (!patientId || *patientId <= 0) throw ApiError{400, "Enter a valid patient ID."};
        auto bedId = optionalResource(body, "bed_id", "Bed");
        auto staffId = optionalResource(body, "staff_id", "Staff");
        auto equipmentId = optionalResource(body, "equipment_id", "Equipment");
        if (!bedId && !staffId && !equipmentId) throw ApiError{400, "Select at least one resource."};
        // Severity for the release timer: taken from the request, or from the patient's queue entry.
        auto requestedPriority = optInt(body, "priority");
        if (requestedPriority && (*requestedPriority < 1 || *requestedPriority > 4))
            throw ApiError{400, "Severity must be from 1 (Critical) to 4 (Low)."};

        json result = withTransaction(db, [&](sql::Connection* con) {
            // Deadlock prevention: every transaction locks rows in the SAME order
            // (patient -> bed -> staff -> equipment), so a circular wait cannot form.
            if (query(con, "SELECT patient_id FROM patients WHERE patient_id=? FOR UPDATE",
                      [&](auto* st) { st->setInt64(1, *patientId); }).empty())
                throw ApiError{404, "Patient not found."};
            long long priority = 0;
            if (requestedPriority) {
                priority = *requestedPriority;
            } else {
                json q = query(con, "SELECT priority FROM emergency_queue WHERE patient_id=? AND queue_status IN ('Waiting','In Treatment') "
                                    "ORDER BY arrival_time DESC, queue_id DESC LIMIT 1",
                               [&](auto* st) { st->setInt64(1, *patientId); });
                if (q.empty())
                    throw ApiError{400, "Patient is not in the emergency queue. Choose a severity so the release timer can be set."};
                priority = q[0]["priority"].get<long long>();
            }
            int releaseMinutes = cfg.releaseMinutesFor(priority);
            if (bedId) {
                json r = query(con, "SELECT bed_status FROM beds WHERE bed_id=? FOR UPDATE", [&](auto* st) { st->setInt64(1, *bedId); });
                if (r.empty() || r[0]["bed_status"] != "Available")
                    throw ApiError{409, "Bed not found or not available. Refresh resources and try again."};
            }
            if (staffId) {
                json r = query(con, "SELECT staff_status FROM staff WHERE staff_id=? FOR UPDATE", [&](auto* st) { st->setInt64(1, *staffId); });
                if (r.empty() || r[0]["staff_status"] != "Available") throw ApiError{409, "Staff member not found or not available."};
            }
            if (equipmentId) {
                json r = query(con, "SELECT available_quantity, equipment_status FROM equipment WHERE equipment_id=? FOR UPDATE",
                               [&](auto* st) { st->setInt64(1, *equipmentId); });
                if (r.empty() || r[0]["available_quantity"].get<long long>() < 1 || r[0]["equipment_status"] == "Maintenance")
                    throw ApiError{409, "Equipment not found or none available."};
            }
            execute(con, "INSERT INTO resource_allocations (patient_id, bed_id, staff_id, equipment_id, priority, release_due_at) "
                         "VALUES (?, ?, ?, ?, ?, NOW() + INTERVAL ? MINUTE)",
                    [&](auto* st) {
                        st->setInt64(1, *patientId);
                        setOptionalInt(st, 2, bedId);
                        setOptionalInt(st, 3, staffId);
                        setOptionalInt(st, 4, equipmentId);
                        st->setInt(5, static_cast<int>(priority));
                        st->setInt(6, releaseMinutes);
                    });
            long long allocationId = lastInsertId(con);
            if (bedId) execute(con, "UPDATE beds SET bed_status='Occupied' WHERE bed_id=?", [&](auto* st) { st->setInt64(1, *bedId); });
            if (staffId) execute(con, "UPDATE staff SET staff_status='Busy' WHERE staff_id=?", [&](auto* st) { st->setInt64(1, *staffId); });
            if (equipmentId)
                // MySQL applies SET assignments left to right, so compute the status first.
                execute(con, "UPDATE equipment SET equipment_status=IF(available_quantity-1=0,'In Use','Available'), "
                             "available_quantity=available_quantity-1 WHERE equipment_id=?",
                        [&](auto* st) { st->setInt64(1, *equipmentId); });
            return json{{"allocation_id", allocationId}, {"patient_id", *patientId},
                        {"bed_id", bedId ? json(*bedId) : json(nullptr)},
                        {"staff_id", staffId ? json(*staffId) : json(nullptr)},
                        {"equipment_id", equipmentId ? json(*equipmentId) : json(nullptr)},
                        {"priority", priority}, {"priority_label", priorityLabel(priority)},
                        {"release_in_minutes", releaseMinutes},
                        {"allocation_status", "Active"},
                        {"message", "Resources allocated. Auto-release in " + std::to_string(releaseMinutes) +
                                        " min (" + priorityLabel(priority) + ")."}};
        });
        sendJson(res, result, 201);
    }));

    svr.Post(R"(/api/allocations/([^/]+)/release)", safe([&](const httplib::Request& req, httplib::Response& res) {
        long long allocationId = parseIdParam(req.matches[1], "allocation ID");
        withTransaction(db, [&](sql::Connection* con) { return releaseOneAllocation(con, allocationId, "Manual", false); });
        sendJson(res, {{"allocation_id", allocationId}, {"message", "Allocation released and resources returned."}});
    }));

    // ------------------------------------------------------------ OS concepts (os_concepts.hpp)

    // Live view of the OS concepts the server itself is using right now.
    svr.Get("/api/os/live", safe([&](const httplib::Request&, httplib::Response& res) {
        auto ps = db.stats();
        auto& tx = txStats();
        auto& tm = oslab::timerStats();
        long long last = tm.lastWakeEpochMs.load();
        sendJson(res, {
            {"process_id", oslab::processId()},
            {"uptime_seconds", (oslab::epochMs() - oslab::startEpochMs()) / 1000},
            {"hardware_threads", std::thread::hardware_concurrency()},
            {"http_thread_pool_size", static_cast<long long>(CPPHTTPLIB_THREAD_POOL_COUNT)},
            {"server", oslab::metrics().snapshot()},
            {"connection_pool", {{"size", ps.size}, {"free", ps.free}, {"in_use", ps.inUse}, {"threads_waiting", ps.waiting},
                                 {"acquires", ps.acquires}, {"had_to_wait", ps.waits}, {"timeouts", ps.timeouts},
                                 {"reconnects", ps.reconnects}, {"max_wait_ms", ps.maxWaitMs}}},
            {"transactions", {{"started", tx.started.load()}, {"committed", tx.commits.load()}, {"rolled_back", tx.rollbacks.load()},
                              {"deadlock_retries", tx.deadlockRetries.load()}, {"lock_timeout_retries", tx.lockTimeoutRetries.load()}}},
            {"timer_thread", {{"running", tm.running.load()}, {"interval_seconds", tm.intervalSeconds.load()},
                              {"wakeups", tm.wakeups.load()}, {"auto_releases", tm.autoReleases.load()},
                              {"seconds_since_last_wake", last ? (oslab::epochMs() - last) / 1000 : -1}}},
        });
    }));

    svr.Get("/api/os/concepts", safe([&](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"concepts", oslab::catalog()}});
    }));

    // Runs one OS concept with real threads inside this process.
    svr.Post(R"(/api/os/run/([a-z_]+))", safe([&](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        json p = parseBody(req);
        auto t0 = std::chrono::steady_clock::now();
        json out;
        if (id == "race_condition") out = oslab::raceCondition(p);
        else if (id == "deadlock") out = oslab::deadlockDemo(p);
        else if (id == "starvation") out = oslab::starvationDemo(p);
        else if (id == "producer_consumer") out = oslab::producerConsumer(p);
        else if (id == "readers_writers") out = oslab::readersWriters(p);
        else if (id == "dining_philosophers") out = oslab::diningPhilosophers(p);
        else if (id == "bankers")
            out = oslab::bankers(oslab::strParam(p, "mode", "live") == "sample" ? oslab::sampleBankers() : liveBankersInput(db), p);
        else if (id == "scheduling") {
            bool sample = oslab::strParam(p, "mode", "live") == "sample";
            auto procs = sample ? std::vector<oslab::Proc>{} : liveProcesses(db, cfg);
            string source = "Live emergency queue (burst = severity release timer in minutes)";
            if (procs.empty()) {
                procs = oslab::sampleProcesses();
                source = sample ? "Starvation sample" : "Starvation sample (the emergency queue is empty right now)";
            }
            out = oslab::scheduling(procs, source, p);
        }
        else if (id == "connection_pool") out = connectionPoolDemo(db, p);
        else if (id == "timer_thread") out = timerThreadInfo(db, cfg);
        else throw ApiError{404, "Unknown OS concept: " + id};
        out["id"] = id;
        out["process_id"] = oslab::processId();
        out["duration_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sendJson(res, out);
    }));

    // ------------------------------------------------------------ DBMS concepts (dbms_concepts.hpp)

    svr.Get("/api/dbms/concepts", safe([&](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"concepts", dbmslab::catalog()}, {"installed", dbmslab::installedObjects()}});
    }));

    // Runs one DBMS concept as real SQL against MySQL.
    svr.Post(R"(/api/dbms/run/([a-z_]+))", safe([&](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        auto t0 = std::chrono::steady_clock::now();
        json out = dbmslab::runDbmsDemo(id, db);
        out["id"] = id;
        out["duration_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sendJson(res, out);
    }));

    svr.Get(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"message", "API endpoint not found."}}, 404);
    });
    svr.Post(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"message", "API endpoint not found."}}, 404);
    });

    // If the React app has been built (npm run build), serve it too.
    for (const char* dist : {"../frontend/dist", "../../frontend/dist", "frontend/dist"}) {
        if (std::filesystem::exists(std::filesystem::path(dist) / "index.html")) {
            svr.set_mount_point("/", dist);
            std::cout << "Serving built frontend from " << dist << "\n";
            break;
        }
    }

    // Background thread: releases allocations whose severity timer has run out.
    std::atomic<bool> running{true};
    oslab::timerStats().intervalSeconds = cfg.autoReleaseCheckSeconds;
    oslab::timerStats().startedEpochMs = oslab::epochMs();
    oslab::timerStats().running = true;
    std::thread autoRelease([&] {
        while (running) {
            // Sleep in 1-second steps so the thread can be stopped quickly (atomic flag).
            for (int i = 0; i < cfg.autoReleaseCheckSeconds && running; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running) break;
            ++oslab::timerStats().wakeups;
            oslab::timerStats().lastWakeEpochMs = oslab::epochMs();
            try {
                json due;
                {
                    auto con = db.acquire();  // returned to the pool before the releases below
                    due = query(con.get(),
                        "SELECT allocation_id FROM resource_allocations WHERE allocation_status='Active' "
                        "AND release_due_at IS NOT NULL AND release_due_at <= NOW() "
                        "ORDER BY release_due_at, allocation_id LIMIT 100");
                }
                for (const auto& d : due) {
                    long long allocationId = d["allocation_id"].get<long long>();
                    try {
                        long long patientId = withTransaction(db, [&](sql::Connection* con) {
                            return releaseOneAllocation(con, allocationId, "Automatic", true);
                        });
                        ++oslab::timerStats().autoReleases;
                        std::cout << "[auto-release] Allocation #" << allocationId << " for patient #" << patientId
                                  << " released (severity timer ran out)" << std::endl;
                    } catch (const ApiError&) {
                        // Already released manually in the meantime.
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[auto-release] " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[auto-release] unexpected error\n";
            }
        }
    });

    std::cout << "EmergencySOS C++ API running at http://localhost:" << cfg.port << "\n"
              << "Database: " << cfg.dbName << " on " << cfg.dbHost << ":" << cfg.dbPort << "\n"
              << "Auto-release timers (min): Critical " << cfg.releaseMinutesCritical << ", High " << cfg.releaseMinutesHigh
              << ", Medium " << cfg.releaseMinutesMedium << ", Low " << cfg.releaseMinutesLow
              << " - checked every " << cfg.autoReleaseCheckSeconds << "s\n";
    bool listened = svr.listen("0.0.0.0", cfg.port);
    running = false;
    autoRelease.join();
    oslab::timerStats().running = false;
    if (!listened) {
        std::cerr << "Could not start the server on port " << cfg.port << ". Is it already in use?\n";
        return 1;
    }
    return 0;
}
