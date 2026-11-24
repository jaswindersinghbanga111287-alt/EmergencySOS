// EmergencySOS — Database (DBMS) concepts, implemented in C++ on top of MySQL and served
// to the React frontend through the /api/dbms/* routes in main.cpp.
//
// installDbmsObjects() runs once when the server starts and creates (idempotently) the
// views, stored function, stored procedures, triggers, audit table and indexes below.
// Each case in runDbmsDemo() executes REAL SQL and returns the statements + result rows.
// Demos that change data always ROLLBACK, so the hospital data is never modified.
//
//   Concept                            Demo id            Used live by the hospital app
//   ---------------------------------  -----------------  -------------------------------------------
//   Keys & constraints (PK/FK/UQ/CHK)  keys_constraints   every table in project_schema.sql
//   ER -> relational, FK actions       relationships      patients 1:N records / queue / allocations
//   Normalization (1NF..3NF)           normalization      name stored once in patients, joined in
//   Joins (INNER / LEFT / multi)       joins              /api/queue, /api/resources
//   Aggregation (GROUP BY, HAVING)     aggregation        /api/dashboard counts
//   Subqueries (nested, correlated)    subqueries         queue check before insert
//   Set operations (UNION, INTERSECT)  set_operations     —
//   Views                              views              v_active_queue, v_resource_utilization
//   Stored function / procedures       stored_routines    fn_priority_label, sp_hospital_summary
//   Triggers + audit trail             triggers           every queue / allocation / record change
//   Indexing + EXPLAIN                 indexing           queue ordering + auto-release timer query
//   Transactions, ACID, savepoints     transactions       withTransaction() in db_pool.hpp
//   Isolation levels, dirty reads      isolation          InnoDB REPEATABLE READ (MVCC)
//   Row locks (FOR UPDATE / NOWAIT)    locking            POST /api/allocations, releases
//   DB deadlock detection + victim     db_deadlock        withTransaction() retry on error 1213
//   Integrity enforcement              integrity          error mapping in safe() (1062, 1452)
#pragma once
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db_helpers.hpp"
#include "db_pool.hpp"

namespace dbmslab {

// =====================================================================================
// Demo recorder: keeps the SQL that ran, the result sets and a narration (thread-safe,
// because the deadlock demo uses two threads).
// =====================================================================================
class Demo {
public:
    json query(sql::Connection* con, const string& title, const string& sqlText, const string& conn = "A") {
        statement(conn, sqlText);
        json rows = queryRaw(con, sqlText);
        std::lock_guard<std::mutex> lock(m_);
        tables_.push_back({{"title", title}, {"sql", sqlText}, {"rows", rows}});
        return rows;
    }
    json silent(sql::Connection* con, const string& sqlText, const string& conn = "A") {
        statement(conn, sqlText);
        return queryRaw(con, sqlText);
    }
    void exec(sql::Connection* con, const string& sqlText, const string& conn = "A") {
        statement(conn, sqlText);
        executeRaw(con, sqlText);
    }
    // Runs a statement that may fail. Returns the MySQL error code (0 = success).
    int attempt(sql::Connection* con, const string& sqlText, const string& conn = "A") {
        statement(conn, sqlText);
        try {
            executeRaw(con, sqlText);
            note(conn, "OK");
            return 0;
        } catch (const sql::SQLException& e) {
            note(conn, "MySQL error " + std::to_string(e.getErrorCode()) + ": " + e.what());
            return e.getErrorCode();
        }
    }
    void note(const string& who, const string& text) {
        std::lock_guard<std::mutex> lock(m_);
        log_.push_back({{"t_ms", elapsed()}, {"thread", who == "A" || who == "B" ? "Connection " + who : who}, {"event", text}});
    }
    void table(const string& title, const json& rows, const string& sqlText = "") {
        std::lock_guard<std::mutex> lock(m_);
        tables_.push_back({{"title", title}, {"sql", sqlText}, {"rows", rows}});
    }
    void startTx(sql::Connection* con, const string& conn = "A") {
        con->setAutoCommit(false);  // = START TRANSACTION (rolled back automatically if anything throws)
        statement(conn, "START TRANSACTION");
    }
    void rollback(sql::Connection* con, const string& conn = "A") {
        statement(conn, "ROLLBACK");
        con->rollback();
        con->setAutoCommit(true);
    }
    json result(json summary) {
        std::lock_guard<std::mutex> lock(m_);
        return {{"summary", summary}, {"statements", statements_}, {"tables", tables_}, {"log", log_}};
    }

private:
    long long elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    }
    void statement(const string& conn, const string& sqlText) {
        std::lock_guard<std::mutex> lock(m_);
        statements_.push_back({{"t_ms", elapsed()}, {"connection", conn}, {"sql", sqlText}});
    }
    std::mutex m_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    json statements_ = json::array(), tables_ = json::array(), log_ = json::array();
};

// Runs CALL ... and returns the first result set (stored procedures can return several).
inline json callProcedure(sql::Connection* con, const string& sqlText) {
    std::unique_ptr<sql::Statement> st(con->createStatement());
    json rows = json::array();
    bool first = true;
    if (st->execute(sqlText)) {
        do {
            std::unique_ptr<sql::ResultSet> rs(st->getResultSet());
            if (rs && first) { rows = rowsToJson(rs.get()); first = false; }
        } while (st->getMoreResults());
    }
    return rows;
}

// =====================================================================================
// Database objects created at start-up (views, routines, triggers, audit table, indexes)
// =====================================================================================
inline std::mutex& installMutex() { static std::mutex m; return m; }
inline json& installStatus() { static json s = json::array(); return s; }

