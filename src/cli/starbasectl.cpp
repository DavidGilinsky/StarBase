// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/starbasectl.cpp
// Purpose:       Command-line administration for StarBase: schema setup, root
//                management, scans, queries, and exports.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "config.hpp"
#include "database.hpp"
#include "fsinfo.hpp"
#include "logging.hpp"
#include "starbase/version.hpp"

namespace {

namespace db = starbase::db;

void print_help(const char* argv0) {
    std::cout
        << "starbasectl " << STARBASE_VERSION << " - StarBase administration\n\n"
        << "Usage: " << argv0 << " [--config <path>] <command> [args]\n\n"
        << "Commands:\n"
        << "  db-init                  Create any missing tables from schema.sql\n"
        << "  db-seed                  Load the default header mapping (seed.sql)\n"
        << "  db-status                Show connection, schema version, table count\n"
        << "  add-root <label> <path>  Register a directory tree to index\n"
        << "  list-roots               Show registered roots and scan status\n"
        << "  set-root <label> <k=v>.. Change a root (enabled, writable, watch_mode,\n"
        << "                           scan_interval_s, settle_seconds)\n"
        << "  remove-root <label>      Unregister a root (indexed rows are removed)\n"
        << "  version                  Print version and exit\n\n"
        << "The database password is read from SB_DB_PASSWORD.\n"
        << "SB_DB_HOST/PORT/USER/NAME override the config file when set.\n";
}

// Config file first, environment second: the environment is how systemd and
// one-off invocations override a deployed config without editing it.
db::DbConfig db_config_from(const starbase::Config& cfg) {
    db::DbConfig dc;
    dc.host = cfg.db_host;
    dc.port = static_cast<uint16_t>(cfg.db_port);
    dc.user = cfg.db_user;
    dc.database = cfg.db_name;

    if (const char* v = std::getenv("SB_DB_HOST")) dc.host = v;
    if (const char* v = std::getenv("SB_DB_PORT")) dc.port = static_cast<uint16_t>(std::atoi(v));
    if (const char* v = std::getenv("SB_DB_USER")) dc.user = v;
    if (const char* v = std::getenv("SB_DB_NAME")) dc.database = v;
    if (const char* v = std::getenv("SB_DB_PASSWORD")) dc.password = v;
    return dc;
}

// Config is optional for the CLI: with SB_DB_* set you can administer a
// database before any config file has been deployed.
starbase::Config load_config_or_defaults(const std::string& path) {
    try {
        return starbase::Config::load(path);
    } catch (const std::exception& e) {
        if (std::getenv("SB_DB_HOST") || std::getenv("SB_DB_NAME") ||
            std::getenv("SB_DB_USER")) {
            return starbase::Config{};
        }
        std::cerr << "error: " << e.what()
                  << "\n       (pass --config, or set SB_DB_HOST/SB_DB_NAME/SB_DB_USER)\n";
        std::exit(1);
    }
}

std::string yes_no(bool b) { return b ? "yes" : "no"; }

int cmd_db_init(db::Database& d, const starbase::Config& cfg) {
    std::string path = cfg.schema_file;
    if (path.empty() || !std::filesystem::exists(path)) {
        for (const char* c : {"sql/schema.sql", "/usr/local/starbase/sql/schema.sql"}) {
            if (std::filesystem::exists(c)) { path = c; break; }
        }
    }
    if (path.empty() || !std::filesystem::exists(path)) {
        std::cerr << "error: cannot find schema.sql (set [api] schema_file)\n";
        return 1;
    }
    const int n = d.apply_script(path);
    std::cout << "applied " << path << " (" << n << " statements)\n"
              << "schema version " << d.schema_version() << ", "
              << d.tables().size() << " tables\n";
    return 0;
}

int cmd_db_seed(db::Database& d, const starbase::Config& cfg) {
    std::string path = cfg.seed_file;
    if (path.empty() || !std::filesystem::exists(path)) {
        for (const char* c : {"sql/seed.sql", "/usr/local/starbase/sql/seed.sql"}) {
            if (std::filesystem::exists(c)) { path = c; break; }
        }
    }
    if (path.empty() || !std::filesystem::exists(path)) {
        std::cerr << "error: cannot find seed.sql (set [api] seed_file)\n";
        return 1;
    }
    if (!d.table_exists("header_map")) {
        std::cerr << "error: schema not loaded; run 'starbasectl db-init' first\n";
        return 1;
    }
    const int n = d.apply_script(path);
    std::cout << "applied " << path << " (" << n << " statements)\n";
    return 0;
}

int cmd_db_status(db::Database& d, const db::DbConfig& dc) {
    std::cout << "server    " << d.server_version() << "\n"
              << "database  " << dc.user << "@" << dc.host << ":" << dc.port
              << "/" << dc.database << "\n"
              << "schema    version " << d.schema_version() << "\n"
              << "tables    " << d.tables().size() << "\n"
              << "roots     " << d.list_roots().size() << "\n";
    return 0;
}

int cmd_add_root(db::Database& d, const starbase::Config& cfg,
                 const std::string& label, const std::string& path_in) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path_in, ec);
    const std::string path = ec ? path_in : canonical.string();

    if (!std::filesystem::is_directory(path, ec)) {
        std::cerr << "error: not a readable directory: " << path << "\n";
        return 1;
    }

    // Probe the filesystem so watch mode and path hashing are right from the
    // start rather than discovered on the first scan.
    const std::string fs_type = starbase::fs::detect_fs_type(path);
    const bool watchable = starbase::fs::supports_inotify(fs_type);
    const bool case_sensitive = starbase::fs::detect_case_sensitive(path);

    db::RootFields f;
    f.fs_type = fs_type;
    f.case_sensitive = case_sensitive;
    f.watch_mode = watchable ? "auto" : "poll";
    f.scan_interval_s = cfg.default_scan_interval_s;
    f.settle_seconds = cfg.default_settle_seconds;

    const int id = d.add_root(label, path, f);
    std::cout << "added root " << id << ": " << label << " -> " << path << "\n"
              << "  filesystem      " << (fs_type.empty() ? "unknown" : fs_type) << "\n"
              << "  case sensitive  " << yes_no(case_sensitive) << "\n"
              << "  watch mode      " << (watchable ? "auto (inotify usable)" : "poll")
              << "\n";
    if (!watchable) {
        std::cout << "  note: inotify cannot see writes made by another host on "
                  << (fs_type.empty() ? "this filesystem" : fs_type)
                  << ", so the scheduled sweep is the only reliable discovery "
                     "mechanism here.\n";
    }
    if (!case_sensitive) {
        std::cout << "  note: this filesystem folds case; paths are case-folded "
                     "before hashing so one file cannot become two rows.\n";
    }
    return 0;
}

