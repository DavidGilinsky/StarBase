// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/http_server.cpp
// Purpose:       Implementation of the embedded HTTP/JSON API and static server.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "http_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "logging.hpp"
#include "mapping_loader.hpp"
#include "query.hpp"
#include "scanner.hpp"
#include "starbase/version.hpp"
#include "tls_cert.hpp"

namespace starbase::api {
namespace {

using json = nlohmann::json;

// A NULL column becomes JSON null; everything else a string, so the UI is free
// to parse numbers itself. Keeps the serializer trivial and lossless.
json cell(const db::Database::Row& row, size_t i) {
    if (i >= row.size() || !row[i]) return nullptr;
    return *row[i];
}

// Turn a result set into an array of objects keyed by the given column names.
json rows_to_json(const std::vector<db::Database::Row>& rows,
                  const std::vector<std::string>& cols) {
    json out = json::array();
    for (const auto& r : rows) {
        json o = json::object();
        for (size_t i = 0; i < cols.size(); ++i) o[cols[i]] = cell(r, i);
        out.push_back(std::move(o));
    }
    return out;
}

void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& msg) {
    send_json(res, json{{"error", msg}}, status);
}

// Clamp a query-string integer to a range, with a default.
long qint(const httplib::Request& req, const char* key, long def, long lo, long hi) {
    if (!req.has_param(key)) return def;
    try {
        long v = std::stol(req.get_param_value(key));
        return std::max(lo, std::min(hi, v));
    } catch (const std::exception&) {
        return def;
    }
}

}  // namespace

struct HttpServer::Impl {
    ApiConfig cfg;
    // Plain Server or SSLServer, chosen at start() and held by base pointer so
    // route setup is identical either way.
    std::unique_ptr<httplib::Server> server;
    std::thread thread;

    explicit Impl(ApiConfig c) : cfg(std::move(c)) {}

    db::Database db() { return db::Database(cfg.db); }

    // A write endpoint is allowed when no token is configured (localhost trust)
    // or the caller presents the matching token.
    bool write_allowed(const httplib::Request& req) const {
        if (cfg.token.empty()) return true;
        if (req.get_header_value("X-SB-Token") == cfg.token) return true;
        return req.has_param("token") && req.get_param_value("token") == cfg.token;
    }

    void routes();
};