inline void installDbmsObjects(ConnectionPool& db) {
    auto con = db.acquire();
    json status = json::array();
    auto step = [&](const string& name, const string& kind, const std::vector<string>& stmts) {
        try {
            for (const auto& s : stmts) executeRaw(con.get(), s);
            status.push_back({{"name", name}, {"kind", kind}, {"ok", true}});
        } catch (const sql::SQLException& e) {
            std::cerr << "[dbms] could not create " << kind << " " << name << ": " << e.what() << "\n";
            status.push_back({{"name", name}, {"kind", kind}, {"ok", false}, {"error", e.what()}});
        }
    };
    auto ensureIndex = [&](const string& table, const string& name, const string& cols) {
        try {
            bool exists = !query(con.get(),
                "SELECT 1 AS ok FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=? AND INDEX_NAME=? LIMIT 1",
                [&](auto* st) { st->setString(1, table); st->setString(2, name); }).empty();
            if (!exists) executeRaw(con.get(), "CREATE INDEX " + name + " ON " + table + " " + cols);
            status.push_back({{"name", name + " ON " + table + cols}, {"kind", "index"}, {"ok", true}});
        } catch (const sql::SQLException& e) {
            status.push_back({{"name", name}, {"kind", "index"}, {"ok", false}, {"error", e.what()}});
        }
    };

    // Audit table filled only by triggers.
    step("audit_log", "table", {
        "CREATE TABLE IF NOT EXISTS audit_log ("
        " audit_id INT AUTO_INCREMENT PRIMARY KEY,"
        " table_name VARCHAR(40) NOT NULL,"
        " action VARCHAR(20) NOT NULL,"
        " record_id INT NOT NULL,"
        " details VARCHAR(255),"
        " changed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"});

    // Views (virtual tables).
    step("v_active_queue", "view", {
        "CREATE OR REPLACE VIEW v_active_queue AS "
        "SELECT q.queue_id, q.patient_id, p.name, q.priority, "
        "CASE q.priority WHEN 1 THEN 'Critical' WHEN 2 THEN 'High' WHEN 3 THEN 'Medium' WHEN 4 THEN 'Low' ELSE 'Unknown' END AS priority_label, "
        "q.arrival_time, TIMESTAMPDIFF(MINUTE, q.arrival_time, NOW()) AS waiting_minutes, q.queue_status "
        "FROM emergency_queue q JOIN patients p ON p.patient_id = q.patient_id "
        "WHERE q.queue_status IN ('Waiting','In Treatment')"});
    step("v_resource_utilization", "view", {
        "CREATE OR REPLACE VIEW v_resource_utilization AS "
        "SELECT 'Beds' AS resource, COUNT(*) AS total, CAST(COALESCE(SUM(bed_status='Available'),0) AS SIGNED) AS available, "
        "CAST(COALESCE(SUM(bed_status='Occupied'),0) AS SIGNED) AS in_use FROM beds "
        "UNION ALL SELECT 'Staff', COUNT(*), CAST(COALESCE(SUM(staff_status='Available'),0) AS SIGNED), "
        "CAST(COALESCE(SUM(staff_status='Busy'),0) AS SIGNED) FROM staff "
        "UNION ALL SELECT 'Equipment units', CAST(COALESCE(SUM(total_quantity),0) AS SIGNED), CAST(COALESCE(SUM(available_quantity),0) AS SIGNED), "
        "CAST(COALESCE(SUM(total_quantity - available_quantity),0) AS SIGNED) FROM equipment"});

    // Stored function + procedures.
    step("fn_priority_label", "function", {
        "DROP FUNCTION IF EXISTS fn_priority_label",
        "CREATE FUNCTION fn_priority_label(p INT) RETURNS VARCHAR(10) DETERMINISTIC NO SQL "
        "RETURN CASE p WHEN 1 THEN 'Critical' WHEN 2 THEN 'High' WHEN 3 THEN 'Medium' WHEN 4 THEN 'Low' ELSE 'Unknown' END"});
    step("sp_hospital_summary", "procedure", {
        "DROP PROCEDURE IF EXISTS sp_hospital_summary",
        "CREATE PROCEDURE sp_hospital_summary() READS SQL DATA "
        "BEGIN "
        " SELECT (SELECT COUNT(*) FROM patients) AS total_patients,"
        "  (SELECT COUNT(*) FROM emergency_queue WHERE queue_status IN ('Waiting','In Treatment')) AS active_queue,"
        "  (SELECT COUNT(*) FROM beds WHERE bed_status='Available') AS beds_available,"
        "  (SELECT COUNT(*) FROM staff WHERE staff_status='Available') AS staff_available,"
        "  (SELECT CAST(COALESCE(SUM(available_quantity),0) AS SIGNED) FROM equipment) AS equipment_units_available,"
        "  (SELECT COUNT(*) FROM resource_allocations WHERE allocation_status='Active') AS active_allocations; "
        "END"});
    step("sp_count_by_priority", "procedure", {
        "DROP PROCEDURE IF EXISTS sp_count_by_priority",
        "CREATE PROCEDURE sp_count_by_priority(IN p_priority INT, OUT p_count INT) READS SQL DATA "
        "BEGIN "
        " SELECT COUNT(*) INTO p_count FROM emergency_queue "
        " WHERE priority = p_priority AND queue_status IN ('Waiting','In Treatment'); "
        "END"});

    // Triggers (AFTER = audit trail, BEFORE = derive a column value).
    step("trg_queue_after_insert", "trigger", {
        "DROP TRIGGER IF EXISTS trg_queue_after_insert",
        "CREATE TRIGGER trg_queue_after_insert AFTER INSERT ON emergency_queue FOR EACH ROW "
        "INSERT INTO audit_log (table_name, action, record_id, details) VALUES "
        "('emergency_queue', 'INSERT', NEW.queue_id, CONCAT('Patient #', NEW.patient_id, ' queued with priority ', NEW.priority))"});
    step("trg_queue_after_update", "trigger", {
        "DROP TRIGGER IF EXISTS trg_queue_after_update",
        "CREATE TRIGGER trg_queue_after_update AFTER UPDATE ON emergency_queue FOR EACH ROW "
        "BEGIN "
        " IF NOT (OLD.queue_status <=> NEW.queue_status) THEN "
        "  INSERT INTO audit_log (table_name, action, record_id, details) VALUES "
        "  ('emergency_queue', 'STATUS', NEW.queue_id, CONCAT('Patient #', NEW.patient_id, ': ', OLD.queue_status, ' -> ', NEW.queue_status)); "
        " END IF; "
        "END"});
    step("trg_alloc_after_insert", "trigger", {
        "DROP TRIGGER IF EXISTS trg_alloc_after_insert",
        "CREATE TRIGGER trg_alloc_after_insert AFTER INSERT ON resource_allocations FOR EACH ROW "
        "INSERT INTO audit_log (table_name, action, record_id, details) VALUES "
        "('resource_allocations', 'ALLOCATE', NEW.allocation_id, CONCAT('Patient #', NEW.patient_id, ' got bed ', IFNULL(NEW.bed_id, '-'), "
        "', staff ', IFNULL(NEW.staff_id, '-'), ', equipment ', IFNULL(NEW.equipment_id, '-')))"});
    step("trg_alloc_after_update", "trigger", {
        "DROP TRIGGER IF EXISTS trg_alloc_after_update",
        "CREATE TRIGGER trg_alloc_after_update AFTER UPDATE ON resource_allocations FOR EACH ROW "
        "BEGIN "
        " IF NOT (OLD.allocation_status <=> NEW.allocation_status) THEN "
        "  INSERT INTO audit_log (table_name, action, record_id, details) VALUES "
        "  ('resource_allocations', 'RELEASE', NEW.allocation_id, CONCAT('Patient #', NEW.patient_id, ' released (', IFNULL(NEW.release_type, '?'), ')')); "
        " END IF; "
        "END"});
    step("trg_records_after_insert", "trigger", {
        "DROP TRIGGER IF EXISTS trg_records_after_insert",
        "CREATE TRIGGER trg_records_after_insert AFTER INSERT ON patient_records FOR EACH ROW "
        "INSERT INTO audit_log (table_name, action, record_id, details) VALUES "
        "('patient_records', 'INSERT', NEW.record_id, CONCAT('Record for patient #', NEW.patient_id, ' (', IFNULL(NEW.admission_status, '?'), ')'))"});
    step("trg_equipment_before_update", "trigger", {
        "DROP TRIGGER IF EXISTS trg_equipment_before_update",
        "CREATE TRIGGER trg_equipment_before_update BEFORE UPDATE ON equipment FOR EACH ROW "
        "BEGIN "
        " IF NEW.available_quantity = 0 AND NEW.equipment_status = 'Available' THEN SET NEW.equipment_status = 'In Use'; END IF; "
        "END"});

    // Indexes that speed up the app's hottest queries.
    ensureIndex("emergency_queue", "idx_queue_status_priority", "(queue_status, priority, arrival_time)");
    ensureIndex("resource_allocations", "idx_alloc_status_due", "(allocation_status, release_due_at)");
    ensureIndex("patient_records", "idx_records_patient_date", "(patient_id, record_date)");
    ensureIndex("patients", "idx_patients_name", "(name)");
    ensureIndex("audit_log", "idx_audit_changed", "(changed_at)");

    int failed = 0;
    for (auto& s : status) if (!s["ok"].get<bool>()) ++failed;
    std::cout << "DBMS objects ready: " << (status.size() - failed) << "/" << status.size()
              << (failed ? " (see warnings above)" : "") << std::endl;
    std::lock_guard<std::mutex> lock(installMutex());
    installStatus() = status;
}

