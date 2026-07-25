// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/config.cpp
// Purpose:       INI-style configuration parser for starbased.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace starbase {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Accepts on/off, true/false, yes/no, 1/0.
bool to_bool(const std::string& v, const std::string& where) {
    const std::string s = lower(v);
    if (s == "on" || s == "true" || s == "yes" || s == "1") return true;
    if (s == "off" || s == "false" || s == "no" || s == "0") return false;
    throw std::runtime_error(where + ": expected a boolean, got '" + v + "'");
}

int to_int(const std::string& v, const std::string& where) {
    try {
        size_t pos = 0;
        const int n = std::stoi(v, &pos);
        if (pos != v.size()) throw std::invalid_argument("trailing characters");
        return n;
    } catch (const std::exception&) {
        throw std::runtime_error(where + ": expected an integer, got '" + v + "'");
    }
}

}  // namespace

Config Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    Config cfg;
    std::string section;
    std::string line;
    int lineno = 0;

    while (std::getline(in, line)) {
        ++lineno;

        // Strip comments. A '#' or ';' anywhere starts one; values needing those
        // characters are not a case this format has to serve.
        const auto hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        const std::string where = path + ":" + std::to_string(lineno);

        if (line.front() == '[') {
            if (line.back() != ']')
                throw std::runtime_error(where + ": malformed section header");
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error(where + ": expected 'key = value'");

        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));

        if (section == "daemon") {
            if (key == "log_level") cfg.log_level = lower(val);
        } else if (section == "database") {
            if (key == "host") {
                // A "host:port" suffix overrides the separate port setting, which
                // is how you point the daemon at a database on another machine.
                const auto colon = val.rfind(':');
                if (colon != std::string::npos && val.find(':') == colon) {
                    cfg.db_host = val.substr(0, colon);
                    cfg.db_port = to_int(val.substr(colon + 1), where);
                } else {
                    cfg.db_host = val;
                }
            } else if (key == "port") {
                cfg.db_port = to_int(val, where);
            } else if (key == "name") {
                cfg.db_name = val;
            } else if (key == "user") {
                cfg.db_user = val;
            } else if (key == "password") {
                throw std::runtime_error(
                    where +
                    ": the database password must not be stored in this file; "
                    "set SB_DB_PASSWORD in the environment instead");
            }
        } else if (section == "api") {
            if (key == "bind") cfg.api_bind = val;
            else if (key == "port") cfg.api_port = to_int(val, where);
            else if (key == "tls") cfg.api_tls = to_bool(val, where);
            else if (key == "tls_cert") cfg.api_tls_cert = val;
            else if (key == "tls_key") cfg.api_tls_key = val;
            else if (key == "schema_file") cfg.schema_file = val;
            else if (key == "seed_file") cfg.seed_file = val;
            else if (key == "web_root") cfg.web_root = val;
        } else if (section == "scanner") {
            if (key == "threads") cfg.scanner_threads = to_int(val, where);
            else if (key == "queue_depth") cfg.scanner_queue_depth = to_int(val, where);
            else if (key == "default_scan_interval_s")
                cfg.default_scan_interval_s = to_int(val, where);
            else if (key == "default_settle_seconds")
                cfg.default_settle_seconds = to_int(val, where);
            else if (key == "default_ignore_globs")
                cfg.default_ignore_globs = val;
        } else if (section == "actions") {
            if (key == "staging_root") cfg.staging_root = val;
            else if (key == "trash_root") cfg.trash_root = val;
            else if (key == "link_mode") cfg.link_mode = lower(val);
        } else if (section == "pixinsight") {
            if (key == "binary") cfg.pixinsight_binary = val;
            else if (key == "wbpp_script") cfg.wbpp_script = val;
        } else if (section == "names") {
            if (key == "sesame_enabled") cfg.sesame_enabled = (lower(val) == "on" || lower(val) == "true" || val == "1");
            else if (key == "sesame_url") cfg.sesame_url = val;
        }
        // Unknown sections and keys are ignored on purpose, so a config file
        // from a newer version does not stop an older daemon from starting.
    }

    if (cfg.link_mode != "symlink" && cfg.link_mode != "hardlink" &&
        cfg.link_mode != "copy") {
        throw std::runtime_error(path + ": [actions] link_mode must be symlink, hardlink, or copy");
    }
    if (cfg.api_port < 1 || cfg.api_port > 65535) {
        throw std::runtime_error(path + ": [api] port out of range");
    }
    if (cfg.scanner_threads < 0) {
        throw std::runtime_error(path + ": [scanner] threads must be >= 0");
    }

    return cfg;
}

}  // namespace starbase
