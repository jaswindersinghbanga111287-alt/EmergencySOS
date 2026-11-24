// Reads settings from backend/.env (if present) and environment variables.
// Environment variables win over the .env file.
#pragma once
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

struct Config {
    std::string dbHost = "127.0.0.1";
    int dbPort = 3306;
    std::string dbUser = "root";
    std::string dbPassword="1";
    std::string dbName = "emergency_sos";
    int dbPoolSize = 10;
    int port = 8080;

    // Severity-based release timers (minutes). When a timer runs out, the
    // background thread releases the allocation automatically.
    int releaseMinutesCritical = 240;
    int releaseMinutesHigh = 120;
    int releaseMinutesMedium = 60;
    int releaseMinutesLow = 30;
    // How often the background thread looks for expired timers (seconds).
    int autoReleaseCheckSeconds = 15;

    int releaseMinutesFor(long long priority) const {
        switch (priority) {
            case 1: return releaseMinutesCritical;
            case 2: return releaseMinutesHigh;
            case 3: return releaseMinutesMedium;
            default: return releaseMinutesLow;
        }
    }
};

inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

inline std::map<std::string, std::string> readEnvFile(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        out[key] = value;
    }
    return out;
}

inline Config loadConfig() {
    // Look for .env in the current folder and in the parent (handy when running from build/).
    auto file = readEnvFile(".env");
    if (file.empty()) file = readEnvFile("../.env");

    auto get = [&](const char* key, const std::string& fallback) {
        if (const char* v = std::getenv(key)) return std::string(v);
        auto it = file.find(key);
        return it != file.end() ? it->second : fallback;
    };
    auto getInt = [&](const char* key, int fallback) {
        try { return std::stoi(get(key, std::to_string(fallback))); } catch (...) { return fallback; }
    };

    Config c;
    c.dbHost = get("DB_HOST", c.dbHost);
    c.dbPort = getInt("DB_PORT", c.dbPort);
    c.dbUser = get("DB_USER", c.dbUser);
    c.dbPassword = get("DB_PASSWORD", c.dbPassword);
    c.dbName = get("DB_NAME", c.dbName);
    c.dbPoolSize = getInt("DB_POOL_SIZE", c.dbPoolSize);
    c.port = getInt("PORT", c.port);
    c.releaseMinutesCritical = getInt("RELEASE_MINUTES_CRITICAL", c.releaseMinutesCritical);
    c.releaseMinutesHigh = getInt("RELEASE_MINUTES_HIGH", c.releaseMinutesHigh);
    c.releaseMinutesMedium = getInt("RELEASE_MINUTES_MEDIUM", c.releaseMinutesMedium);
    c.releaseMinutesLow = getInt("RELEASE_MINUTES_LOW", c.releaseMinutesLow);
    c.autoReleaseCheckSeconds = getInt("AUTO_RELEASE_CHECK_SECONDS", c.autoReleaseCheckSeconds);
    if (c.dbPoolSize < 1) c.dbPoolSize = 1;
    for (int* m : {&c.releaseMinutesCritical, &c.releaseMinutesHigh, &c.releaseMinutesMedium, &c.releaseMinutesLow})
        if (*m < 1) *m = 1;
    if (c.autoReleaseCheckSeconds < 1) c.autoReleaseCheckSeconds = 1;
    return c;
}