int cmd_list_roots(db::Database& d) {
    const auto roots = d.list_roots();
    if (roots.empty()) {
        std::cout << "no roots registered (add one with 'starbasectl add-root')\n";
        return 0;
    }
    std::cout << std::left << std::setw(16) << "LABEL" << std::setw(8) << "FS"
              << std::setw(7) << "WATCH" << std::setw(5) << "CS" << std::setw(5) << "EN"
              << std::setw(5) << "RW" << std::setw(10) << "FILES"
              << std::setw(9) << "STATUS" << "PATH\n";
    for (const auto& r : roots) {
        std::cout << std::left << std::setw(16) << r.label
                  << std::setw(8) << (r.fs_type.empty() ? "-" : r.fs_type)
                  << std::setw(7) << r.watch_mode
                  << std::setw(5) << (r.case_sensitive ? "y" : "n")
                  << std::setw(5) << (r.enabled ? "y" : "n")
                  << std::setw(5) << (r.writable ? "y" : "n")
                  << std::setw(10) << r.file_count
                  << std::setw(9) << r.last_scan_status
                  << r.path << "\n";
    }
    return 0;
}

int cmd_set_root(db::Database& d, const std::string& label,
                 const std::vector<std::string>& kvs) {
    db::RootFields f;
    for (const auto& kv : kvs) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) {
            std::cerr << "error: expected key=value, got '" << kv << "'\n";
            return 2;
        }
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        const bool truthy = (v == "1" || v == "true" || v == "yes" || v == "on");
        if (k == "enabled") f.enabled = truthy;
        else if (k == "writable") f.writable = truthy;
        else if (k == "case_sensitive") f.case_sensitive = truthy;
        else if (k == "watch_mode") f.watch_mode = v;
        else if (k == "scan_interval_s") f.scan_interval_s = std::atoi(v.c_str());
        else if (k == "settle_seconds") f.settle_seconds = std::atoi(v.c_str());
        else if (k == "ignore_globs") f.ignore_globs = v;
        else { std::cerr << "error: unknown field '" << k << "'\n"; return 2; }
    }
    if (!d.update_root(label, f)) {
        std::cerr << "error: no such root, or nothing to change: " << label << "\n";
        return 1;
    }
    std::cout << "updated root " << label << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string config_path = "/etc/starbase/starbase.conf";

    for (size_t i = 0; i < args.size();) {
        if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
            config_path = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i),
                       args.begin() + static_cast<long>(i) + 2);
            continue;
        }
        ++i;
    }

    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        print_help(argv[0]);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "version" || args[0] == "-v" || args[0] == "--version") {
        std::cout << "starbasectl " << STARBASE_VERSION << "\n";
        return 0;
    }

    const std::string cmd = args[0];
    const starbase::Config cfg = load_config_or_defaults(config_path);
    const db::DbConfig dc = db_config_from(cfg);

    if (dc.password.empty()) {
        std::cerr << "warning: SB_DB_PASSWORD is not set\n";
    }

    try {
        db::Database d(dc);

        if (cmd == "db-init")   return cmd_db_init(d, cfg);
        if (cmd == "db-seed")   return cmd_db_seed(d, cfg);
        if (cmd == "db-status") return cmd_db_status(d, dc);
        if (cmd == "list-roots") return cmd_list_roots(d);

        if (cmd == "add-root") {
            if (args.size() < 3) {
                std::cerr << "usage: starbasectl add-root <label> <path>\n";
                return 2;
            }
            return cmd_add_root(d, cfg, args[1], args[2]);
        }
        if (cmd == "set-root") {
            if (args.size() < 3) {
                std::cerr << "usage: starbasectl set-root <label> <key=value>...\n";
                return 2;
            }
            return cmd_set_root(d, args[1],
                                std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (cmd == "remove-root") {
            if (args.size() < 2) {
                std::cerr << "usage: starbasectl remove-root <label>\n";
                return 2;
            }
            if (!d.remove_root(args[1])) {
                std::cerr << "error: no such root: " << args[1] << "\n";
                return 1;
            }
            std::cout << "removed root " << args[1] << "\n";
            return 0;
        }
        if (cmd == "scan") {
            std::cerr << "starbasectl: 'scan' is not implemented yet (M3)\n";
            return 3;
        }

        std::cerr << "starbasectl: unknown command '" << cmd << "'\n";
        print_help(argv[0]);
        return 2;
    } catch (const db::DbError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