inline json installedObjects() {
    std::lock_guard<std::mutex> lock(installMutex());
    return installStatus();
}

// =====================================================================================
// Demos
// =====================================================================================
inline json keysConstraints(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    json rows = d.query(con.get(), "Keys and constraints per table",
        "SELECT tc.TABLE_NAME AS table_name, tc.CONSTRAINT_TYPE AS constraint_type, tc.CONSTRAINT_NAME AS constraint_name, "
        "GROUP_CONCAT(k.COLUMN_NAME ORDER BY k.ORDINAL_POSITION SEPARATOR ', ') AS key_columns "
        "FROM information_schema.TABLE_CONSTRAINTS tc "
        "LEFT JOIN information_schema.KEY_COLUMN_USAGE k ON k.CONSTRAINT_SCHEMA = tc.CONSTRAINT_SCHEMA "
        " AND k.TABLE_NAME = tc.TABLE_NAME AND k.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
        "WHERE tc.CONSTRAINT_SCHEMA = DATABASE() "
        "GROUP BY tc.TABLE_NAME, tc.CONSTRAINT_TYPE, tc.CONSTRAINT_NAME "
        "ORDER BY tc.TABLE_NAME, FIELD(tc.CONSTRAINT_TYPE, 'PRIMARY KEY', 'UNIQUE', 'FOREIGN KEY', 'CHECK'), tc.CONSTRAINT_NAME");
    d.query(con.get(), "CHECK constraints (domain integrity)",
        "SELECT CONSTRAINT_NAME AS constraint_name, CHECK_CLAUSE AS check_clause "
        "FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = DATABASE() ORDER BY CONSTRAINT_NAME");
    d.query(con.get(), "NOT NULL / DEFAULT columns (patients)",
        "SELECT COLUMN_NAME AS column_name, COLUMN_TYPE AS type, IS_NULLABLE AS nullable, COLUMN_DEFAULT AS default_value, COLUMN_KEY AS key_type "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'patients' ORDER BY ORDINAL_POSITION");
    int pk = 0, fk = 0, uq = 0, ck = 0;
    for (auto& r : rows) {
        string t = r["constraint_type"];
        if (t == "PRIMARY KEY") ++pk; else if (t == "FOREIGN KEY") ++fk; else if (t == "UNIQUE") ++uq; else if (t == "CHECK") ++ck;
    }
    d.note("MySQL", "Read from the data dictionary (information_schema)");
    return d.result({{"primary_keys", pk}, {"foreign_keys", fk}, {"unique_keys", uq}, {"check_constraints", ck}});
}

inline json relationships(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    json rows = d.query(con.get(), "Foreign keys (ER relationships mapped to tables)",
        "SELECT k.REFERENCED_TABLE_NAME AS parent_table, k.REFERENCED_COLUMN_NAME AS parent_key, '1 : N' AS cardinality, "
        "k.TABLE_NAME AS child_table, k.COLUMN_NAME AS foreign_key, r.UPDATE_RULE AS on_update, r.DELETE_RULE AS on_delete "
        "FROM information_schema.KEY_COLUMN_USAGE k "
        "JOIN information_schema.REFERENTIAL_CONSTRAINTS r ON r.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA AND r.CONSTRAINT_NAME = k.CONSTRAINT_NAME "
        "WHERE k.CONSTRAINT_SCHEMA = DATABASE() AND k.REFERENCED_TABLE_NAME IS NOT NULL "
        "ORDER BY parent_table, child_table");
    d.table("Entities", json::array({
        {{"entity", "Patient"}, {"table", "patients"}, {"type", "Strong entity"}},
        {{"entity", "Medical record"}, {"table", "patient_records"}, {"type", "Weak entity (depends on patient, ON DELETE CASCADE)"}},
        {{"entity", "Queue entry"}, {"table", "emergency_queue"}, {"type", "Weak entity (depends on patient)"}},
        {{"entity", "Bed / Staff / Equipment"}, {"table", "beds, staff, equipment"}, {"type", "Strong entities (resources)"}},
        {{"entity", "Allocation"}, {"table", "resource_allocations"}, {"type", "Associative entity: Patient M:N Resources"}},
    }));
    d.note("MySQL", "CASCADE = child rows follow the parent; RESTRICT / NO ACTION = parent cannot be deleted while children exist");
    return d.result({{"relationships", rows.size()}});
}

inline json normalization(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "Attributes per relation",
        "SELECT TABLE_NAME AS relation, GROUP_CONCAT(COLUMN_NAME ORDER BY ORDINAL_POSITION SEPARATOR ', ') AS attributes, "
        "GROUP_CONCAT(IF(COLUMN_KEY = 'PRI', COLUMN_NAME, NULL)) AS primary_key "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME IN ('patients','patient_records','emergency_queue','beds','staff','equipment','resource_allocations') "
        "GROUP BY TABLE_NAME ORDER BY TABLE_NAME");
    json dup = d.query(con.get(), "Is patient data repeated outside 'patients'? (should be 0)",
        "SELECT COUNT(*) AS repeated_patient_columns FROM information_schema.COLUMNS c "
        "JOIN information_schema.TABLES t ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME AND t.TABLE_TYPE = 'BASE TABLE' "
        "WHERE c.TABLE_SCHEMA = DATABASE() AND c.TABLE_NAME <> 'patients' AND c.COLUMN_NAME IN ('name','age','gender','contact','address')");
    d.query(con.get(), "Data reconstructed with a lossless join",
        "SELECT q.queue_id, q.patient_id, p.name, p.age, q.priority FROM emergency_queue q "
        "JOIN patients p ON p.patient_id = q.patient_id ORDER BY q.queue_id DESC LIMIT 10");
    d.table("Normal forms", json::array({
        {{"normal_form", "1NF"}, {"rule", "Atomic values, no repeating groups"}, {"in_this_schema", "One symptom text / one resource id per column; allocations are separate rows"}},
        {{"normal_form", "2NF"}, {"rule", "No partial dependency on part of a key"}, {"in_this_schema", "Every table has a single-column surrogate primary key"}},
        {{"normal_form", "3NF"}, {"rule", "No transitive dependency (non-key -> non-key)"}, {"in_this_schema", "Patient name/contact live only in patients; queue, records and allocations store patient_id"}},
        {{"normal_form", "BCNF"}, {"rule", "Every determinant is a candidate key"}, {"in_this_schema", "bed_number and aadhaar are UNIQUE candidate keys"}},
    }));
    return d.result({{"repeated_patient_columns", dup[0]["repeated_patient_columns"]}, {"highest_normal_form", "BCNF / 3NF"}});
}

