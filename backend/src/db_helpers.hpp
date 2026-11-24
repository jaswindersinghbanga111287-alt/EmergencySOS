// Shared MySQL helpers used by main.cpp and dbms_concepts.hpp.
#pragma once
#include <mysql/jdbc.h>

#include <memory>
#include <string>

#include "json.hpp"

using json = nlohmann::ordered_json;  // keeps keys in insertion order (SQL column order)
using std::string;

// ---------------------------------------------------------------- helpers

struct ApiError {
    int status;
    string message;
};

inline string priorityLabel(long long p) {
    switch (p) {
        case 1: return "Critical";
        case 2: return "High";
        case 3: return "Medium";
        case 4: return "Low";
        default: return "Unknown";
    }
}

// Converts every row of a result set into a JSON array, using column types.
inline json rowsToJson(sql::ResultSet* rs) {
    json rows = json::array();
    sql::ResultSetMetaData* meta = rs->getMetaData();
    unsigned int cols = meta->getColumnCount();
    while (rs->next()) {
        json row = json::object();
        for (unsigned int i = 1; i <= cols; ++i) {
            string name = meta->getColumnLabel(i);
            if (rs->isNull(i)) { row[name] = nullptr; continue; }
            switch (meta->getColumnType(i)) {
                case sql::DataType::BIT:
                case sql::DataType::TINYINT:
                case sql::DataType::SMALLINT:
                case sql::DataType::MEDIUMINT:
                case sql::DataType::INTEGER:
                case sql::DataType::BIGINT:
                    row[name] = static_cast<long long>(rs->getInt64(i));
                    break;
                case sql::DataType::REAL:
                case sql::DataType::DOUBLE:
                case sql::DataType::DECIMAL:
                case sql::DataType::NUMERIC:
                    row[name] = static_cast<double>(rs->getDouble(i));
                    break;
                default:
                    row[name] = string(rs->getString(i));
            }
        }
        rows.push_back(row);
    }
    return rows;
}

// Runs a prepared SELECT. `bind` sets the ? parameters.
template <typename Bind>
inline json query(sql::Connection* con, const string& sqlText, Bind bind) {
    std::unique_ptr<sql::PreparedStatement> st(con->prepareStatement(sqlText));
    bind(st.get());
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    return rowsToJson(rs.get());
}
inline json query(sql::Connection* con, const string& sqlText) {
    return query(con, sqlText, [](sql::PreparedStatement*) {});
}

template <typename Bind>
inline void execute(sql::Connection* con, const string& sqlText, Bind bind) {
    std::unique_ptr<sql::PreparedStatement> st(con->prepareStatement(sqlText));
    bind(st.get());
    st->executeUpdate();
}

inline long long lastInsertId(sql::Connection* con) {
    return query(con, "SELECT LAST_INSERT_ID() AS id")[0]["id"].get<long long>();
}

inline long long countOf(sql::Connection* con, const string& sqlText) {
    return query(con, sqlText)[0]["n"].get<long long>();
}

// Runs a plain (non-prepared) SQL statement that returns rows, e.g. EXPLAIN or SHOW.
inline json queryRaw(sql::Connection* con, const string& sqlText) {
    std::unique_ptr<sql::Statement> st(con->createStatement());
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery(sqlText));
    return rowsToJson(rs.get());
}

// Runs a plain SQL statement that returns no rows (DDL, SAVEPOINT, SET ...).
// Any rows a statement returns are read and discarded, so the connection stays in sync
// (MySQL streams results; unread rows cause "Commands out of sync").
inline void executeRaw(sql::Connection* con, const string& sqlText) {
    std::unique_ptr<sql::Statement> st(con->createStatement());
    if (st->execute(sqlText)) {
        do {
            std::unique_ptr<sql::ResultSet> rs(st->getResultSet());
            if (rs) while (rs->next()) {}
        } while (st->getMoreResults());
    }
}
