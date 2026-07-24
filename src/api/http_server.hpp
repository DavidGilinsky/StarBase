// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/http_server.hpp
// Purpose:       Embedded HTTP/JSON API and static web UI server. Read endpoints
//                browse and inspect the index; a token-gated write endpoint
//                triggers a scan. Each request opens its own database connection.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include "database.hpp"

namespace starbase::api {

struct ApiConfig {
    std::string bind = "127.0.0.1";
    int port = 8642;              // distinct from NightWatcher2 (8080), AirWatcher (8686)
    std::string web_root;        // static files served at /
    // Optional shared secret for write endpoints (from SB_API_TOKEN). When set,
    // a write requires the X-SB-Token header (or ?token=). Empty leaves writes
    // open, which is only safe on a localhost bind.
    std::string token;
    db::DbConfig db;             // handlers open their own connection from this
    std::string schema_file;     // for POST /api/v1/db/init (optional)
    std::string seed_file;
};

// Runs a cpp-httplib server on a background thread. Each request handler opens
// its own database connection, since Database is not thread-safe.
class HttpServer {
public:
    explicit HttpServer(ApiConfig cfg);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Bind and start serving on a background thread. Throws if the bind fails.
    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace starbase::api