inline json joins(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    json inner = d.query(con.get(), "INNER JOIN — active queue with patient names",
        "SELECT q.queue_id, p.patient_id, p.name, q.priority, q.queue_status FROM emergency_queue q "
        "INNER JOIN patients p ON p.patient_id = q.patient_id "
        "WHERE q.queue_status IN ('Waiting','In Treatment') ORDER BY q.priority, q.arrival_time LIMIT 20");
    json left = d.query(con.get(), "LEFT OUTER JOIN — every patient, even with no records",
        "SELECT p.patient_id, p.name, COUNT(r.record_id) AS records FROM patients p "
        "LEFT JOIN patient_records r ON r.patient_id = p.patient_id GROUP BY p.patient_id, p.name ORDER BY p.patient_id DESC LIMIT 20");
    d.query(con.get(), "Multi-table LEFT JOIN — allocations with bed, staff and equipment",
        "SELECT a.allocation_id, p.name AS patient, b.bed_number, s.staff_name, e.equipment_name, a.allocation_status "
        "FROM resource_allocations a JOIN patients p ON p.patient_id = a.patient_id "
        "LEFT JOIN beds b ON b.bed_id = a.bed_id LEFT JOIN staff s ON s.staff_id = a.staff_id "
        "LEFT JOIN equipment e ON e.equipment_id = a.equipment_id ORDER BY a.allocation_id DESC LIMIT 20");
    d.query(con.get(), "SELF JOIN — staff pairs with the same role (who can cover for whom)",
        "SELECT s1.staff_name AS staff, s2.staff_name AS can_be_covered_by, s1.role FROM staff s1 "
        "JOIN staff s2 ON s1.role = s2.role AND s1.staff_id < s2.staff_id ORDER BY s1.role, s1.staff_id LIMIT 20");
    return d.result({{"inner_join_rows", inner.size()}, {"left_join_rows", left.size()}});
}

inline json aggregation(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "GROUP BY priority (all queue entries)",
        "SELECT priority, fn_priority_label(priority) AS label, COUNT(*) AS entries, "
        "SUM(queue_status IN ('Waiting','In Treatment')) AS active, SUM(queue_status = 'Completed') AS completed, "
        "ROUND(AVG(TIMESTAMPDIFF(MINUTE, arrival_time, NOW())), 1) AS avg_age_minutes "
        "FROM emergency_queue GROUP BY priority ORDER BY priority");
    d.query(con.get(), "Equipment utilisation (computed columns)",
        "SELECT equipment_name, total_quantity, available_quantity, total_quantity - available_quantity AS in_use, "
        "ROUND(100 * (total_quantity - available_quantity) / NULLIF(total_quantity, 0), 1) AS utilisation_pct "
        "FROM equipment ORDER BY utilisation_pct DESC, equipment_name");
    d.query(con.get(), "HAVING — wards with at least one free bed",
        "SELECT ward, COUNT(*) AS beds, SUM(bed_status = 'Available') AS free_beds FROM beds "
        "GROUP BY ward HAVING SUM(bed_status = 'Available') >= 1 ORDER BY free_beds DESC");
    d.query(con.get(), "Allocations by status and release type",
        "SELECT allocation_status, IFNULL(release_type, '—') AS release_type, COUNT(*) AS allocations "
        "FROM resource_allocations GROUP BY allocation_status, release_type ORDER BY allocation_status, release_type");
    return d.result({{"aggregate_functions", "COUNT, SUM, AVG, ROUND"}, {"clauses", "GROUP BY, HAVING"}});
}

inline json subqueries(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "Nested subquery — patients with at least the average number of records",
        "SELECT p.patient_id, p.name, COUNT(r.record_id) AS records FROM patients p "
        "JOIN patient_records r ON r.patient_id = p.patient_id GROUP BY p.patient_id, p.name "
        "HAVING COUNT(r.record_id) >= (SELECT AVG(c) FROM (SELECT COUNT(*) AS c FROM patient_records GROUP BY patient_id) AS per_patient) "
        "ORDER BY records DESC LIMIT 20");
    d.query(con.get(), "Correlated subquery — latest record of every patient",
        "SELECT r.patient_id, r.record_id, r.admission_status, r.record_date FROM patient_records r "
        "WHERE r.record_date = (SELECT MAX(r2.record_date) FROM patient_records r2 WHERE r2.patient_id = r.patient_id) "
        "ORDER BY r.patient_id LIMIT 20");
    d.query(con.get(), "EXISTS — patients currently holding resources",
        "SELECT p.patient_id, p.name FROM patients p WHERE EXISTS "
        "(SELECT 1 FROM resource_allocations a WHERE a.patient_id = p.patient_id AND a.allocation_status = 'Active') LIMIT 20");
    d.query(con.get(), "NOT IN — beds that have never been allocated",
        "SELECT bed_number, ward, bed_status FROM beds WHERE bed_id NOT IN "
        "(SELECT bed_id FROM resource_allocations WHERE bed_id IS NOT NULL) ORDER BY bed_number");
    return d.result({{"kinds", "nested, correlated, EXISTS, NOT IN"}});
}