void HttpServer::Impl::routes() {
    server->set_default_headers({{"Access-Control-Allow-Origin", "*"}});

    // ---- GET /api/v1/status ----
    server->Get("/api/v1/status", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            json j;
            j["version"] = STARBASE_VERSION;
            j["server"] = d.server_version();
            j["schema_version"] = d.schema_version();
            auto scalar = [&](const std::string& sql) -> long long {
                auto r = d.query(sql);
                return (r.empty() || !r[0][0]) ? 0 : std::stoll(*r[0][0]);
            };
            j["roots"] = scalar("SELECT COUNT(*) FROM roots");
            j["files"] = scalar("SELECT COUNT(*) FROM files");
            j["frames"] = scalar("SELECT COUNT(*) FROM frames");
            j["objects"] = scalar("SELECT COUNT(DISTINCT object) FROM frames WHERE object IS NOT NULL");
            j["nights"] = scalar("SELECT COUNT(DISTINCT session_night) FROM frames");
            j["errors"] = scalar("SELECT COUNT(*) FROM files WHERE status='error'");
            send_json(res, j);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- GET /api/v1/roots ----
    server->Get("/api/v1/roots", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            const std::vector<std::string> cols = {"id",       "label",     "path",
                                                   "enabled",  "writable",  "fs_type",
                                                   "watch_mode", "file_count", "last_scan_status"};
            auto rows = d.query(
                "SELECT id, label, path, enabled, writable, fs_type, watch_mode, file_count, "
                "last_scan_status FROM roots ORDER BY label");
            send_json(res, rows_to_json(rows, cols));
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- GET /api/v1/frames  (paginated, filtered browse) ----
    server->Get("/api/v1/frames", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const long limit = qint(req, "limit", 50, 1, 500);
            const long offset = qint(req, "offset", 0, 0, 100000000);

            // Build a WHERE from a small allowlist of filters; every value is
            // escaped, and only these columns can be filtered.
            std::string where = " WHERE 1=1";
            auto add_eq = [&](const char* param, const char* col) {
                if (req.has_param(param))
                    where += " AND " + std::string(col) + " = '" +
                             d.escape(req.get_param_value(param)) + "'";
            };
            add_eq("object", "object");
            add_eq("image_type", "image_type");
            add_eq("filter", "filter");
            add_eq("night", "session_night");
            add_eq("rig", "rig");
            add_eq("camera", "camera");
            if (req.has_param("root"))
                where += " AND root_label = '" + d.escape(req.get_param_value("root")) + "'";

            const std::string total_sql = "SELECT COUNT(*) FROM v_frames" + where;
            auto tr = d.query(total_sql);
            const long long total = (tr.empty() || !tr[0][0]) ? 0 : std::stoll(*tr[0][0]);

            const std::vector<std::string> cols = {
                "frame_id",   "abs_path",   "filename",  "image_type", "object",
                "filter",     "session_night", "date_obs_utc", "exposure_s", "gain",
                "rig",        "camera",     "site",      "sqm_mag_arcsec2"};
            auto rows = d.query(
                "SELECT frame_id, abs_path, filename, image_type, object, filter, "
                "session_night, date_obs_utc, exposure_s, gain, rig, camera, site, "
                "sqm_mag_arcsec2 FROM v_frames" + where +
                " ORDER BY date_obs_utc DESC LIMIT " + std::to_string(limit) +
                " OFFSET " + std::to_string(offset));

            json j;
            j["total"] = total;
            j["limit"] = limit;
            j["offset"] = offset;
            j["frames"] = rows_to_json(rows, cols);
            send_json(res, j);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- GET /api/v1/frames/:id  (detail + full header) ----
    server->Get(R"(/api/v1/frames/(\d+))", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
        try {
            auto d = db();
            const std::string id = d.escape(req.matches[1]);
            auto rows = d.query("SELECT * FROM v_frames WHERE frame_id = " + id + " LIMIT 1");
            if (rows.empty()) { send_error(res, 404, "no such frame"); return; }

            // Column names from the view, in order, so we can key the object.
            auto meta = d.query(
                "SELECT column_name FROM information_schema.columns WHERE table_schema = "
                "DATABASE() AND table_name = 'v_frames' ORDER BY ordinal_position");
            json frame = json::object();
            for (size_t i = 0; i < meta.size() && i < rows[0].size(); ++i)
                frame[*meta[i][0]] = cell(rows[0], i);

            // The full header, verbatim and in order.
            auto kw = d.query("SELECT keyword, value, comment FROM frame_keywords "
                              "WHERE frame_id = " + id + " ORDER BY ord");
            frame["keywords"] = rows_to_json(kw, {"keyword", "value", "comment"});
            send_json(res, frame);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- POST /api/v1/query  (filter-AST search) ----
    // Body: {"filter": <ast>, "sort": [...], "limit": N, "offset": M}
    server->Post("/api/v1/query", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string where, order;
            try {
                where = starbase::query::compile_filter(body.value("filter", json()), d);
                order = starbase::query::compile_sort(body.value("sort", json()));
            } catch (const starbase::query::QueryError& qe) {
                send_error(res, 400, qe.what());
                return;
            }
            const long limit = std::max(1L, std::min(500L, body.value("limit", 50L)));
            const long offset = std::max(0L, body.value("offset", 0L));

            auto tr = d.query("SELECT COUNT(*) FROM v_frames WHERE " + where);
            const long long total = (tr.empty() || !tr[0][0]) ? 0 : std::stoll(*tr[0][0]);

            const std::vector<std::string> cols = {
                "frame_id",   "abs_path",   "filename",  "image_type", "object",
                "filter",     "session_night", "date_obs_utc", "exposure_s", "gain",
                "rig",        "camera",     "site",      "sqm_mag_arcsec2"};
            auto rows = d.query(
                "SELECT frame_id, abs_path, filename, image_type, object, filter, "
                "session_night, date_obs_utc, exposure_s, gain, rig, camera, site, "
                "sqm_mag_arcsec2 FROM v_frames WHERE " + where + " ORDER BY " + order +
                " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));

            json j;
            j["total"] = total;
            j["limit"] = limit;
            j["offset"] = offset;
            j["where"] = where;  // echoed so a caller can see the compiled predicate
            j["frames"] = rows_to_json(rows, cols);
            send_json(res, j);
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid JSON body: ") + e.what());
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- Saved queries: list, get, save, delete ----
    server->Get("/api/v1/queries", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            auto rows = d.query(
                "SELECT id, name, description, filter_json, sort_json, last_count, "
                "DATE_FORMAT(last_run_at,'%Y-%m-%dT%H:%i:%sZ') FROM saved_queries ORDER BY name");
            send_json(res, rows_to_json(rows, {"id", "name", "description", "filter_json",
                                               "sort_json", "last_count", "last_run_at"}));
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Post("/api/v1/queries", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            json body = json::parse(req.body);
            const std::string name = body.at("name").get<std::string>();
            if (name.empty()) { send_error(res, 400, "name is required"); return; }
            // Store the filter as-is; validate it compiles before saving.
            const json filter = body.value("filter", json::object());
            try { starbase::query::compile_filter(filter, d); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            const std::string sort = body.contains("sort") ? body["sort"].dump() : "";
            const std::string desc = body.value("description", "");
            d.exec("INSERT INTO saved_queries (name, description, filter_json, sort_json) VALUES ('" +
                   d.escape(name) + "', " + (desc.empty() ? "NULL" : "'" + d.escape(desc) + "'") +
                   ", '" + d.escape(filter.dump()) + "', " +
                   (sort.empty() ? "NULL" : "'" + d.escape(sort) + "'") + ") "
                   "ON DUPLICATE KEY UPDATE description=VALUES(description), "
                   "filter_json=VALUES(filter_json), sort_json=VALUES(sort_json)");
            send_json(res, json{{"saved", name}});
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid JSON: ") + e.what());
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/queries/(\d+))", [this](const httplib::Request& req,
                                                      httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            d.exec("DELETE FROM saved_queries WHERE id = " + d.escape(req.matches[1]));
            send_json(res, json{{"deleted", std::stoi(req.matches[1])}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/summary  (dashboard aggregates) ----
    server->Get("/api/v1/summary", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string by = req.has_param("by") ? req.get_param_value("by") : "object";
            std::string group, label;
            if (by == "night") { group = "session_night"; label = "session_night"; }
            else if (by == "filter") { group = "filter"; label = "filter"; }
            else { group = "object"; label = "object"; }

            auto rows = d.query(
                "SELECT COALESCE(" + group + ",'(none)') AS label, image_type, COUNT(*) AS n, "
                "ROUND(SUM(exposure_s)/3600, 2) AS hours FROM frames "
                "GROUP BY " + group + ", image_type ORDER BY n DESC LIMIT 500");
            send_json(res, rows_to_json(rows, {"label", "image_type", "count", "hours"}));
            (void)label;
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- POST /api/v1/scan  (write; token-gated) ----
    server->Post("/api/v1/scan", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const auto mapping = starbase::index::load_mapping(d);
            std::vector<db::RootRow> roots;
            if (req.has_param("root")) {
                auto r = d.find_root_by_label(req.get_param_value("root"));
                if (!r) { send_error(res, 404, "no such root"); return; }
                roots.push_back(*r);
            } else {
                for (const auto& r : d.list_roots())
                    if (r.enabled) roots.push_back(r);
            }
            json results = json::array();
            for (const auto& r : roots) {
                starbase::scan::ScanConfig sc;
                sc.settle_seconds = r.settle_seconds;
                sc.case_sensitive = r.case_sensitive;
                const auto st = starbase::scan::scan_root(cfg.db, r, mapping, {}, sc);
                results.push_back({{"root", r.label},
                                   {"seen", st.files_seen},
                                   {"added", st.files_added},
                                   {"updated", st.files_updated},
                                   {"unchanged", st.files_skipped},
                                   {"settling", st.files_settling},
                                   {"error", st.files_error},
                                   {"frames", st.frames_written},
                                   {"ms", st.duration_ms}});
            }
            send_json(res, results);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- static web UI at / ----
    if (!cfg.web_root.empty()) {
        server->set_mount_point("/", cfg.web_root);
    }
}

HttpServer::HttpServer(ApiConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    const std::string bind = impl_->cfg.bind;
    const int port = impl_->cfg.port;

    // Decide HTTP vs HTTPS. TLS needs both a cert and a key path; when enabled
    // and either is missing, a self-signed pair is generated (SANs cover
    // localhost + 127.0.0.1), so the operator gets working HTTPS out of the box.
    bool tls = impl_->cfg.tls && !impl_->cfg.tls_cert.empty() && !impl_->cfg.tls_key.empty();
    if (tls) {
        std::string info;
        if (!ensure_self_signed_cert(impl_->cfg.tls_cert, impl_->cfg.tls_key, "starbase", info))
            throw std::runtime_error("TLS enabled but no usable certificate: " + info);
        starbase::log_info("TLS: " + info);
        auto ssl = std::make_unique<httplib::SSLServer>(impl_->cfg.tls_cert.c_str(),
                                                        impl_->cfg.tls_key.c_str());
        if (!ssl->is_valid())
            throw std::runtime_error("TLS certificate/key invalid or unreadable (" +
                                     impl_->cfg.tls_cert + ", " + impl_->cfg.tls_key + ")");
        impl_->server = std::move(ssl);
    } else {
        if (impl_->cfg.tls)
            starbase::log_warn("tls=on but no tls_cert/tls_key configured; serving plain HTTP");
        impl_->server = std::make_unique<httplib::Server>();
    }

    impl_->routes();
    if (!impl_->server->bind_to_port(bind.c_str(), port))
        throw std::runtime_error("cannot bind API to " + bind + ":" + std::to_string(port) +
                                 " (in use?)");
    impl_->thread = std::thread([this] { impl_->server->listen_after_bind(); });
    const std::string scheme = tls ? "https" : "http";
    starbase::log_info("API listening on " + scheme + "://" + bind + ":" + std::to_string(port) +
                       (impl_->cfg.web_root.empty() ? "" : " (web UI at /)"));
}

void HttpServer::stop() {
    if (impl_ && impl_->server && impl_->server->is_running()) impl_->server->stop();
    if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace starbase::api
