// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/main.cpp
// Purpose:       Entry point for starbased: load config, install signal
//                handlers, and run until asked to stop. Scanner, API, and action
//                engine are wired in at later milestones.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <atomic>
#include <memory>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "config.hpp"
#include "logging.hpp"
#include "starbase/version.hpp"

#ifdef SB_HAVE_API
#include "http_server.hpp"
#endif

using starbase::Config;
using starbase::log_error;
using starbase::log_info;
using starbase::log_warn;

namespace {

// Set from signal handlers, so both must be lock-free and async-signal-safe.
std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};

extern "C" void handle_stop(int) { g_stop.store(true); }
extern "C" void handle_reload(int) { g_reload.store(true); }

void print_version() { std::cout << "starbased " << STARBASE_VERSION << "\n"; }

void print_help(const char* argv0) {
    std::cout
        << "starbased " << STARBASE_VERSION
        << " - StarBase astronomical image index daemon\n\n"
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  -c, --config <path>   Path to the INI-style config file\n"
        << "  -f, --foreground      Log to stderr and do not detach (default)\n"
        << "  -v, --version         Print version and exit\n"
        << "  -h, --help            Print this help and exit\n\n"
        << "Monitored trees are registered in the database, not in the config\n"
        << "file; add them with `starbasectl add-root` or from the web UI.\n"
        << "The database password is read from SB_DB_PASSWORD.\n"
        << "Send SIGHUP to reload configuration and the root list.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "/etc/starbase/starbase.conf";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        }
        if (arg == "-f" || arg == "--foreground") {
            continue;  // accepted for symmetry; the daemon never forks
        }
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a path\n";
                return 2;
            }
            config_path = argv[++i];
            continue;
        }
        std::cerr << "error: unknown argument '" << arg << "'\n";
        print_help(argv[0]);
        return 2;
    }

    Config cfg;
    try {
        cfg = Config::load(config_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    starbase::set_log_level(starbase::log_level_from_string(cfg.log_level));

    struct sigaction sa{};
    sa.sa_handler = handle_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sa.sa_handler = handle_reload;
    sigaction(SIGHUP, &sa, nullptr);
    // A client vanishing mid-write must not take the daemon down with it.
    signal(SIGPIPE, SIG_IGN);

    log_info(std::string("starbased ") + STARBASE_VERSION + " starting");
    log_info("config: " + config_path);
    log_info("database: " + cfg.db_user + "@" + cfg.db_host + ":" +
             std::to_string(cfg.db_port) + "/" + cfg.db_name);

    if (std::getenv("SB_DB_PASSWORD") == nullptr) {
        log_warn(
            "SB_DB_PASSWORD is not set; database access will fail. Supply it via "
            "the systemd EnvironmentFile (/etc/starbase/starbase.env).");
    }

#ifdef SB_HAVE_API
    std::unique_ptr<starbase::api::HttpServer> server;
    try {
        starbase::api::ApiConfig api;
        api.bind = cfg.api_bind;
        api.port = cfg.api_port;
        api.web_root = cfg.web_root;
        api.tls = cfg.api_tls;
        api.tls_cert = cfg.api_tls_cert;
        api.tls_key = cfg.api_tls_key;
        api.schema_file = cfg.schema_file;
        api.seed_file = cfg.seed_file;
        api.staging_root = cfg.staging_root;
        api.trash_root = cfg.trash_root;
        api.link_mode = cfg.link_mode;
        api.db.host = cfg.db_host;
        api.db.port = static_cast<uint16_t>(cfg.db_port);
        api.db.user = cfg.db_user;
        api.db.database = cfg.db_name;
        if (const char* v = std::getenv("SB_DB_PASSWORD")) api.db.password = v;
        if (const char* v = std::getenv("SB_API_TOKEN")) api.token = v;
        // Off-localhost with no token is a footgun: warn loudly.
        if (api.bind != "127.0.0.1" && api.bind != "localhost" && api.token.empty())
            log_warn("API bound off localhost with no SB_API_TOKEN; writes are open to the LAN");
        server = std::make_unique<starbase::api::HttpServer>(std::move(api));
        server->start();
    } catch (const std::exception& e) {
        log_error(std::string("failed to start API server: ") + e.what());
        return 1;
    }
#else
    log_info("built without the API/database layer; idling");
#endif

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            log_info("SIGHUP received; reloading configuration");
            try {
                cfg = Config::load(config_path);
                starbase::set_log_level(starbase::log_level_from_string(cfg.log_level));
                log_info("configuration reloaded");
            } catch (const std::exception& e) {
                log_error(std::string("reload failed, keeping previous config: ") + e.what());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    log_info("shutting down");
#ifdef SB_HAVE_API
    if (server) server->stop();
#endif
    return 0;
}