inline json setOperations(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "UNION ALL — every hospital resource in one list",
        "SELECT 'Bed' AS type, bed_number AS name, bed_status AS status FROM beds "
        "UNION ALL SELECT 'Staff', staff_name, staff_status FROM staff "
        "UNION ALL SELECT 'Equipment', equipment_name, equipment_status FROM equipment ORDER BY type, name");
    d.query(con.get(), "UNION — distinct statuses used across resources (duplicates removed)",
        "SELECT bed_status AS status FROM beds UNION SELECT staff_status FROM staff UNION SELECT equipment_status FROM equipment");
    string intersect = "SELECT patient_id FROM emergency_queue INTERSECT SELECT patient_id FROM patient_records ORDER BY patient_id";
    try {
        d.query(con.get(), "INTERSECT — patients who were queued AND have medical records", intersect);
    } catch (const sql::SQLException&) {
        d.note("MySQL", "INTERSECT needs MySQL 8.0.31+; using the equivalent IN subquery");
        d.query(con.get(), "INTERSECT (emulated) — patients who were queued AND have medical records",
            "SELECT DISTINCT patient_id FROM emergency_queue WHERE patient_id IN (SELECT patient_id FROM patient_records) ORDER BY patient_id");
    }
    string except = "SELECT patient_id FROM patients EXCEPT SELECT patient_id FROM patient_records ORDER BY patient_id";
    try {
        d.query(con.get(), "EXCEPT — patients with no medical record yet", except);
    } catch (const sql::SQLException&) {
        d.note("MySQL", "EXCEPT needs MySQL 8.0.31+; using the equivalent NOT EXISTS subquery");
        d.query(con.get(), "EXCEPT (emulated) — patients with no medical record yet",
            "SELECT p.patient_id FROM patients p WHERE NOT EXISTS (SELECT 1 FROM patient_records r WHERE r.patient_id = p.patient_id) ORDER BY p.patient_id");
    }
    return d.result({{"operations", "UNION ALL, UNION, INTERSECT, EXCEPT"}});
}

inline json views(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    json q = d.query(con.get(), "SELECT * FROM v_active_queue", "SELECT * FROM v_active_queue ORDER BY priority, arrival_time");
    d.query(con.get(), "SELECT * FROM v_resource_utilization", "SELECT * FROM v_resource_utilization");
    d.query(con.get(), "Views stored in the data dictionary",
        "SELECT TABLE_NAME AS view_name, IS_UPDATABLE AS updatable, CHECK_OPTION AS check_option FROM information_schema.VIEWS "
        "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_NAME");
    d.note("MySQL", "A view stores the query, not the data: it always reflects the current tables");
    return d.result({{"active_queue_rows", q.size()}});
}

inline json storedRoutines(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "Stored FUNCTION fn_priority_label()",
        "SELECT p AS priority, fn_priority_label(p) AS label FROM (SELECT 1 AS p UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) AS levels");
    string call1 = "CALL sp_hospital_summary()";
    d.note("A", call1);
    d.table("Stored PROCEDURE sp_hospital_summary()", callProcedure(con.get(), call1), call1);
    d.exec(con.get(), "CALL sp_count_by_priority(1, @critical_count)");
    d.query(con.get(), "Procedure with IN / OUT parameters", "SELECT @critical_count AS critical_patients_waiting");
    d.query(con.get(), "Routines stored in the database",
        "SELECT ROUTINE_NAME AS routine_name, ROUTINE_TYPE AS type, IS_DETERMINISTIC AS is_deterministic, SQL_DATA_ACCESS AS data_access "
        "FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() ORDER BY ROUTINE_TYPE, ROUTINE_NAME");
    return d.result({{"function", "fn_priority_label"}, {"procedures", "sp_hospital_summary, sp_count_by_priority"}});
}

inline json triggers(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "Triggers installed",
        "SELECT TRIGGER_NAME AS trigger_name, ACTION_TIMING AS timing, EVENT_MANIPULATION AS event, EVENT_OBJECT_TABLE AS on_table "
        "FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = DATABASE() ORDER BY EVENT_OBJECT_TABLE, TRIGGER_NAME");
    // Live proof inside a transaction that is rolled back afterwards.
    d.startTx(con.get());
    d.exec(con.get(), "INSERT INTO patients (name) VALUES ('Trigger demo (rolled back)')");
    long long pid = queryRaw(con.get(), "SELECT LAST_INSERT_ID() AS id")[0]["id"].get<long long>();
    d.exec(con.get(), "INSERT INTO emergency_queue (patient_id, priority) VALUES (" + std::to_string(pid) + ", 2)");
    d.exec(con.get(), "UPDATE emergency_queue SET queue_status = 'In Treatment' WHERE patient_id = " + std::to_string(pid));
    json fired = d.query(con.get(), "Rows the triggers wrote (inside the transaction)",
        "SELECT audit_id, table_name, action, record_id, details FROM audit_log WHERE details LIKE 'Patient #" + std::to_string(pid) + "%' ORDER BY audit_id");
    d.rollback(con.get());
    json after = d.silent(con.get(), "SELECT COUNT(*) AS n FROM audit_log WHERE details LIKE 'Patient #" + std::to_string(pid) + "%'");
    d.note("A", "After ROLLBACK the trigger rows are gone too: triggers run inside the same transaction (atomicity)");
    d.query(con.get(), "Latest real audit trail (written by triggers while staff use the app)",
        "SELECT audit_id, table_name, action, record_id, details, changed_at FROM audit_log ORDER BY audit_id DESC LIMIT 25");
    return d.result({{"trigger_rows_inside_tx", fired.size()}, {"trigger_rows_after_rollback", after[0]["n"]}});
}

inline json indexing(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.query(con.get(), "Indexes (B+ tree) on the busiest tables",
        "SELECT TABLE_NAME AS table_name, INDEX_NAME AS index_name, GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX SEPARATOR ', ') AS columns_in_order, "
        "IF(MIN(NON_UNIQUE) = 0, 'UNIQUE', 'NON-UNIQUE') AS uniqueness, MIN(INDEX_TYPE) AS structure "
        "FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME IN ('emergency_queue','resource_allocations','patient_records','patients') "
        "GROUP BY TABLE_NAME, INDEX_NAME ORDER BY TABLE_NAME, INDEX_NAME");
    d.query(con.get(), "EXPLAIN — emergency queue query used by GET /api/queue",
        "EXPLAIN SELECT queue_id, patient_id, priority FROM emergency_queue "
        "WHERE queue_status IN ('Waiting','In Treatment') ORDER BY priority, arrival_time");
    d.query(con.get(), "EXPLAIN — query the auto-release timer thread runs every few seconds",
        "EXPLAIN SELECT allocation_id FROM resource_allocations WHERE allocation_status = 'Active' "
        "AND release_due_at IS NOT NULL AND release_due_at <= NOW() ORDER BY release_due_at");
    d.query(con.get(), "EXPLAIN — primary-key lookup (GET /api/patients/:id)",
        "EXPLAIN SELECT * FROM patients WHERE patient_id = 1");
    d.note("MySQL", "type=const/ref/range = index used; type=ALL = full table scan. On tiny tables MySQL may still prefer a scan.");
    return d.result({{"custom_indexes", "idx_queue_status_priority, idx_alloc_status_due, idx_records_patient_date, idx_patients_name"}});
}

