// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/config_test.cpp
// Purpose:       Unit tests for the INI-style configuration parser.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "config.hpp"
#include "logging.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

std::string write_temp(const std::string& body) {
    std::string path = "/tmp/starbase_config_test_" + std::to_string(::getpid()) + ".conf";
    std::ofstream out(path);
    out << body;
    return path;
}

void test_defaults_and_overrides() {
    const std::string path = write_temp(
        "[daemon]\n"
        "log_level = debug   # trailing comment\n"
        "\n"
        "; semicolon comment\n"
        "[database]\n"
        "host = db.example.com:3307\n"
        "name = starbase\n"
        "\n"
        "[api]\n"
        "port = 9090\n"
        "tls = on\n"
        "\n"
        "[scanner]\n"
        "threads = 12\n"
        "\n"
        "[actions]\n"
        "link_mode = hardlink\n");

    const auto cfg = starbase::Config::load(path);
    check(cfg.log_level == "debug", "log_level parsed");
    // A host:port suffix must override the separate port setting.
    check(cfg.db_host == "db.example.com", "db host split from host:port");
    check(cfg.db_port == 3307, "db port taken from host:port suffix");
    check(cfg.db_name == "starbase", "db name parsed");
    check(cfg.db_user == "starbase", "db user default retained");
    check(cfg.api_port == 9090, "api port parsed");
    check(cfg.api_tls, "tls = on parsed as true");
    check(cfg.scanner_threads == 12, "scanner threads parsed");
    check(cfg.link_mode == "hardlink", "link_mode parsed");
    // Untouched sections keep their defaults.
    check(cfg.default_settle_seconds == 30, "settle default retained");
    std::remove(path.c_str());
}

void test_password_in_file_is_rejected() {
    // A password in the config file is a security regression, not a convenience.
    const std::string path = write_temp("[database]\npassword = hunter2\n");
    bool threw = false;
    try {
        starbase::Config::load(path);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "a password key in [database] is rejected");
    std::remove(path.c_str());
}

void test_invalid_values_rejected() {
    const std::string bad_bool = write_temp("[api]\ntls = maybe\n");
    bool threw = false;
    try { starbase::Config::load(bad_bool); } catch (const std::exception&) { threw = true; }
    check(threw, "non-boolean tls value is rejected");
    std::remove(bad_bool.c_str());

    const std::string bad_mode = write_temp("[actions]\nlink_mode = teleport\n");
    threw = false;
    try { starbase::Config::load(bad_mode); } catch (const std::exception&) { threw = true; }
    check(threw, "unknown link_mode is rejected");
    std::remove(bad_mode.c_str());

    const std::string bad_line = write_temp("[api]\nthis line has no equals sign\n");
    threw = false;
    try { starbase::Config::load(bad_line); } catch (const std::exception&) { threw = true; }
    check(threw, "malformed line is rejected");
    std::remove(bad_line.c_str());
}

void test_missing_file_throws() {
    bool threw = false;
    try {
        starbase::Config::load("/nonexistent/starbase.conf");
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "missing config file throws");
}

void test_unknown_keys_ignored() {
    // Forward compatibility: a newer config must not stop an older daemon.
    const std::string path = write_temp(
        "[api]\nport = 8080\nsome_future_key = value\n"
        "[not_a_section_we_know]\nwhatever = 1\n");
    bool threw = false;
    try { starbase::Config::load(path); } catch (const std::exception&) { threw = true; }
    check(!threw, "unknown sections and keys are ignored");
    std::remove(path.c_str());
}

void test_log_level_parsing() {
    using starbase::LogLevel;
    check(starbase::log_level_from_string("debug") == LogLevel::Debug, "log level debug");
    check(starbase::log_level_from_string("WARNING") == LogLevel::Warn, "log level warning (case)");
    check(starbase::log_level_from_string("error") == LogLevel::Error, "log level error");
    check(starbase::log_level_from_string("nonsense") == LogLevel::Info, "unknown level falls back to info");
}

}  // namespace

int main() {
    test_defaults_and_overrides();
    test_password_in_file_is_rejected();
    test_invalid_values_rejected();
    test_missing_file_throws();
    test_unknown_keys_ignored();
    test_log_level_parsing();

    if (g_failures == 0) {
        std::cout << "config_test: all checks passed\n";
        return 0;
    }
    std::cerr << "config_test: " << g_failures << " check(s) failed\n";
    return 1;
}
