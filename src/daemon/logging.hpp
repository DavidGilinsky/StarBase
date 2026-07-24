// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/logging.hpp
// Purpose:       Minimal thread-safe, timestamped logging to stderr (captured by
//                journald when running under systemd).
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase {

enum class LogLevel { Debug, Info, Warn, Error };

// Messages below the configured level are dropped. Default is Info.
void set_log_level(LogLevel level);
LogLevel log_level();

// Parse "debug" | "info" | "warning"/"warn" | "error"; unknown values yield Info.
LogLevel log_level_from_string(const std::string& s);

void log_msg(LogLevel level, const std::string& msg);

inline void log_debug(const std::string& m) { log_msg(LogLevel::Debug, m); }
inline void log_info(const std::string& m) { log_msg(LogLevel::Info, m); }
inline void log_warn(const std::string& m) { log_msg(LogLevel::Warn, m); }
inline void log_error(const std::string& m) { log_msg(LogLevel::Error, m); }

}  // namespace starbase