inline json transactions(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    auto freeBeds = [&](const string& when) {
        json r = d.silent(con.get(), "SELECT COUNT(*) AS n FROM beds WHERE bed_status = 'Available'");
        long long n = r[0]["n"];
        d.note("A", "Available beds " + when + ": " + std::to_string(n));
        return n;
    };
    json free = queryRaw(con.get(), "SELECT bed_id, bed_number FROM beds WHERE bed_status = 'Available' ORDER BY bed_id LIMIT 2");
    if (free.size() < 2) throw ApiError{409, "The transaction demo needs at least 2 available beds."};
    string b1 = std::to_string(free[0]["bed_id"].get<long long>()), b2 = std::to_string(free[1]["bed_id"].get<long long>());

    long long before = freeBeds("before the transaction");
    d.startTx(con.get());
    d.exec(con.get(), "UPDATE beds SET bed_status = 'Maintenance' WHERE bed_id = " + b1);
    long long step1 = freeBeds("inside the transaction (1 bed changed)");
    d.exec(con.get(), "SAVEPOINT after_first_bed");
    d.exec(con.get(), "UPDATE beds SET bed_status = 'Maintenance' WHERE bed_id = " + b2);
    long long step2 = freeBeds("after the 2nd change");
    d.exec(con.get(), "ROLLBACK TO SAVEPOINT after_first_bed");
    long long step3 = freeBeds("after ROLLBACK TO SAVEPOINT (only 2nd change undone)");
    d.rollback(con.get());
    long long after = freeBeds("after ROLLBACK (all changes undone)");

    auto& tx = txStats();
    d.table("ACID properties", json::array({
        {{"property", "Atomicity"}, {"proof", "All updates were undone together by ROLLBACK (" + std::to_string(before) + " -> " + std::to_string(after) + " free beds)"}},
        {{"property", "Consistency"}, {"proof", "CHECK, FOREIGN KEY and ENUM constraints are verified for every statement (see Integrity demo)"}},
        {{"property", "Isolation"}, {"proof", "Other connections never saw the uncommitted 'Maintenance' beds (see Isolation demo)"}},
        {{"property", "Durability"}, {"proof", "After COMMIT, InnoDB's redo log keeps the change even if the server crashes"}},
    }));
    d.table("Transactions run by the live app since start-up", json::array({
        {{"started", tx.started.load()}, {"committed", tx.commits.load()}, {"rolled_back", tx.rollbacks.load()},
         {"deadlock_retries", tx.deadlockRetries.load()}, {"lock_timeout_retries", tx.lockTimeoutRetries.load()}}}));
    return d.result({{"free_beds_before", before}, {"inside_tx", step1}, {"after_second_update", step2},
                     {"after_savepoint_rollback", step3}, {"after_rollback", after}, {"atomicity_ok", before == after}});
}

// Restores a session variable even if the demo throws.
struct SessionReset {
    sql::Connection* con;
    string sqlText;
    ~SessionReset() { try { executeRaw(con, sqlText); } catch (...) {} }
};

inline json isolation(ConnectionPool& db) {
    if (db.size() < 3) throw ApiError{409, "This demo needs DB_POOL_SIZE of at least 3."};
    Demo d;
    auto a = db.acquire();
    auto b = db.acquire();
    json bed = queryRaw(a.get(), "SELECT bed_id, bed_number, bed_status FROM beds WHERE bed_status = 'Available' ORDER BY bed_id LIMIT 1");
    if (bed.empty()) throw ApiError{409, "The isolation demo needs one available bed."};
    string id = std::to_string(bed[0]["bed_id"].get<long long>());
    string level = queryRaw(b.get(), "SELECT @@transaction_isolation AS level")[0]["level"];
    d.note("B", "Default isolation level: " + level);
    SessionReset reset{b.get(), "SET SESSION TRANSACTION ISOLATION LEVEL " + std::regex_replace(level, std::regex("-"), " ")};

    d.startTx(a.get(), "A");
    d.exec(a.get(), "UPDATE beds SET bed_status = 'Maintenance' WHERE bed_id = " + id, "A");
    d.note("A", "Changed bed " + string(bed[0]["bed_number"]) + " to 'Maintenance' but has NOT committed");

    json rc = d.silent(b.get(), "SELECT bed_status FROM beds WHERE bed_id = " + id, "B");
    d.note("B", "Under " + level + " reads '" + string(rc[0]["bed_status"]) + "' -> no dirty read (MVCC snapshot)");

    d.exec(b.get(), "SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED", "B");
    json ru = d.silent(b.get(), "SELECT bed_status FROM beds WHERE bed_id = " + id, "B");
    d.note("B", "Under READ UNCOMMITTED reads '" + string(ru[0]["bed_status"]) + "' -> DIRTY READ of uncommitted data");

    d.rollback(a.get(), "A");
    json fin = d.silent(b.get(), "SELECT bed_status FROM beds WHERE bed_id = " + id, "B");
    d.note("B", "After A rolls back the bed is '" + string(fin[0]["bed_status"]) + "' again — the dirty value never existed");

    d.table("Isolation levels and the anomalies they prevent", json::array({
        {{"level", "READ UNCOMMITTED"}, {"dirty_read", "possible"}, {"non_repeatable_read", "possible"}, {"phantom", "possible"}},
        {{"level", "READ COMMITTED"}, {"dirty_read", "prevented"}, {"non_repeatable_read", "possible"}, {"phantom", "possible"}},
        {{"level", "REPEATABLE READ (MySQL default)"}, {"dirty_read", "prevented"}, {"non_repeatable_read", "prevented"}, {"phantom", "prevented by next-key locks"}},
        {{"level", "SERIALIZABLE"}, {"dirty_read", "prevented"}, {"non_repeatable_read", "prevented"}, {"phantom", "prevented"}},
    }));
    return d.result({{"default_level", level}, {"seen_by_default_level", rc[0]["bed_status"]},
                     {"seen_by_read_uncommitted", ru[0]["bed_status"]}, {"after_rollback", fin[0]["bed_status"]}});
}

