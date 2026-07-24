// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/logging.cpp
// Purpose:       Implementation of the timestamped stderr logger.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "logging.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace starbase {
namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

// UTC, ISO-8601, with milliseconds. Everything this project stores or prints is
// UTC; local time appears only where a human explicitly asks for it.
std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    gmtime_r(&secs, &tm);

    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
       << std::setw(3) << ms.count() << 'Z';
    return os.str();
}

}  // namespace

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel log_level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

LogLevel log_level_from_string(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    std::transform(s.begin(), s.end(), std::back_inserter(v),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "debug") return LogLevel::Debug;
    if (v == "warn" || v == "warning") return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    return LogLevel::Info;
}

void log_msg(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;
    std::cerr << timestamp() << " [" << level_name(level) << "] " << msg << std::endl;
}

}  // namespace starbase