inline json locking(ConnectionPool& db) {
    if (db.size() < 3) throw ApiError{409, "This demo needs DB_POOL_SIZE of at least 3."};
    Demo d;
    auto a = db.acquire();
    auto b = db.acquire();
    json bed = queryRaw(a.get(), "SELECT bed_id, bed_number FROM beds ORDER BY bed_id LIMIT 1");
    if (bed.empty()) throw ApiError{409, "The locking demo needs at least one bed."};
    string id = std::to_string(bed[0]["bed_id"].get<long long>());
    string sel = "SELECT bed_id, bed_status FROM beds WHERE bed_id = " + id;

    d.startTx(a.get(), "A");
    d.silent(a.get(), sel + " FOR UPDATE", "A");
    d.note("A", "Holds an EXCLUSIVE (X) row lock on bed " + string(bed[0]["bed_number"]) + " — exactly what POST /api/allocations does");

    d.startTx(b.get(), "B");
    int x = d.attempt(b.get(), sel + " FOR UPDATE NOWAIT", "B");
    int s = d.attempt(b.get(), sel + " FOR SHARE NOWAIT", "B");
    int plain = d.attempt(b.get(), sel, "B");
    d.note("B", "A plain SELECT is a non-locking consistent read, so it is never blocked");

    d.rollback(a.get(), "A");
    int after = d.attempt(b.get(), sel + " FOR UPDATE NOWAIT", "B");
    d.rollback(b.get(), "B");

    d.table("Lock compatibility", json::array({
        {{"held_by_A", "X (FOR UPDATE)"}, {"B_requests", "X (FOR UPDATE)"}, {"result", x ? "blocked (error " + std::to_string(x) + ")" : "granted"}},
        {{"held_by_A", "X (FOR UPDATE)"}, {"B_requests", "S (FOR SHARE)"}, {"result", s ? "blocked (error " + std::to_string(s) + ")" : "granted"}},
        {{"held_by_A", "X (FOR UPDATE)"}, {"B_requests", "plain SELECT"}, {"result", plain ? "error" : "granted (MVCC snapshot)"}},
        {{"held_by_A", "none (A rolled back)"}, {"B_requests", "X (FOR UPDATE)"}, {"result", after ? "error" : "granted"}},
    }));
    return d.result({{"exclusive_blocked", x != 0}, {"shared_blocked", s != 0}, {"plain_read_blocked", plain != 0}, {"granted_after_release", after == 0}});
}

inline json dbDeadlock(ConnectionPool& db) {
    if (db.size() < 3) throw ApiError{409, "This demo needs DB_POOL_SIZE of at least 3."};
    Demo d;
    auto a = db.acquire();
    auto b = db.acquire();
    json beds = queryRaw(a.get(), "SELECT bed_id, bed_number FROM beds ORDER BY bed_id LIMIT 2");
    if (beds.size() < 2) throw ApiError{409, "The deadlock demo needs at least 2 beds."};
    string b1 = std::to_string(beds[0]["bed_id"].get<long long>()), b2 = std::to_string(beds[1]["bed_id"].get<long long>());
    string n1 = beds[0]["bed_number"], n2 = beds[1]["bed_number"];
    SessionReset ra{a.get(), "SET SESSION innodb_lock_wait_timeout = DEFAULT"};
    SessionReset rb{b.get(), "SET SESSION innodb_lock_wait_timeout = DEFAULT"};
    d.exec(a.get(), "SET SESSION innodb_lock_wait_timeout = 5", "A");
    d.exec(b.get(), "SET SESSION innodb_lock_wait_timeout = 5", "B");

    d.startTx(a.get(), "A");
    d.startTx(b.get(), "B");
    d.silent(a.get(), "SELECT bed_id FROM beds WHERE bed_id = " + b1 + " FOR UPDATE", "A");
    d.note("A", "locked bed " + n1);
    d.silent(b.get(), "SELECT bed_id FROM beds WHERE bed_id = " + b2 + " FOR UPDATE", "B");
    d.note("B", "locked bed " + n2);

    int codeA = 0, codeB = 0;
    // Connection A runs on its own OS thread so it can block while B continues.
    std::thread ta([&] {
        d.note("A", "requests bed " + n2 + " (held by B) -> waits");
        codeA = d.attempt(a.get(), "SELECT bed_id FROM beds WHERE bed_id = " + b2 + " FOR UPDATE", "A");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    d.note("B", "requests bed " + n1 + " (held by A) -> circular wait");
    codeB = d.attempt(b.get(), "SELECT bed_id FROM beds WHERE bed_id = " + b1 + " FOR UPDATE", "B");
    ta.join();
    string victim = codeA == ER_LOCK_DEADLOCK ? "A" : codeB == ER_LOCK_DEADLOCK ? "B" : "none";
    if (victim != "none")
        d.note("InnoDB", "Deadlock detected in the wait-for graph. Victim: connection " + victim +
               " (rolled back); the other transaction got its lock and continued");
    else
        d.note("InnoDB", "No deadlock error (innodb_deadlock_detect may be OFF; lock wait timeout used instead)");
    d.rollback(a.get(), "A");
    d.rollback(b.get(), "B");
    d.note("App", "withTransaction() in db_pool.hpp catches error 1213, rolls back and retries — deadlock recovery");

    d.table("Wait-for graph", json::array({
        {{"transaction", "A"}, {"holds", "bed " + n1}, {"waits_for", "bed " + n2 + " (held by B)"}, {"error", codeA}},
        {{"transaction", "B"}, {"holds", "bed " + n2}, {"waits_for", "bed " + n1 + " (held by A)"}, {"error", codeB}},
    }));
    return d.result({{"deadlock_detected", victim != "none"}, {"victim", victim}, {"error_code", victim != "none" ? ER_LOCK_DEADLOCK : std::max(codeA, codeB)},
                     {"live_app_deadlock_retries", txStats().deadlockRetries.load()}});
}

inline json integrity(ConnectionPool& db) {
    Demo d;
    auto con = db.acquire();
    d.startTx(con.get());
    d.exec(con.get(), "INSERT INTO patients (name) VALUES ('Integrity demo (rolled back)')");
    string pid = std::to_string(queryRaw(con.get(), "SELECT LAST_INSERT_ID() AS id")[0]["id"].get<long long>());
    d.exec(con.get(), "INSERT INTO beds (bed_number, ward, bed_type) VALUES ('INTEGRITY-DEMO', 'Demo', 'Standard')");
    d.exec(con.get(), "INSERT INTO resource_allocations (patient_id) VALUES (" + pid + ")");

    struct Case { string constraint, sqlText, meaning; };
    std::vector<Case> cases = {
        {"CHECK (priority 1-4)", "INSERT INTO emergency_queue (patient_id, priority) VALUES (" + pid + ", 9)", "Domain integrity"},
        {"FOREIGN KEY", "INSERT INTO patient_records (patient_id, symptoms) VALUES (999999999, 'demo')", "Referential integrity: parent must exist"},
        {"ON DELETE RESTRICT", "DELETE FROM patients WHERE patient_id = " + pid, "Cannot delete a patient who still has allocations"},
        {"UNIQUE (bed_number)", "INSERT INTO beds (bed_number, ward, bed_type) VALUES ('INTEGRITY-DEMO', 'Demo', 'Standard')", "Key integrity: no duplicates"},
        {"PRIMARY KEY", "INSERT INTO patients (patient_id, name) VALUES (" + pid + ", 'duplicate id')", "Entity integrity: unique, not null"},
        {"NOT NULL", "INSERT INTO emergency_queue (patient_id, priority) VALUES (NULL, 1)", "Required value"},
        {"CHECK (available <= total)", "INSERT INTO equipment (equipment_name, total_quantity, available_quantity) VALUES ('Demo', 1, 5)", "Business rule as a constraint"},
        {"ENUM domain", "UPDATE beds SET bed_status = 'Broken' WHERE bed_number = 'INTEGRITY-DEMO'", "Only listed values allowed (strict SQL mode)"},
    };
    json rows = json::array();
    int rejected = 0;
    for (auto& c : cases) {
        int code = d.attempt(con.get(), c.sqlText);
        if (code) ++rejected;
        rows.push_back({{"constraint", c.constraint}, {"meaning", c.meaning}, {"mysql_error", code},
                        {"result", code ? "REJECTED" : "accepted"}});
    }
    d.rollback(con.get());
    d.note("A", "Everything rolled back — no demo data left behind");
    d.table("Constraint checks", rows);
    return d.result({{"violations_attempted", cases.size()}, {"rejected_by_mysql", rejected}});
}

// =====================================================================================
// Catalog + dispatcher
// =====================================================================================
inline json catalog() {
    auto c = [](const string& id, const string& cat, const string& title, const string& desc, const string& where) {
        return json{{"id", id}, {"category", cat}, {"title", title}, {"description", desc}, {"where_used", where}, {"params", json::array()}};
    };
    return json::array({
        c("keys_constraints", "Data model", "Keys & constraints", "Primary, foreign, unique and CHECK constraints read live from MySQL's data dictionary.", "Every table in database/project_schema.sql."),
        c("relationships", "Data model", "ER model -> relational schema", "How the patient, resource and allocation entities map to tables, with cardinality and ON DELETE / ON UPDATE actions.", "patients 1:N patient_records, emergency_queue, resource_allocations."),
        c("normalization", "Data model", "Normalization (1NF to BCNF)", "Shows each relation's attributes, proves patient data is stored only once and rebuilds it with a lossless join.", "Patient details live only in patients; other tables keep patient_id."),
        c("joins", "SQL queries", "Joins", "INNER, LEFT OUTER, multi-table and SELF joins on the hospital data.", "GET /api/queue (INNER JOIN), GET /api/resources (multi-table LEFT JOIN)."),
        c("aggregation", "SQL queries", "Aggregation, GROUP BY & HAVING", "COUNT, SUM, AVG and computed columns grouped by priority, ward and status.", "GET /api/dashboard counts."),
        c("subqueries", "SQL queries", "Subqueries", "Nested, correlated, EXISTS and NOT IN subqueries.", "POST /api/queue checks for an existing entry before inserting."),
        c("set_operations", "SQL queries", "Set operations", "UNION ALL, UNION, INTERSECT and EXCEPT (with fallbacks for older MySQL).", "One combined resource list."),
        c("views", "Database objects", "Views", "v_active_queue and v_resource_utilization, created by the C++ server at start-up.", "Same logic as the active-queue query in main.cpp."),
        c("stored_routines", "Database objects", "Stored function & procedures", "fn_priority_label(), sp_hospital_summary() and sp_count_by_priority(IN, OUT).", "Priority labels and dashboard summary."),
        c("triggers", "Database objects", "Triggers & audit trail", "AFTER triggers write every queue, allocation and record change into audit_log; a BEFORE trigger derives equipment status. Proven inside a rolled-back transaction.", "Fires on every real change made from the frontend."),
        c("indexing", "Storage & performance", "Indexing & EXPLAIN", "B+ tree indexes on the hottest columns and the execution plans MySQL picks.", "Queue ordering and the auto-release timer query."),
        c("transactions", "Transactions", "Transactions, ACID & savepoints", "Changes beds inside a transaction, rolls back to a SAVEPOINT, then rolls back everything.", "withTransaction() wraps every multi-step write."),
        c("isolation", "Transactions", "Isolation levels & dirty reads", "Two connections: B cannot see A's uncommitted change under REPEATABLE READ, but can under READ UNCOMMITTED.", "InnoDB MVCC protects every API read."),
        c("locking", "Concurrency control", "Row-level locking", "Exclusive (FOR UPDATE) and shared (FOR SHARE) locks, NOWAIT conflicts and non-locking reads.", "POST /api/allocations locks patient -> bed -> staff -> equipment."),
        c("db_deadlock", "Concurrency control", "Database deadlock detection", "Two transactions lock two beds in opposite order. InnoDB detects the cycle and rolls back a victim (error 1213).", "withTransaction() retries on 1213 — deadlock recovery."),
        c("integrity", "Concurrency control", "Integrity enforcement", "Tries to break CHECK, FOREIGN KEY, UNIQUE, PRIMARY KEY, NOT NULL and ENUM rules — MySQL rejects each one.", "safe() turns 1062 / 1452 into friendly messages."),
    });
}

inline json runDbmsDemo(const string& id, ConnectionPool& db) {
    static const std::map<string, std::function<json(ConnectionPool&)>> demos = {
        {"keys_constraints", keysConstraints}, {"relationships", relationships}, {"normalization", normalization},
        {"joins", joins}, {"aggregation", aggregation}, {"subqueries", subqueries}, {"set_operations", setOperations},
        {"views", views}, {"stored_routines", storedRoutines}, {"triggers", triggers}, {"indexing", indexing},
        {"transactions", transactions}, {"isolation", isolation}, {"locking", locking}, {"db_deadlock", dbDeadlock},
        {"integrity", integrity},
    };
    auto it = demos.find(id);
    if (it == demos.end()) throw ApiError{404, "Unknown DBMS concept: " + id};
    return it->second(db);
}

}  // namespace dbmslab
