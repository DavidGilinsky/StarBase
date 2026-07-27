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

#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cctype>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "action.hpp"
#include "calibration.hpp"
#include "canon.hpp"
#include "fsinfo.hpp"
#include "password.hpp"
#include "sesame.hpp"
#include "logging.hpp"
#include "mapping_loader.hpp"
#include "query.hpp"
#include "scanner.hpp"
#include "starbase/version.hpp"
#include "tls_cert.hpp"
#include "wbpp.hpp"

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

// A syntactically acceptable listen address (IPv4/IPv6 literal or hostname,
// including 0.0.0.0 / ::). A valid-but-unbindable value is caught at bind time,
// where the daemon falls back to localhost rather than leaving the API down.
bool valid_bind(const std::string& b) {
    if (b.empty() || b.size() > 64) return false;
    for (const char c : b)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' ||
              c == '-' || c == '_'))
            return false;
    return true;
}

// Value of one cookie from the request's Cookie header, or empty.
std::string get_cookie(const httplib::Request& req, const std::string& name) {
    const std::string c = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        const size_t eq = c.find('=', pos);
        if (eq == std::string::npos) break;
        size_t ks = c.find_first_not_of(" \t", pos);
        if (ks == std::string::npos) ks = pos;
        const std::string key = c.substr(ks, eq - ks);
        const size_t semi = c.find(';', eq);
        const std::string val = c.substr(eq + 1, (semi == std::string::npos ? c.size() : semi) - eq - 1);
        if (key == name) return val;
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return std::string();
}

// A frame's observatory location, read from the raw header cards (OBSGEO first,
// then the SITE*/*-OBS fallbacks). OBSGEO-* are signed decimal degrees.
struct FrameLoc { long long id; double lat, lon; };
std::vector<FrameLoc> frame_locations(db::Database& d) {
    auto rows = d.query(
        "SELECT f.id, "
        "(SELECT k.value FROM frame_keywords k WHERE k.frame_id=f.id AND k.keyword IN "
        "('OBSGEO-B','SITELAT','LAT-OBS') ORDER BY FIELD(k.keyword,'OBSGEO-B','SITELAT','LAT-OBS') LIMIT 1), "
        "(SELECT k.value FROM frame_keywords k WHERE k.frame_id=f.id AND k.keyword IN "
        "('OBSGEO-L','SITELONG','LONG-OBS') ORDER BY FIELD(k.keyword,'OBSGEO-L','SITELONG','LONG-OBS') LIMIT 1) "
        "FROM frames f");
    std::vector<FrameLoc> out;
    for (const auto& r : rows) {
        if (!r[0] || !r[1] || !r[2]) continue;
        try {
            const double la = std::stod(*r[1]), lo = std::stod(*r[2]);
            if (la < -90 || la > 90 || lo < -180 || lo > 180) continue;
            out.push_back({std::stoll(*r[0]), la, lo});
        } catch (const std::exception&) { /* unparseable card */ }
    }
    return out;
}

// Great-circle distance in metres.
double haversine_m(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000.0, d2r = M_PI / 180.0;
    const double dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                     std::cos(la1 * d2r) * std::cos(la2 * d2r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return R * 2 * std::asin(std::min(1.0, std::sqrt(a)));
}

// Advisory only: another active rig on the same camera whose focal range overlaps
// [fmin,fmax]. Overlapping ranges make the v_rig_resolve lookup (LIMIT 1)
// non-deterministic, the same first-match caveat EquipmentResolver carries.
// Empty string when there is no overlap.
std::string rig_overlap_warning(db::Database& d, long long camera_id, double fmin, double fmax,
                                long long exclude_rig_id) {
    auto ov = d.query("SELECT name FROM rigs WHERE status = 'active' AND camera_id = " +
                      std::to_string(camera_id) + " AND id <> " + std::to_string(exclude_rig_id) +
                      " AND focal_min_mm <= " + std::to_string(fmax) +
                      " AND focal_max_mm >= " + std::to_string(fmin) + " LIMIT 1");
    if (ov.empty() || !ov[0][0]) return {};
    return "focal range overlaps active rig '" + *ov[0][0] +
           "' on the same camera; rig resolution (first match) is non-deterministic where they overlap";
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

    // Live scan progress. POST /scan runs in scan_thread and reports here (one
    // scan at a time); GET /scan/status reads it. Guarded by scan_mtx.
    struct ScanState {
        bool active = false;
        bool finished = false;               // a completed run still worth showing
        std::string root;                    // root currently being scanned
        int total_roots = 0, done_roots = 0;
        long seen = 0, added = 0, updated = 0, skipped = 0, settling = 0, errored = 0,
             frames = 0, sidecars = 0;       // live totals for the current root
        std::chrono::steady_clock::time_point started;
        json results = json::array();        // per-root final stats
    };
    std::mutex scan_mtx;
    ScanState scan_state;
    std::thread scan_thread;
    std::atomic<bool> scan_cancel{false};

    // Interval scheduler: rescans due roots on their scan_interval_s.
    std::thread scheduler_thread;
    std::atomic<bool> scheduler_stop{false};

    // Start a background scan of `roots` if none is running (returns false if one
    // already is). Shared by POST /scan and the scheduler so both honour the
    // single-scan guard and report to scan_state.
    bool try_start_scan(std::vector<db::RootRow> roots, starbase::extract::HeaderMapping mapping);
    // Loop that periodically scans roots whose scan_interval_s has elapsed.
    void scheduler_loop();

    explicit Impl(ApiConfig c) : cfg(std::move(c)) {}

    db::Database db() { return db::Database(cfg.db); }

    // The identity behind a request: the static admin token, or a login session
    // (cookie sb_session, or an Authorization: Bearer that is not the token).
    struct Caller { std::string username; std::string role; bool via_token = false; };

    std::optional<Caller> authenticate(const httplib::Request& req) {
        const std::string h = req.get_header_value("Authorization");
        const std::string bearer = (h.rfind("Bearer ", 0) == 0) ? h.substr(7) : "";
        if (!cfg.token.empty() &&
            (req.get_header_value("X-SB-Token") == cfg.token || bearer == cfg.token ||
             (req.has_param("token") && req.get_param_value("token") == cfg.token)))
            return Caller{"(token)", "admin", true};

        std::string sess = get_cookie(req, "sb_session");
        if (sess.empty()) sess = bearer;
        if (!sess.empty()) {
            try {
                auto d = db();
                auto r = d.query(
                    "SELECT u.username, u.role FROM sessions s JOIN users u ON u.id = s.user_id "
                    "WHERE s.token = '" + d.escape(sess) + "' AND s.expires_at > UTC_TIMESTAMP() "
                    "AND u.enabled = 1 LIMIT 1");
                if (!r.empty() && r[0][0])
                    return Caller{*r[0][0], r[0][1] ? *r[0][1] : "readonly", false};
            } catch (const std::exception&) { /* fall through to unauthenticated */ }
        }
        return std::nullopt;
    }

    bool any_users() {
        try {
            auto d = db();
            auto r = d.query("SELECT COUNT(*) FROM users");
            return !r.empty() && r[0][0] && std::stoll(*r[0][0]) > 0;
        } catch (const std::exception&) { return false; }
    }

    // A write is allowed for an admin/user session or the static token. When no
    // auth is configured at all (no token, no users) writes stay open, so a
    // fresh localhost install keeps working until an admin is created.
    bool write_allowed(const httplib::Request& req) {
        if (auto c = authenticate(req)) return c->role == "admin" || c->role == "user";
        return cfg.token.empty() && !any_users();
    }

    // Gate a user-management endpoint on an admin identity; sends 401/403 itself.
    bool require_admin(const httplib::Request& req, httplib::Response& res) {
        auto c = authenticate(req);
        if (c && c->role == "admin") return true;
        send_error(res, c ? 403 : 401, c ? "admin role required" : "login required");
        return false;
    }

    // Apply schema.sql on startup (idempotent) so a fresh database is built and
    // tables added since the last version appear without a manual migration.
    void ensure_schema();
    // Seed an initial admin so a fresh install can be logged into. Idempotent.
    void ensure_default_admin();

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
            // Canonical targets, so the "Targets" count matches the Browse/Query
            // target pulldown (distinct object_canonical) rather than raw OBJECT.
            j["objects"] = scalar("SELECT COUNT(DISTINCT object_canonical) FROM frames "
                                  "WHERE object_canonical IS NOT NULL AND object_canonical <> ''");
            j["nights"] = scalar("SELECT COUNT(DISTINCT session_night) FROM frames");
            j["errors"] = scalar("SELECT COUNT(*) FROM files WHERE status='error'");
            send_json(res, j);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- GET /api/v1/fs/list  (server-side directory browser for root picking) ----
    // Lists the subdirectories of a path the daemon can see, so the Roots tab can
    // offer a picker instead of a hand-typed path. Gated like root management: a
    // caller who can add a root (any path) can already browse to choose one.
    server->Get("/api/v1/fs/list", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a login is required to browse the server"); return; }
        try {
            namespace stdfs = std::filesystem;
            std::string reqpath = req.has_param("path") ? req.get_param_value("path") : "/";
            if (reqpath.empty()) reqpath = "/";
            std::error_code ec;
            stdfs::path p = stdfs::weakly_canonical(reqpath, ec);
            if (ec || p.empty()) p = stdfs::path(reqpath);
            if (!stdfs::is_directory(p, ec)) { send_error(res, 400, "not a directory: " + p.string()); return; }

            json entries = json::array();
            stdfs::directory_iterator it(p, stdfs::directory_options::skip_permission_denied, ec);
            if (ec) { send_error(res, 400, "cannot read directory: " + ec.message()); return; }
            for (const auto& e : it) {
                std::error_code de;
                if (e.is_directory(de) && !de)
                    entries.push_back({{"name", e.path().filename().string()},
                                       {"path", e.path().string()}});
            }
            std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
                return a["name"].get<std::string>() < b["name"].get<std::string>();
            });
            json out;
            out["path"] = p.string();
            out["parent"] = (p != p.root_path()) ? json(p.parent_path().string()) : json(nullptr);
            out["entries"] = entries;
            send_json(res, out);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/facets/:field  (distinct values, for Browse dropdowns) ----
    // Only an allowlisted set of low-cardinality columns, so the field name is
    // safe to interpolate and the query stays cheap.
    server->Get(R"(/api/v1/facets/([a-z_]+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            const std::string field = req.matches[1];
            if (field != "rig" && field != "camera" && field != "filter" && field != "object") {
                send_error(res, 404, "no such facet"); return;
            }
            auto d = db();
            // The object facet lists canonical designations (deduped M/NGC), so a
            // pick maps straight onto the catalog-aware object search.
            const std::string col = (field == "object") ? "object_canonical" : field;
            auto rows = d.query("SELECT DISTINCT `" + col + "` FROM v_frames WHERE `" + col +
                                "` IS NOT NULL AND `" + col + "` <> '' ORDER BY `" + col + "`");
            json arr = json::array();
            for (const auto& r : rows) if (r[0]) arr.push_back(*r[0]);
            send_json(res, arr);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/roots ----
    server->Get("/api/v1/roots", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            const std::vector<std::string> cols = {
                "id", "label", "path", "enabled", "writable", "case_sensitive",
                "fs_type", "watch_mode", "scan_interval_s", "settle_seconds",
                "ignore_globs", "file_count", "last_scan_status",
                "last_scan_end", "last_scan_error"};
            auto rows = d.query(
                "SELECT id, label, path, enabled, writable, case_sensitive, fs_type, "
                "watch_mode, scan_interval_s, settle_seconds, ignore_globs, file_count, "
                "last_scan_status, "
                "DATE_FORMAT(last_scan_end,'%Y-%m-%dT%H:%i:%sZ'), last_scan_error "
                "FROM roots ORDER BY label");
            send_json(res, rows_to_json(rows, cols));
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- POST /api/v1/roots  (register a directory tree; token-gated) ----
    // Body: {"label":"lights","path":"/abs/dir", optional writable/enabled/
    //        watch_mode/scan_interval_s/settle_seconds/ignore_globs}. The
    //        filesystem is probed (type, case-folding, inotify) like the CLI.
    server->Post("/api/v1/roots", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            json body = json::parse(req.body);
            const std::string label = body.value("label", "");
            std::string path = body.value("path", "");
            if (label.empty() || path.empty()) { send_error(res, 400, "label and path are required"); return; }

            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(path, ec);
            if (!ec) path = canon.string();
            if (!std::filesystem::is_directory(path, ec)) {
                send_error(res, 400, "not a readable directory: " + path); return;
            }
            const std::string fs_type = starbase::fs::detect_fs_type(path);
            const bool watchable = starbase::fs::supports_inotify(fs_type);
            const bool case_sensitive = starbase::fs::detect_case_sensitive(path);

            db::RootFields f;
            f.fs_type = fs_type;
            f.case_sensitive = case_sensitive;
            f.watch_mode = body.contains("watch_mode") ? body["watch_mode"].get<std::string>()
                                                       : std::string(watchable ? "auto" : "poll");
            if (body.contains("writable")) f.writable = body["writable"].get<bool>();
            if (body.contains("enabled")) f.enabled = body["enabled"].get<bool>();
            if (body.contains("scan_interval_s")) f.scan_interval_s = body["scan_interval_s"].get<int>();
            if (body.contains("settle_seconds")) f.settle_seconds = body["settle_seconds"].get<int>();
            if (body.contains("ignore_globs")) f.ignore_globs = body["ignore_globs"].get<std::string>();

            int id;
            try { id = d.add_root(label, path, f); }
            catch (const db::DbError& de) {
                // 1062 = duplicate key: the label or path is already registered.
                send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return;
            }
            send_json(res, json{{"id", id}, {"label", label}, {"path", path},
                                {"fs_type", fs_type}, {"case_sensitive", case_sensitive},
                                {"watch_mode", *f.watch_mode}, {"watchable", watchable}});
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid JSON: ") + e.what());
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- PATCH /api/v1/roots/:label  (change settings; token-gated) ----
    server->Patch(R"(/api/v1/roots/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const std::string label = req.matches[1];
            json body = json::parse(req.body);
            db::RootFields f;
            if (body.contains("enabled")) f.enabled = body["enabled"].get<bool>();
            if (body.contains("writable")) f.writable = body["writable"].get<bool>();
            if (body.contains("case_sensitive")) f.case_sensitive = body["case_sensitive"].get<bool>();
            if (body.contains("watch_mode")) f.watch_mode = body["watch_mode"].get<std::string>();
            if (body.contains("scan_interval_s")) f.scan_interval_s = body["scan_interval_s"].get<int>();
            if (body.contains("settle_seconds")) f.settle_seconds = body["settle_seconds"].get<int>();
            if (body.contains("ignore_globs")) f.ignore_globs = body["ignore_globs"].get<std::string>();
            if (!d.update_root(label, f)) { send_error(res, 404, "no such root"); return; }
            send_json(res, json{{"updated", label}});
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid JSON: ") + e.what());
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- DELETE /api/v1/roots/:label  (unregister; indexed rows cascade) ----
    server->Delete(R"(/api/v1/roots/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            if (!d.remove_root(req.matches[1])) { send_error(res, 404, "no such root"); return; }
            send_json(res, json{{"deleted", std::string(req.matches[1])}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
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
            // Object search is catalog-aware: a value is expanded to all its
            // equivalent designations (M31 / M 31 / NGC 224 all match), and the
            // match is against object_canonical, which is spacing-normalized.
            if (req.has_param("object")) {
                std::string in;
                for (const auto& x : starbase::names::designations(req.get_param_value("object")))
                    in += (in.empty() ? "" : ", ") + std::string("'") + d.escape(x) + "'";
                where += " AND object_canonical IN (" + in + ")";
            }
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

            // Sidecars/logs recorded alongside this frame (M9).
            auto arts = d.query("SELECT kind, filename, rel_path, size_bytes, "
                                "DATE_FORMAT(mtime_utc,'%Y-%m-%dT%H:%i:%sZ') FROM artifacts "
                                "WHERE frame_id = " + id + " ORDER BY filename");
            frame["artifacts"] =
                rows_to_json(arts, {"kind", "filename", "rel_path", "size_bytes", "mtime"});

            // Tags on this frame, and the collections it belongs to.
            auto tg = d.query("SELECT t.id, t.name, t.color FROM frame_tags ft "
                              "JOIN tags t ON t.id = ft.tag_id WHERE ft.frame_id = " + id +
                              " ORDER BY t.name");
            frame["tags"] = rows_to_json(tg, {"id", "name", "color"});
            auto cl = d.query("SELECT c.id, c.name FROM collection_frames cf "
                              "JOIN collections c ON c.id = cf.collection_id "
                              "WHERE cf.frame_id = " + id + " ORDER BY c.name");
            frame["collections"] = rows_to_json(cl, {"id", "name"});
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

            std::vector<std::string> cols = {
                "frame_id",   "abs_path",   "filename",  "image_type", "object",
                "filter",     "session_night", "date_obs_utc", "exposure_s", "gain",
                "rig",        "camera",     "site",      "sqm_mag_arcsec2"};
            // Reflect the query: add each field the filter used as a column (when
            // it is a real v_frames column and not already selected), and echo the
            // queried field list so the UI can render those columns.
            std::set<std::string> viewcols;
            for (const auto& r : d.query(
                     "SELECT column_name FROM information_schema.columns WHERE "
                     "table_schema = DATABASE() AND table_name = 'v_frames'"))
                if (r[0]) viewcols.insert(*r[0]);
            json query_fields = json::array();
            std::set<std::string> have(cols.begin(), cols.end());
            for (const auto& f : starbase::query::filter_fields(body.value("filter", json()))) {
                if (!viewcols.count(f)) continue;  // ignore a field not exposed by the view
                query_fields.push_back(f);
                if (!have.count(f)) { cols.push_back(f); have.insert(f); }
            }

            std::string sel;
            for (size_t i = 0; i < cols.size(); ++i) sel += (i ? ", " : "") + cols[i];
            auto rows = d.query("SELECT " + sel + " FROM v_frames WHERE " + where +
                                " ORDER BY " + order + " LIMIT " + std::to_string(limit) +
                                " OFFSET " + std::to_string(offset));

            json j;
            j["total"] = total;
            j["limit"] = limit;
            j["offset"] = offset;
            j["where"] = where;             // echoed so a caller can see the predicate
            j["query_fields"] = query_fields;  // fields the filter used, for result columns
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

    // ---- GET /api/v1/frames/:id/calibration  (matched darks/flats/bias) ----
    server->Get(R"(/api/v1/frames/(\d+)/calibration)", [this](const httplib::Request& req,
                                                              httplib::Response& res) {
        try {
            auto d = db();
            const long long id = std::stoll(req.matches[1]);
            auto key = starbase::match::light_key_for(d, id);
            if (!key) { send_error(res, 404, "no such light frame"); return; }
            const int limit = static_cast<int>(qint(req, "limit", 10, 1, 100));

            json out = json::array();
            for (const auto& m : starbase::match::match_calibration(d, *key, limit)) {
                json group;
                group["target_type"] = m.target_type;
                group["rule"] = m.rule_name;
                group["total"] = m.total;
                if (!m.warning.empty()) group["warning"] = m.warning;
                json cands = json::array();
                for (const auto& c : m.candidates) {
                    cands.push_back({{"frame_id", c.frame_id},
                                     {"image_type", c.image_type},
                                     {"is_master", c.is_master},
                                     {"filename", c.filename},
                                     {"abs_path", c.abs_path},
                                     {"session_night", c.session_night},
                                     {"date_obs_utc", c.date_obs_utc},
                                     {"score", c.score},
                                     {"reason", c.reason}});
                }
                group["candidates"] = cands;
                out.push_back(std::move(group));
            }
            send_json(res, out);
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- POST /api/v1/actions  (stage / fsop / export; token-gated) ----
    // Body: {"op":"stage|copy|symlink|move|trash|export",
    //        "frames":[ids] | "filter":<ast>, "target":"dir", "format":"...",
    //        "dry_run":true, "link_mode":"symlink|hardlink|copy"}
    server->Post("/api/v1/actions", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            const std::string op = body.value("op", "");

            // Resolve the target set from explicit ids or a filter.
            std::vector<starbase::action::FrameRef> frames;
            if (body.contains("frames") && body["frames"].is_array()) {
                std::vector<long long> ids;
                for (const auto& v : body["frames"]) ids.push_back(v.get<long long>());
                frames = starbase::action::resolve_by_ids(d, ids);
            } else if (body.contains("filter")) {
                const long limit = body.value("limit", 100000L);
                try { frames = starbase::action::resolve_by_filter(d, body["filter"].dump(), limit); }
                catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            }
            if (frames.empty()) { send_error(res, 400, "no frames resolved (give 'frames' or 'filter')"); return; }

            starbase::action::ActionConfig ac;
            ac.staging_root = cfg.staging_root;
            ac.trash_root = cfg.trash_root;
            ac.link_mode = body.value("link_mode", cfg.link_mode);

            auto job_json = [](const starbase::action::JobResult& jr) {
                json j;
                j["job_id"] = jr.job_id;
                j["type"] = jr.type;
                j["dry_run"] = jr.dry_run;
                j["total"] = jr.total;
                j["done"] = jr.done;
                j["failed"] = jr.failed;
                j["root"] = jr.root;
                json items = json::array();
                for (const auto& it : jr.items)
                    items.push_back({{"frame_id", it.frame_id}, {"action", it.action},
                                     {"src", it.src}, {"dst", it.dst},
                                     {"status", it.status}, {"detail", it.detail}});
                j["items"] = items;
                return j;
            };

            if (op == "stage") {
                send_json(res, job_json(starbase::action::stage(d, ac, frames)));
            } else if (op == "wbpp") {
                // Stage the set, then render a WBPP command-line handoff over the
                // typed tree. starbased has no desktop session, so the payload is
                // a command plus a launcher the user runs from their own session.
                auto jr = starbase::action::stage(d, ac, frames);
                std::vector<std::string> types;
                for (const auto& f : frames) {
                    const std::string t = f.image_type.empty() ? "unknown" : f.image_type;
                    if (std::find(types.begin(), types.end(), t) == types.end())
                        types.push_back(t);
                }
                starbase::pix::WbppProfile prof;
                if (body.contains("profile") && body["profile"].is_object()) {
                    const auto& pj = body["profile"];
                    prof.output_dir = pj.value("output_dir", std::string());
                    prof.keywords = pj.value("keywords", prof.keywords);
                    prof.grouping_enabled = pj.value("grouping_enabled", prof.grouping_enabled);
                    prof.fits_convention = pj.value("fits_convention", prof.fits_convention);
                    prof.pixinsight_sh = pj.value("pixinsight_sh", prof.pixinsight_sh);
                    prof.wbpp_script = pj.value("wbpp_script", prof.wbpp_script);
                    if (pj.contains("extra_params") && pj["extra_params"].is_array())
                        for (const auto& e : pj["extra_params"])
                            prof.extra_params.push_back(e.get<std::string>());
                }
                if (prof.output_dir.empty()) prof.output_dir = jr.root + "/out";
                const bool load_only = body.value("load_only", false);
                auto plan = starbase::pix::render(jr.root, types, prof, load_only);

                // Record the rendered command against the job for later download.
                json pp = {{"mode", plan.mode}, {"output_dir", plan.output_dir},
                           {"command", plan.command}, {"launcher", plan.launcher}};
                d.exec("UPDATE jobs SET type='wbpp', params_json='" + d.escape(pp.dump()) +
                       "' WHERE id=" + std::to_string(jr.job_id));

                json out = job_json(jr);
                out["type"] = "wbpp";
                out["mode"] = plan.mode;
                out["output_dir"] = plan.output_dir;
                out["dirs"] = plan.dirs;
                out["command"] = plan.command;
                out["launcher"] = plan.launcher;
                out["warnings"] = plan.warnings;
                send_json(res, out);
            } else if (op == "export") {
                const std::string fmt = body.value("format", "csv");
                const std::string content = starbase::action::export_frames(d, frames, fmt);
                res.set_content(content, fmt == "json" ? "application/json"
                                        : fmt == "paths" ? "text/plain" : "text/csv");
            } else if (op == "copy" || op == "symlink" || op == "move" || op == "trash") {
                // Destructive ops default to a dry run unless dry_run:false is explicit.
                const bool destructive = (op == "move" || op == "trash");
                const bool dry = body.value("dry_run", destructive);
                const std::string target = body.value("target", "");
                if ((op == "copy" || op == "symlink" || op == "move") && target.empty()) {
                    send_error(res, 400, "'" + op + "' needs a target directory"); return;
                }
                send_json(res, job_json(starbase::action::fsop(d, ac, op, frames, target, dry)));
            } else {
                send_error(res, 400, "unknown op '" + op + "'");
            }
        } catch (const json::exception& e) {
            send_error(res, 400, std::string("invalid JSON: ") + e.what());
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- GET /api/v1/jobs and /api/v1/jobs/:id  (audit trail) ----
    server->Get("/api/v1/jobs", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            auto rows = d.query(
                "SELECT id, type, status, dry_run, total_items, done_items, failed_items, "
                "DATE_FORMAT(created_at,'%Y-%m-%dT%H:%i:%sZ') FROM jobs ORDER BY id DESC LIMIT 100");
            send_json(res, rows_to_json(rows, {"id", "type", "status", "dry_run", "total",
                                               "done", "failed", "created_at"}));
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Get(R"(/api/v1/jobs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string id = d.escape(req.matches[1]);
            auto jr = d.query("SELECT id, type, status, dry_run, total_items, done_items, "
                              "failed_items FROM jobs WHERE id = " + id);
            if (jr.empty()) { send_error(res, 404, "no such job"); return; }
            json j;
            const std::vector<std::string> jc = {"id", "type", "status", "dry_run",
                                                 "total", "done", "failed"};
            for (size_t i = 0; i < jc.size(); ++i) j[jc[i]] = cell(jr[0], i);
            auto items = d.query("SELECT frame_id, action, src_path, dst_path, status, detail "
                                 "FROM job_items WHERE job_id = " + id + " ORDER BY id LIMIT 5000");
            j["items"] = rows_to_json(items, {"frame_id", "action", "src", "dst", "status", "detail"});
            send_json(res, j);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/jobs/:id/launcher  (download the WBPP launcher script) ----
    server->Get(R"(/api/v1/jobs/(\d+)/launcher)",
                [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string id = d.escape(req.matches[1]);
            auto r = d.query("SELECT params_json FROM jobs WHERE id = " + id +
                             " AND type = 'wbpp'");
            if (r.empty() || !r[0][0]) { send_error(res, 404, "no WBPP job with that id"); return; }
            json pp = json::parse(*r[0][0]);
            const std::string launcher = pp.value("launcher", "");
            if (launcher.empty()) { send_error(res, 404, "job has no launcher"); return; }
            res.set_header("Content-Disposition",
                           "attachment; filename=\"starbase-wbpp-job-" + id + ".sh\"");
            res.set_content(launcher, "text/x-shellscript");
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/queries/:id/paths  (REST pull for the PJSR helper) ----
    // Resolves a saved query live and returns its frame paths, so a small
    // StarBase-authored script inside PixInsight can fetch a set by id.
    server->Get(R"(/api/v1/queries/(\d+)/paths)",
                [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string id = d.escape(req.matches[1]);
            auto q = d.query("SELECT name, filter_json FROM saved_queries WHERE id = " + id);
            if (q.empty()) { send_error(res, 404, "no such query"); return; }
            const std::string name = q[0][0] ? *q[0][0] : "";
            const std::string filter = q[0][1] ? *q[0][1] : "{}";
            std::vector<starbase::action::FrameRef> frames;
            try { frames = starbase::action::resolve_by_filter(d, filter); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            d.exec("UPDATE saved_queries SET last_run_at = UTC_TIMESTAMP(), last_count = " +
                   std::to_string(frames.size()) + " WHERE id = " + id);
            if (req.has_param("format") && req.get_param_value("format") == "text") {
                std::string body;
                for (const auto& f : frames) body += f.abs_path + "\n";
                res.set_content(body, "text/plain");
                return;
            }
            json j;
            j["id"] = std::stoll(id);
            j["name"] = name;
            j["count"] = frames.size();
            json paths = json::array();
            for (const auto& f : frames) paths.push_back(f.abs_path);
            j["paths"] = paths;
            send_json(res, j);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/summary  (dashboard aggregates) ----
    server->Get("/api/v1/summary", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string by = req.has_param("by") ? req.get_param_value("by") : "object";
            std::string group, where;
            if (by == "night") group = "session_night";
            else if (by == "filter") group = "filter";
            // Objects roll up by the canonical name, so the Dashboard's target list
            // matches the Browse/Query target pulldown (the distinct object_canonical
            // facet). Frames with no canonical target are omitted from that list.
            else { group = "object_canonical";
                   where = " WHERE object_canonical IS NOT NULL AND object_canonical <> ''"; }

            auto rows = d.query(
                "SELECT COALESCE(" + group + ",'(none)') AS label, image_type, COUNT(*) AS n, "
                "ROUND(SUM(exposure_s)/3600, 2) AS hours FROM frames" + where +
                " GROUP BY " + group + ", image_type ORDER BY n DESC LIMIT 500");
            send_json(res, rows_to_json(rows, {"label", "image_type", "count", "hours"}));
        } catch (const std::exception& e) {
            send_error(res, 500, e.what());
        }
    });

    // ---- POST /api/v1/scan  (start a background scan; write-gated) ----
    // Returns immediately; progress is polled from GET /api/v1/scan/status.
    server->Post("/api/v1/scan", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        // Resolve the roots and mapping up front (needs the DB) so the worker
        // thread only touches the scanner.
        std::vector<db::RootRow> roots;
        starbase::extract::HeaderMapping mapping;
        try {
            auto d = db();
            mapping = starbase::index::load_mapping(d);
            if (req.has_param("root")) {
                auto r = d.find_root_by_label(req.get_param_value("root"));
                if (!r) { send_error(res, 404, "no such root"); return; }
                roots.push_back(*r);
            } else {
                for (const auto& r : d.list_roots()) if (r.enabled) roots.push_back(r);
            }
        } catch (const std::exception& e) { send_error(res, 500, e.what()); return; }
        if (roots.empty()) { send_error(res, 400, "no enabled roots to scan"); return; }
        const int n = static_cast<int>(roots.size());
        if (!try_start_scan(std::move(roots), std::move(mapping))) {
            send_error(res, 409, "a scan is already running"); return;
        }
        send_json(res, json{{"started", true}, {"roots", n}}, 202);
    });

    // ---- GET /api/v1/scan/status  (live scan progress) ----
    server->Get("/api/v1/scan/status", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(scan_mtx);
        json j;
        j["active"] = scan_state.active;
        j["finished"] = scan_state.finished;
        j["root"] = scan_state.root;
        j["done_roots"] = scan_state.done_roots;
        j["total_roots"] = scan_state.total_roots;
        if (scan_state.active || scan_state.finished) {
            j["seen"] = scan_state.seen; j["added"] = scan_state.added;
            j["updated"] = scan_state.updated; j["unchanged"] = scan_state.skipped;
            j["settling"] = scan_state.settling; j["error"] = scan_state.errored;
            j["frames"] = scan_state.frames; j["sidecars"] = scan_state.sidecars;
            j["results"] = scan_state.results;
            if (scan_state.active)
                j["elapsed_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - scan_state.started).count();
        }
        send_json(res, j);
    });

    // ---- Tags and collections ---------------------------------------------
    // A frame set to add/remove comes from the same {filter}|{frames:[ids]} body
    // the actions endpoint takes; this renders it to a WHERE over v_frames.
    auto set_where = [](db::Database& d, const json& body) -> std::string {
        if (body.contains("frames") && body["frames"].is_array() && !body["frames"].empty()) {
            std::string list;
            for (const auto& v : body["frames"]) {
                if (!list.empty()) list += ",";
                list += std::to_string(v.get<long long>());
            }
            return "frame_id IN (" + list + ")";
        }
        if (body.contains("filter"))
            return starbase::query::compile_filter(body["filter"], d);
        return "";
    };

    // ---- Tags: list / create / delete ----
    server->Get("/api/v1/tags", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            auto rows = d.query(
                "SELECT t.id, t.name, t.color, t.description, "
                "(SELECT COUNT(*) FROM frame_tags ft WHERE ft.tag_id = t.id) AS n "
                "FROM tags t ORDER BY t.name");
            send_json(res, rows_to_json(rows, {"id", "name", "color", "description", "count"}));
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Post("/api/v1/tags", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            json body = json::parse(req.body);
            const std::string name = body.value("name", "");
            if (name.empty()) { send_error(res, 400, "name is required"); return; }
            const std::string color = body.value("color", "");
            const std::string desc = body.value("description", "");
            d.exec("INSERT INTO tags (name, color, description) VALUES ('" + d.escape(name) + "', " +
                   (color.empty() ? "NULL" : "'" + d.escape(color) + "'") + ", " +
                   (desc.empty() ? "NULL" : "'" + d.escape(desc) + "'") + ") "
                   "ON DUPLICATE KEY UPDATE color=VALUES(color), description=VALUES(description)");
            send_json(res, json{{"saved", name}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Delete(R"(/api/v1/tags/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            d.exec("DELETE FROM tags WHERE id = " + d.escape(req.matches[1]));
            send_json(res, json{{"deleted", std::stoi(req.matches[1])}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Tag membership: bulk add / remove over a frame set ----
    server->Post(R"(/api/v1/tags/(\d+)/members)", [this, set_where](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const std::string tid = d.escape(req.matches[1]);
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string where;
            try { where = set_where(d, body); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            if (where.empty()) { send_error(res, 400, "give 'frames' or 'filter'"); return; }
            d.exec("INSERT IGNORE INTO frame_tags (frame_id, tag_id) SELECT frame_id, " + tid +
                   " FROM v_frames WHERE " + where);
            send_json(res, json{{"tag_id", std::stoi(tid)}, {"added", d.affected_rows()}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Delete(R"(/api/v1/tags/(\d+)/members)", [this, set_where](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const std::string tid = d.escape(req.matches[1]);
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string where;
            try { where = set_where(d, body); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            if (where.empty()) { send_error(res, 400, "give 'frames' or 'filter'"); return; }
            d.exec("DELETE FROM frame_tags WHERE tag_id = " + tid +
                   " AND frame_id IN (SELECT frame_id FROM v_frames WHERE " + where + ")");
            send_json(res, json{{"tag_id", std::stoi(tid)}, {"removed", d.affected_rows()}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Collections: list / create / detail / delete ----
    server->Get("/api/v1/collections", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            auto rows = d.query(
                "SELECT c.id, c.name, c.description, "
                "(SELECT COUNT(*) FROM collection_frames cf WHERE cf.collection_id = c.id) AS n, "
                "DATE_FORMAT(c.updated_at,'%Y-%m-%dT%H:%i:%sZ') FROM collections c ORDER BY c.name");
            send_json(res, rows_to_json(rows, {"id", "name", "description", "count", "updated_at"}));
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Post("/api/v1/collections", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            json body = json::parse(req.body);
            const std::string name = body.value("name", "");
            if (name.empty()) { send_error(res, 400, "name is required"); return; }
            const std::string desc = body.value("description", "");
            d.exec("INSERT INTO collections (name, description) VALUES ('" + d.escape(name) + "', " +
                   (desc.empty() ? "NULL" : "'" + d.escape(desc) + "'") + ") "
                   "ON DUPLICATE KEY UPDATE description=VALUES(description)");
            send_json(res, json{{"saved", name}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Get(R"(/api/v1/collections/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto d = db();
            const std::string id = d.escape(req.matches[1]);
            auto c = d.query("SELECT id, name, description FROM collections WHERE id = " + id);
            if (c.empty()) { send_error(res, 404, "no such collection"); return; }
            json out;
            out["id"] = cell(c[0], 0); out["name"] = cell(c[0], 1); out["description"] = cell(c[0], 2);
            auto members = d.query(
                "SELECT v.frame_id, v.image_type, v.object, v.filter, v.session_night, "
                "v.exposure_s, v.gain, v.rig, v.filename FROM collection_frames cf "
                "JOIN v_frames v ON v.frame_id = cf.frame_id WHERE cf.collection_id = " + id +
                " ORDER BY cf.position, cf.added_at LIMIT 5000");
            out["frames"] = rows_to_json(members, {"frame_id", "image_type", "object", "filter",
                                                   "session_night", "exposure_s", "gain", "rig", "filename"});
            send_json(res, out);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Delete(R"(/api/v1/collections/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            d.exec("DELETE FROM collections WHERE id = " + d.escape(req.matches[1]));
            send_json(res, json{{"deleted", std::stoi(req.matches[1])}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Collection membership: bulk add / remove ----
    server->Post(R"(/api/v1/collections/(\d+)/members)", [this, set_where](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const std::string cid = d.escape(req.matches[1]);
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string where;
            try { where = set_where(d, body); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            if (where.empty()) { send_error(res, 400, "give 'frames' or 'filter'"); return; }
            d.exec("INSERT IGNORE INTO collection_frames (collection_id, frame_id) SELECT " + cid +
                   ", frame_id FROM v_frames WHERE " + where);
            send_json(res, json{{"collection_id", std::stoi(cid)}, {"added", d.affected_rows()}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });
    server->Delete(R"(/api/v1/collections/(\d+)/members)", [this, set_where](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            auto d = db();
            const std::string cid = d.escape(req.matches[1]);
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string where;
            try { where = set_where(d, body); }
            catch (const starbase::query::QueryError& qe) { send_error(res, 400, qe.what()); return; }
            if (where.empty()) { send_error(res, 400, "give 'frames' or 'filter'"); return; }
            d.exec("DELETE FROM collection_frames WHERE collection_id = " + cid +
                   " AND frame_id IN (SELECT frame_id FROM v_frames WHERE " + where + ")");
            send_json(res, json{{"collection_id", std::stoi(cid)}, {"removed", d.affected_rows()}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- GET /api/v1/db  (schema/status: version, tables, sizes) ----
    server->Get("/api/v1/db", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            json j;
            j["schema_version"] = d.schema_version();
            j["server"] = d.server_version();
            j["database"] = cfg.db.database;
            // Table list with on-disk sizes; exact row counts per table (the
            // schema is small, so COUNT(*) is cheap and avoids the InnoDB
            // TABLE_ROWS estimate being wildly off).
            auto meta = d.query(
                "SELECT table_name, ROUND(data_length/1048576,3), ROUND(index_length/1048576,3) "
                "FROM information_schema.tables WHERE table_schema = DATABASE() "
                "AND table_type = 'BASE TABLE' ORDER BY table_name");
            json tables = json::array();
            long long total = 0;
            for (const auto& r : meta) {
                const std::string name = r[0] ? *r[0] : "";
                long long rows = 0;
                try {
                    auto cr = d.query("SELECT COUNT(*) FROM `" + name + "`");
                    if (!cr.empty() && cr[0][0]) rows = std::stoll(*cr[0][0]);
                } catch (const std::exception&) { /* skip an unreadable table */ }
                total += rows;
                tables.push_back({{"name", name}, {"rows", rows},
                                  {"data_mb", cell(r, 1)}, {"index_mb", cell(r, 2)}});
            }
            j["tables"] = tables;
            j["total_rows"] = total;
            auto fs = d.query("SELECT status, COUNT(*) FROM files GROUP BY status ORDER BY status");
            j["file_status"] = rows_to_json(fs, {"status", "count"});
            auto views = d.query("SELECT table_name FROM information_schema.views "
                                 "WHERE table_schema = DATABASE() ORDER BY table_name");
            json vs = json::array();
            for (const auto& r : views) vs.push_back(cell(r, 0));
            j["views"] = vs;
            send_json(res, j);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- POST /api/v1/db/seed  (re-apply the header mapping seed; idempotent) ----
    server->Post("/api/v1/db/seed", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a valid token is required"); return; }
        try {
            if (cfg.seed_file.empty()) { send_error(res, 400, "no seed file is configured"); return; }
            auto d = db();
            const int n = d.apply_script(cfg.seed_file);
            send_json(res, json{{"seeded", cfg.seed_file}, {"statements", n}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- POST /api/v1/db/prune-missing  (delete long-missing file rows) ----
    // Scan reconciliation soft-deletes vanished files (status='missing') so a
    // returning file re-links. This is the manual, explicit cleanup: it removes
    // those rows for good, cascading their frames and tag/collection memberships.
    // older_than_days limits it to files not seen in that many days (0 = every
    // missing file); dry_run counts without deleting.
    server->Post("/api/v1/db/prune-missing", [this](const httplib::Request& req, httplib::Response& res) {
        if (!write_allowed(req)) { send_error(res, 403, "a login is required"); return; }
        try {
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            const int days = body.value("older_than_days", 0);
            const bool dry = body.value("dry_run", false);
            auto pred = [&](const std::string& p) {
                std::string w = p + "status = 'missing'";
                if (days > 0)
                    w += " AND " + p + "last_seen_utc IS NOT NULL AND " + p +
                         "last_seen_utc < (UTC_TIMESTAMP() - INTERVAL " + std::to_string(days) + " DAY)";
                return w;
            };
            auto d = db();
            long long files = 0, frames = 0;
            if (auto r = d.query("SELECT COUNT(*) FROM files WHERE " + pred("")); !r.empty() && r[0][0])
                files = std::stoll(*r[0][0]);
            if (auto r = d.query("SELECT COUNT(*) FROM frames fr JOIN files fl ON fl.id = fr.file_id "
                                 "WHERE " + pred("fl.")); !r.empty() && r[0][0])
                frames = std::stoll(*r[0][0]);
            if (!dry && files > 0) d.exec("DELETE FROM files WHERE " + pred(""));
            send_json(res, json{{"dry_run", dry}, {"older_than_days", days},
                                {"files", files}, {"frames", frames},
                                {"pruned", dry ? 0 : files}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Authentication (NightWatcher2 pattern) ---------------------------
    const int kSessionTtl = 7 * 24 * 3600;  // 7 days

    // POST /login {username, password} -> sets the sb_session cookie.
    server->Post("/api/v1/login", [this, kSessionTtl](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            const std::string user = body.value("username", "");
            const std::string pass = body.value("password", "");
            if (user.empty() || pass.empty()) { send_error(res, 400, "username and password required"); return; }
            auto d = db();
            auto r = d.query("SELECT id, HEX(pwd_salt), HEX(pwd_hash), pwd_iterations, role, "
                             "enabled, must_change_password FROM users WHERE username = '" +
                             d.escape(user) + "' LIMIT 1");
            if (r.empty() || !r[0][0] ||
                !starbase::auth::verify_password(pass, r[0][1].value_or(""), r[0][2].value_or(""),
                                                 r[0][3] ? std::stoi(*r[0][3]) : 0)) {
                send_error(res, 401, "invalid credentials"); return;
            }
            if (r[0][5] && *r[0][5] == "0") { send_error(res, 403, "account is disabled"); return; }
            const std::string sess = starbase::auth::random_hex(32);
            d.exec("INSERT INTO sessions (token, user_id, expires_at, remote_addr, user_agent) VALUES ('" +
                   d.escape(sess) + "', " + *r[0][0] + ", UTC_TIMESTAMP() + INTERVAL " +
                   std::to_string(kSessionTtl) + " SECOND, '" + d.escape(req.remote_addr) + "', '" +
                   d.escape(req.get_header_value("User-Agent").substr(0, 255)) + "')");
            d.exec("UPDATE users SET last_login_at = UTC_TIMESTAMP() WHERE id = " + std::string(*r[0][0]));
            res.set_header("Set-Cookie", "sb_session=" + sess +
                           "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" + std::to_string(kSessionTtl));
            send_json(res, json{{"token", sess}, {"username", user}, {"role", cell(r[0], 4)},
                                {"must_change_password", r[0][6] && *r[0][6] == "1"}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Post("/api/v1/logout", [this](const httplib::Request& req, httplib::Response& res) {
        std::string sess = get_cookie(req, "sb_session");
        if (sess.empty()) { const std::string h = req.get_header_value("Authorization"); if (h.rfind("Bearer ", 0) == 0) sess = h.substr(7); }
        try { if (!sess.empty()) { auto d = db(); d.exec("DELETE FROM sessions WHERE token = '" + d.escape(sess) + "'"); } }
        catch (const std::exception&) { /* best effort */ }
        res.set_header("Set-Cookie", "sb_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        send_json(res, json{{"ok", true}});
    });

    // GET /me -> current identity, or {authenticated:false} when open/anonymous.
    server->Get("/api/v1/me", [this](const httplib::Request& req, httplib::Response& res) {
        auto c = authenticate(req);
        if (c) { send_json(res, json{{"authenticated", true}, {"username", c->username}, {"role", c->role}, {"via", c->via_token ? "token" : "session"}}); return; }
        // Not logged in: tell the UI whether auth is even set up, so it knows
        // whether to demand a login or run open.
        send_json(res, json{{"authenticated", false}, {"auth_required", cfg.token.empty() ? any_users() : true}});
    });

    // POST /me/password {current_password, new_password} for the logged-in user.
    server->Post("/api/v1/me/password", [this](const httplib::Request& req, httplib::Response& res) {
        auto c = authenticate(req);
        if (!c || c->via_token) { send_error(res, 401, "log in with a user account to change a password"); return; }
        try {
            json body = json::parse(req.body);
            const std::string cur = body.value("current_password", ""), nw = body.value("new_password", "");
            if (nw.size() < 4) { send_error(res, 400, "new password too short"); return; }
            auto d = db();
            auto r = d.query("SELECT HEX(pwd_salt), HEX(pwd_hash), pwd_iterations FROM users WHERE username = '" + d.escape(c->username) + "'");
            if (r.empty() || !starbase::auth::verify_password(cur, r[0][0].value_or(""), r[0][1].value_or(""), r[0][2] ? std::stoi(*r[0][2]) : 0)) {
                send_error(res, 403, "current password is incorrect"); return;
            }
            const auto hp = starbase::auth::hash_password(nw);
            d.exec("UPDATE users SET pwd_hash = UNHEX('" + hp.hash_hex + "'), pwd_salt = UNHEX('" + hp.salt_hex +
                   "'), pwd_iterations = " + std::to_string(hp.iterations) +
                   ", must_change_password = 0 WHERE username = '" + d.escape(c->username) + "'");
            send_json(res, json{{"ok", true}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- User management (admin only) ----
    server->Get("/api/v1/users", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            auto rows = d.query("SELECT id, username, role, enabled, must_change_password, "
                                "DATE_FORMAT(created_at,'%Y-%m-%dT%H:%i:%sZ'), "
                                "DATE_FORMAT(last_login_at,'%Y-%m-%dT%H:%i:%sZ') FROM users ORDER BY username");
            send_json(res, rows_to_json(rows, {"id", "username", "role", "enabled",
                                               "must_change_password", "created_at", "last_login_at"}));
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Post("/api/v1/users", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            const std::string user = body.value("username", ""), pass = body.value("password", "");
            std::string role = body.value("role", "user");
            if (user.empty() || pass.empty()) { send_error(res, 400, "username and password required"); return; }
            if (role != "admin" && role != "user" && role != "readonly") { send_error(res, 400, "role must be admin, user, or readonly"); return; }
            auto d = db();
            auto ex = d.query("SELECT 1 FROM users WHERE username = '" + d.escape(user) + "'");
            if (!ex.empty()) { send_error(res, 409, "user already exists"); return; }
            const auto hp = starbase::auth::hash_password(pass);
            d.exec("INSERT INTO users (username, pwd_hash, pwd_salt, pwd_iterations, role, must_change_password) "
                   "VALUES ('" + d.escape(user) + "', UNHEX('" + hp.hash_hex + "'), UNHEX('" + hp.salt_hex +
                   "'), " + std::to_string(hp.iterations) + ", '" + d.escape(role) + "', " +
                   (body.value("must_change_password", false) ? "1" : "0") + ")");
            send_json(res, json{{"username", user}, {"role", role}}, 201);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // PATCH /users/:name  {role?, enabled?} ; DELETE /users/:name
    server->Patch(R"(/api/v1/users/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            const std::string user = req.matches[1];
            json body = json::parse(req.body);
            auto d = db();
            auto u = d.query("SELECT role FROM users WHERE username = '" + d.escape(user) + "'");
            if (u.empty()) { send_error(res, 404, "no such user"); return; }
            std::vector<std::string> sets;
            if (body.contains("role")) {
                const std::string role = body["role"].get<std::string>();
                if (role != "admin" && role != "user" && role != "readonly") { send_error(res, 400, "bad role"); return; }
                // Don't let the last admin be demoted out of adminship.
                if (u[0][0] && *u[0][0] == "admin" && role != "admin") {
                    auto a = d.query("SELECT COUNT(*) FROM users WHERE role='admin' AND enabled=1");
                    if (!a.empty() && a[0][0] && std::stoll(*a[0][0]) <= 1) { send_error(res, 409, "cannot demote the last admin"); return; }
                }
                sets.push_back("role = '" + d.escape(role) + "'");
            }
            if (body.contains("enabled")) {
                const bool en = body["enabled"].get<bool>();
                if (!en && u[0][0] && *u[0][0] == "admin") {
                    auto a = d.query("SELECT COUNT(*) FROM users WHERE role='admin' AND enabled=1");
                    if (!a.empty() && a[0][0] && std::stoll(*a[0][0]) <= 1) { send_error(res, 409, "cannot disable the last admin"); return; }
                }
                sets.push_back(std::string("enabled = ") + (en ? "1" : "0"));
            }
            if (body.contains("password")) {
                const auto hp = starbase::auth::hash_password(body["password"].get<std::string>());
                sets.push_back("pwd_hash = UNHEX('" + hp.hash_hex + "')");
                sets.push_back("pwd_salt = UNHEX('" + hp.salt_hex + "')");
                sets.push_back("pwd_iterations = " + std::to_string(hp.iterations));
                sets.push_back("must_change_password = 1");
            }
            if (sets.empty()) { send_error(res, 400, "nothing to update"); return; }
            std::string sql = "UPDATE users SET ";
            for (size_t i = 0; i < sets.size(); ++i) sql += (i ? ", " : "") + sets[i];
            sql += " WHERE username = '" + d.escape(user) + "'";
            d.exec(sql);
            send_json(res, json{{"updated", user}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/users/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            const std::string user = req.matches[1];
            auto d = db();
            auto u = d.query("SELECT role FROM users WHERE username = '" + d.escape(user) + "'");
            if (u.empty()) { send_error(res, 404, "no such user"); return; }
            if (u[0][0] && *u[0][0] == "admin") {
                auto a = d.query("SELECT COUNT(*) FROM users WHERE role='admin'");
                if (!a.empty() && a[0][0] && std::stoll(*a[0][0]) <= 1) { send_error(res, 409, "cannot delete the last admin"); return; }
            }
            d.exec("DELETE FROM users WHERE username = '" + d.escape(user) + "'");
            send_json(res, json{{"deleted", user}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Target-name normalization ----------------------------------------
    // Distinct raw OBJECT values with their frame counts, the currently stored
    // canonical, an offline catalog proposal, and any cached Sesame resolution.
    server->Get("/api/v1/objects", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto d = db();
            auto rows = d.query(
                "SELECT object, MAX(object_canonical), COUNT(*) FROM frames "
                "WHERE object IS NOT NULL AND object <> '' GROUP BY object ORDER BY object");
            json cache = json::object();
            for (const auto& c : d.query("SELECT raw, canonical, ra_deg, dec_deg, otype "
                                         "FROM object_names WHERE source = 'sesame'"))
                if (c[0]) cache[*c[0]] = {{"name", cell(c, 1)}, {"ra_deg", cell(c, 2)},
                                          {"dec_deg", cell(c, 3)}, {"otype", cell(c, 4)}};
            json arr = json::array();
            for (const auto& r : rows) {
                const std::string raw = r[0] ? *r[0] : "";
                const auto c = starbase::names::canonicalize(raw);
                json o = {{"raw", raw}, {"count", r[2] ? std::stoll(*r[2]) : 0},
                          {"current", cell(r, 1)}, {"offline", c.canonical},
                          {"catalog", c.catalog}, {"placeholder", c.placeholder},
                          {"changed", c.changed}};
                o["sesame"] = cache.contains(raw) ? cache[raw] : json(nullptr);
                arr.push_back(std::move(o));
            }
            send_json(res, json{{"sesame_enabled", cfg.sesame_enabled}, {"objects", arr}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // Resolve names through CDS Sesame (opt-in) and cache the results. Admin.
    server->Post("/api/v1/objects/resolve", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        if (!cfg.sesame_enabled) { send_error(res, 400, "Sesame is disabled; set [names] sesame_enabled = on"); return; }
        try {
            auto d = db();
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::vector<std::string> names;
            if (body.contains("names") && body["names"].is_array())
                for (const auto& n : body["names"]) names.push_back(n.get<std::string>());
            else
                for (const auto& r : d.query("SELECT DISTINCT object FROM frames WHERE object IS NOT NULL AND object <> ''"))
                    if (r[0]) names.push_back(*r[0]);
            json out = json::array();
            for (const auto& raw : names) {
                if (starbase::names::canonicalize(raw).placeholder) { out.push_back({{"raw", raw}, {"skipped", "placeholder"}}); continue; }
                const auto sr = starbase::names::sesame_resolve(raw, cfg.sesame_url);
                if (sr.ok) {
                    d.exec("INSERT INTO object_names (raw, canonical, ra_deg, dec_deg, otype, source, placeholder) VALUES ('" +
                           d.escape(raw) + "', " + (sr.name.empty() ? "NULL" : "'" + d.escape(sr.name) + "'") + ", " +
                           (sr.has_coords ? std::to_string(sr.ra_deg) : "NULL") + ", " +
                           (sr.has_coords ? std::to_string(sr.dec_deg) : "NULL") + ", " +
                           (sr.otype.empty() ? "NULL" : "'" + d.escape(sr.otype) + "'") + ", 'sesame', 0) "
                           "ON DUPLICATE KEY UPDATE canonical=VALUES(canonical), ra_deg=VALUES(ra_deg), "
                           "dec_deg=VALUES(dec_deg), otype=VALUES(otype), source='sesame'");
                    out.push_back({{"raw", raw}, {"name", sr.name}, {"otype", sr.otype},
                                   {"ra_deg", sr.ra_deg}, {"dec_deg", sr.dec_deg}, {"has_coords", sr.has_coords}});
                } else {
                    out.push_back({{"raw", raw}, {"error", sr.error}});
                }
            }
            send_json(res, out);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // Write a raw -> canonical mapping into frames.object_canonical (raw OBJECT
    // is preserved). Dry-run by default: preview the per-name frame counts.
    server->Post("/api/v1/objects/apply", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            if (!body.contains("mapping") || !body["mapping"].is_object()) { send_error(res, 400, "mapping is required"); return; }
            const bool dry = body.value("dry_run", true);
            auto d = db();
            json changes = json::array();
            long long total = 0;
            for (auto& [raw, v] : body["mapping"].items()) {
                if (!v.is_string()) continue;
                const std::string canon = v.get<std::string>();
                if (canon.empty()) continue;
                auto cnt = d.query("SELECT COUNT(*) FROM frames WHERE object = '" + d.escape(raw) +
                                   "' AND (object_canonical IS NULL OR object_canonical <> '" + d.escape(canon) + "')");
                const long long n = (!cnt.empty() && cnt[0][0]) ? std::stoll(*cnt[0][0]) : 0;
                if (n == 0) continue;
                total += n;
                changes.push_back({{"raw", raw}, {"canonical", canon}, {"frames", n}});
                if (!dry) {
                    d.exec("UPDATE frames SET object_canonical = '" + d.escape(canon) + "' WHERE object = '" + d.escape(raw) + "'");
                    d.exec("INSERT INTO object_names (raw, canonical, source, placeholder) VALUES ('" +
                           d.escape(raw) + "', '" + d.escape(canon) + "', 'offline', 0) "
                           "ON DUPLICATE KEY UPDATE canonical=VALUES(canonical)");
                }
            }
            send_json(res, json{{"dry_run", dry}, {"total_frames", total}, {"changes", changes}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Equipment builder: rigs from camera + focal-length combinations ----
    // Cameras and filters auto-create during a scan; rigs do not (they pair a
    // camera with a focal range). This suggests rig definitions from the
    // combinations actually present, and creating one back-fills rig_id.
    server->Get("/api/v1/equipment", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            auto camrows = d.query("SELECT c.id, c.model, "
                "(SELECT COUNT(*) FROM frames f WHERE f.camera_id = c.id) FROM cameras c ORDER BY c.model");
            auto trows = d.query("SELECT id, name FROM telescopes ORDER BY name");
            auto rrows = d.query(
                "SELECT r.id, r.name, r.camera_id, c.model, t.name, r.focal_min_mm, r.focal_max_mm, "
                "(SELECT COUNT(*) FROM frames f WHERE f.rig_id = r.id), r.site_id, s.name, "
                "r.active_flat_set_id, afs.label FROM rigs r "
                "JOIN cameras c ON c.id = r.camera_id LEFT JOIN telescopes t ON t.id = r.telescope_id "
                "LEFT JOIN sites s ON s.id = r.site_id "
                "LEFT JOIN flat_sets afs ON afs.id = r.active_flat_set_id ORDER BY r.name");
            auto siterows = d.query(
                "SELECT s.id, s.name, s.latitude, s.longitude, s.elevation_m, s.timezone, "
                "(SELECT COUNT(*) FROM frames f WHERE f.site_id = s.id) FROM sites s ORDER BY s.name");
            // Flat sets: which flats belong together, per rig, and which one is
            // active. is_site marks fixed rigs (sticky set) vs mobile (per-night).
            auto fsrows = d.query(
                "SELECT fs.id, fs.rig_id, r.name, fs.label, fs.source, fs.captured_night, "
                "fs.valid_from, fs.valid_to, "
                "(SELECT COUNT(*) FROM frames f WHERE f.flat_set_id = fs.id), "
                "(r.active_flat_set_id = fs.id), (r.site_id IS NOT NULL) FROM flat_sets fs "
                "JOIN rigs r ON r.id = fs.rig_id ORDER BY r.name, fs.captured_night, fs.label");
            auto pinrows = d.query(
                "SELECT p.id, p.flat_set_id, fs.label, p.scope, p.rig_id, r.name, p.session_night, "
                "p.saved_query_id, q.name FROM light_flat_pins p JOIN flat_sets fs ON fs.id = p.flat_set_id "
                "LEFT JOIN rigs r ON r.id = p.rig_id LEFT JOIN saved_queries q ON q.id = p.saved_query_id "
                "ORDER BY p.id");
            // Existing rig ranges, to skip already-covered clusters.
            struct Range { long long cam; double lo, hi; };
            std::vector<Range> covered;
            for (const auto& r : rrows)
                if (r[2] && r[5] && r[6]) covered.push_back({std::stoll(*r[2]), std::stod(*r[5]), std::stod(*r[6])});

            // Distinct camera+focal counts, clustered per camera within ~2%.
            auto frows = d.query(
                "SELECT f.camera_id, c.model, f.focal_len_mm, COUNT(*) FROM frames f "
                "JOIN cameras c ON c.id = f.camera_id WHERE f.camera_id IS NOT NULL "
                "AND f.focal_len_mm IS NOT NULL GROUP BY f.camera_id, f.focal_len_mm "
                "ORDER BY f.camera_id, f.focal_len_mm");
            json suggestions = json::array();
            size_t i = 0;
            while (i < frows.size()) {
                if (!frows[i][0] || !frows[i][2]) { ++i; continue; }
                const long long cam = std::stoll(*frows[i][0]);
                const std::string model = frows[i][1] ? *frows[i][1] : "";
                double lo = std::stod(*frows[i][2]), hi = lo;
                long long cnt = frows[i][3] ? std::stoll(*frows[i][3]) : 0;
                size_t j = i + 1;
                while (j < frows.size() && frows[j][0] && std::stoll(*frows[j][0]) == cam && frows[j][2]) {
                    const double fl = std::stod(*frows[j][2]);
                    if (fl <= hi * 1.02) { hi = fl; cnt += frows[j][3] ? std::stoll(*frows[j][3]) : 0; ++j; }
                    else break;
                }
                bool cov = false;
                for (const auto& rg : covered)
                    if (rg.cam == cam && !(hi < rg.lo || lo > rg.hi)) { cov = true; break; }
                if (!cov)
                    suggestions.push_back({{"camera_id", cam}, {"camera", model},
                        {"focal_min", lo}, {"focal_max", hi}, {"frames", cnt},
                        {"suggest_min", std::floor(lo - std::max(lo * 0.015, 1.0))},
                        {"suggest_max", std::ceil(hi + std::max(hi * 0.015, 1.0))}});
                i = j;
            }
            send_json(res, json{
                {"cameras", rows_to_json(camrows, {"id", "model", "frames"})},
                {"telescopes", rows_to_json(trows, {"id", "name"})},
                {"rigs", rows_to_json(rrows, {"id", "name", "camera_id", "camera", "telescope",
                                              "focal_min_mm", "focal_max_mm", "frames", "site_id", "site",
                                              "active_flat_set_id", "active_flat_set"})},
                {"sites", rows_to_json(siterows, {"id", "name", "latitude", "longitude",
                                                  "elevation_m", "timezone", "frames"})},
                {"flat_sets", rows_to_json(fsrows, {"id", "rig_id", "rig", "label", "source",
                                                    "captured_night", "valid_from", "valid_to",
                                                    "frames", "is_active", "is_site"})},
                {"flat_pins", rows_to_json(pinrows, {"id", "flat_set_id", "flat_set", "scope",
                                                     "rig_id", "rig", "session_night",
                                                     "saved_query_id", "saved_query"})},
                {"suggestions", suggestions}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Post("/api/v1/equipment/rigs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            const std::string name = body.value("name", "");
            const long long cam = body.value("camera_id", 0LL);
            const double fmin = body.value("focal_min_mm", 0.0), fmax = body.value("focal_max_mm", 0.0);
            if (name.empty() || cam <= 0) { send_error(res, 400, "name and camera_id are required"); return; }
            if (fmin <= 0 || fmax < fmin) { send_error(res, 400, "focal range is invalid"); return; }
            auto d = db();
            std::string tel = "NULL";
            if (body.contains("telescope") && body["telescope"].is_string() && !body["telescope"].get<std::string>().empty()) {
                const std::string tn = body["telescope"].get<std::string>();
                d.exec("INSERT IGNORE INTO telescopes (name) VALUES ('" + d.escape(tn) + "')");
                auto tr = d.query("SELECT id FROM telescopes WHERE name = '" + d.escape(tn) + "'");
                if (!tr.empty() && tr[0][0]) tel = *tr[0][0];
            }
            const long long site = body.value("site_id", 0LL);
            const std::string site_sql = site > 0 ? std::to_string(site) : "NULL";
            long long rig_id;
            try {
                rig_id = d.exec("INSERT INTO rigs (name, camera_id, telescope_id, site_id, focal_min_mm, focal_max_mm) "
                                "VALUES ('" + d.escape(name) + "', " + std::to_string(cam) + ", " + tel + ", " +
                                site_sql + ", " + std::to_string(fmin) + ", " + std::to_string(fmax) + ")");
            } catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            // Back-fill rig_id (and site_id when the rig carries a site, so the
            // rig's frames get the location too).
            d.exec("UPDATE frames SET rig_id = " + std::to_string(rig_id) +
                   (site > 0 ? ", site_id = " + std::to_string(site) : "") + " WHERE camera_id = " +
                   std::to_string(cam) + " AND focal_len_mm BETWEEN " + std::to_string(fmin) +
                   " AND " + std::to_string(fmax));
            json out = {{"id", rig_id}, {"name", name}, {"assigned", d.affected_rows()}};
            if (auto w = rig_overlap_warning(d, cam, fmin, fmax, rig_id); !w.empty()) out["warning"] = w;
            send_json(res, out, 201);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Patch(R"(/api/v1/equipment/rigs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            const std::string id = std::string(req.matches[1]);
            json body = json::parse(req.body);
            auto d = db();
            auto cur = d.query("SELECT camera_id, focal_min_mm, focal_max_mm, site_id FROM rigs WHERE id = " + id);
            if (cur.empty()) { send_error(res, 404, "no such rig"); return; }
            std::vector<std::string> sets;
            if (body.contains("name")) sets.push_back("name = '" + d.escape(body["name"].get<std::string>()) + "'");
            // Camera and focal range both drive frame membership; a change to
            // either recomputes which frames the rig owns.
            long long cam = cur[0][0] ? std::stoll(*cur[0][0]) : 0;
            bool membership_changed = false;
            if (body.contains("camera_id") && body["camera_id"].get<long long>() > 0) {
                cam = body["camera_id"].get<long long>();
                sets.push_back("camera_id = " + std::to_string(cam));
                membership_changed = true;
            }
            double fmin = cur[0][1] ? std::stod(*cur[0][1]) : 0, fmax = cur[0][2] ? std::stod(*cur[0][2]) : 0;
            if (body.contains("focal_min_mm")) { fmin = body["focal_min_mm"].get<double>(); sets.push_back("focal_min_mm = " + std::to_string(fmin)); membership_changed = true; }
            if (body.contains("focal_max_mm")) { fmax = body["focal_max_mm"].get<double>(); sets.push_back("focal_max_mm = " + std::to_string(fmax)); membership_changed = true; }
            if (fmax < fmin) { send_error(res, 400, "focal range is invalid"); return; }
            // Telescope: a name (created if new), or empty to clear it.
            if (body.contains("telescope") && body["telescope"].is_string()) {
                const std::string tn = body["telescope"].get<std::string>();
                if (tn.empty()) sets.push_back("telescope_id = NULL");
                else {
                    d.exec("INSERT IGNORE INTO telescopes (name) VALUES ('" + d.escape(tn) + "')");
                    auto tr = d.query("SELECT id FROM telescopes WHERE name = '" + d.escape(tn) + "'");
                    if (!tr.empty() && tr[0][0]) sets.push_back("telescope_id = " + *tr[0][0]);
                }
            }
            // Site: 0 clears it, >0 sets it (and re-stamps the rig's frames below).
            long long rig_site = cur[0][3] ? std::stoll(*cur[0][3]) : 0;
            bool site_provided = false;
            if (body.contains("site_id")) {
                rig_site = body["site_id"].get<long long>();
                site_provided = true;
                sets.push_back(rig_site > 0 ? "site_id = " + std::to_string(rig_site) : "site_id = NULL");
            }
            // Active (sticky) flat set for a fixed rig: 0 clears; >0 must belong
            // to this rig.
            if (body.contains("active_flat_set_id")) {
                const long long afs = body["active_flat_set_id"].get<long long>();
                if (afs > 0) {
                    auto chk = d.query("SELECT id FROM flat_sets WHERE id = " + std::to_string(afs) +
                                       " AND rig_id = " + id);
                    if (chk.empty()) { send_error(res, 400, "flat set does not belong to this rig"); return; }
                    sets.push_back("active_flat_set_id = " + std::to_string(afs));
                } else {
                    sets.push_back("active_flat_set_id = NULL");
                }
            }
            if (sets.empty()) { send_error(res, 400, "nothing to update"); return; }
            std::string sql = "UPDATE rigs SET ";
            for (size_t i = 0; i < sets.size(); ++i) sql += (i ? ", " : "") + sets[i];
            try { d.exec(sql + " WHERE id = " + id); }
            catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            long long reassigned = 0;
            if (membership_changed && cam > 0) {
                d.exec("UPDATE frames SET rig_id = NULL WHERE rig_id = " + id);  // drop old membership
                d.exec("UPDATE frames SET rig_id = " + id + " WHERE camera_id = " + std::to_string(cam) +
                       " AND focal_len_mm BETWEEN " + std::to_string(fmin) + " AND " + std::to_string(fmax));
                reassigned = d.affected_rows();
            }
            // Stamp the rig's site onto its frames when the rig carries one and
            // either its membership or its site just changed.
            if (rig_site > 0 && (membership_changed || site_provided))
                d.exec("UPDATE frames SET site_id = " + std::to_string(rig_site) + " WHERE rig_id = " + id);
            json out = {{"updated", std::stoi(id)}, {"reassigned", reassigned}};
            if (auto w = rig_overlap_warning(d, cam, fmin, fmax, std::stoll(id)); !w.empty()) out["warning"] = w;
            send_json(res, out);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/equipment/rigs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const std::string id = std::string(req.matches[1]);
            d.exec("UPDATE frames SET rig_id = NULL WHERE rig_id = " + id);
            const long long freed = d.affected_rows();
            d.exec("DELETE FROM rigs WHERE id = " + id);  // FK also sets frames.rig_id NULL
            send_json(res, json{{"deleted", std::stoi(id)}, {"freed", freed}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Site builder: cluster frames by observatory location ---------------
    // Reads each frame's location from its header cards, clusters points within
    // `distance` metres, and skips clusters already covered by a site.
    server->Get("/api/v1/equipment/site-suggestions", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const double dist = static_cast<double>(qint(req, "distance", 250, 1, 1000000));
            auto locs = frame_locations(d);
            // Greedy cluster: a point joins the first cluster within `dist` of its
            // centroid, else starts a new one (centroid = frame-weighted mean).
            struct Cluster { double lat, lon; long long n; };
            std::vector<Cluster> clusters;
            for (const auto& p : locs) {
                bool placed = false;
                for (auto& c : clusters) {
                    if (haversine_m(c.lat, c.lon, p.lat, p.lon) <= dist) {
                        c.lat = (c.lat * c.n + p.lat) / (c.n + 1);
                        c.lon = (c.lon * c.n + p.lon) / (c.n + 1);
                        c.n += 1; placed = true; break;
                    }
                }
                if (!placed) clusters.push_back({p.lat, p.lon, 1});
            }
            auto sites = d.query("SELECT id, name, latitude, longitude FROM sites WHERE latitude IS NOT NULL");
            json out = json::array();
            for (const auto& c : clusters) {
                json o = {{"latitude", c.lat}, {"longitude", c.lon}, {"frames", c.n}};
                std::string covered;
                for (const auto& s : sites)
                    if (s[2] && s[3] && haversine_m(std::stod(*s[2]), std::stod(*s[3]), c.lat, c.lon) <= dist) {
                        covered = s[1] ? *s[1] : ""; break;
                    }
                if (!covered.empty()) o["covered_by"] = covered;
                out.push_back(std::move(o));
            }
            send_json(res, json{{"distance_m", dist}, {"clusters", out}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Post("/api/v1/equipment/sites", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            const std::string name = body.value("name", "");
            if (name.empty() || !body.contains("latitude") || !body.contains("longitude")) {
                send_error(res, 400, "name, latitude and longitude are required"); return;
            }
            const double lat = body["latitude"].get<double>(), lon = body["longitude"].get<double>();
            const double dist = body.value("distance_m", 250.0);
            auto d = db();
            std::string cols = "name, latitude, longitude", vals = "'" + d.escape(name) + "', " +
                std::to_string(lat) + ", " + std::to_string(lon);
            if (body.contains("elevation_m")) { cols += ", elevation_m"; vals += ", " + std::to_string(body["elevation_m"].get<double>()); }
            if (body.contains("timezone") && body["timezone"].is_string() && !body["timezone"].get<std::string>().empty()) {
                cols += ", timezone"; vals += ", '" + d.escape(body["timezone"].get<std::string>()) + "'";
            }
            long long site_id;
            try { site_id = d.exec("INSERT INTO sites (" + cols + ") VALUES (" + vals + ")"); }
            catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            // Assign every frame within `dist` of the new site.
            std::vector<long long> ids;
            for (const auto& p : frame_locations(d))
                if (haversine_m(lat, lon, p.lat, p.lon) <= dist) ids.push_back(p.id);
            long long assigned = 0;
            for (size_t i = 0; i < ids.size(); i += 1000) {
                std::string list;
                for (size_t j = i; j < ids.size() && j < i + 1000; ++j) list += (list.empty() ? "" : ",") + std::to_string(ids[j]);
                if (!list.empty()) { d.exec("UPDATE frames SET site_id = " + std::to_string(site_id) + " WHERE id IN (" + list + ")"); assigned += d.affected_rows(); }
            }
            send_json(res, json{{"id", site_id}, {"name", name}, {"assigned", assigned}}, 201);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // Edit a site's attributes. This corrects the label (name, coordinates,
    // timezone, elevation); it does not re-cluster or re-assign frames, so a
    // frame keeps its site until you delete the site or rescan.
    server->Patch(R"(/api/v1/equipment/sites/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            const std::string id = std::string(req.matches[1]);
            json body = json::parse(req.body);
            auto d = db();
            auto cur = d.query("SELECT id FROM sites WHERE id = " + id);
            if (cur.empty()) { send_error(res, 404, "no such site"); return; }
            std::vector<std::string> sets;
            if (body.contains("name")) {
                const std::string nm = body["name"].get<std::string>();
                if (nm.empty()) { send_error(res, 400, "name cannot be empty"); return; }
                sets.push_back("name = '" + d.escape(nm) + "'");
            }
            if (body.contains("latitude"))  sets.push_back("latitude = " + std::to_string(body["latitude"].get<double>()));
            if (body.contains("longitude")) sets.push_back("longitude = " + std::to_string(body["longitude"].get<double>()));
            if (body.contains("elevation_m")) {
                if (body["elevation_m"].is_null()) sets.push_back("elevation_m = NULL");
                else sets.push_back("elevation_m = " + std::to_string(body["elevation_m"].get<double>()));
            }
            if (body.contains("timezone") && body["timezone"].is_string()) {
                const std::string tz = body["timezone"].get<std::string>();
                sets.push_back(tz.empty() ? "timezone = NULL" : "timezone = '" + d.escape(tz) + "'");
            }
            if (sets.empty()) { send_error(res, 400, "nothing to update"); return; }
            std::string sql = "UPDATE sites SET ";
            for (size_t i = 0; i < sets.size(); ++i) sql += (i ? ", " : "") + sets[i];
            try { d.exec(sql + " WHERE id = " + id); }
            catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            send_json(res, json{{"updated", std::stoi(id)}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/equipment/sites/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const std::string id = std::string(req.matches[1]);
            d.exec("UPDATE frames SET site_id = NULL WHERE site_id = " + id);
            const long long freed = d.affected_rows();
            d.exec("DELETE FROM sites WHERE id = " + id);
            send_json(res, json{{"deleted", std::stoi(id)}, {"freed", freed}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Flat sets: bind a specific group of flats to lights ----------------
    // Suggestions: contiguous runs of not-yet-assigned flats per rig, clustered
    // by DATE-OBS gap. This is the flat analogue of the location-cluster builder.
    server->Get("/api/v1/equipment/flat-set-suggestions", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const long long gap_s = qint(req, "gap_minutes", 45, 1, 100000) * 60LL;
            auto rows = d.query(
                "SELECT f.rig_id, r.name, f.date_obs_utc, f.session_night, "
                "COALESCE(fl.name, f.filter_raw), UNIX_TIMESTAMP(f.date_obs_utc) "
                "FROM frames f JOIN rigs r ON r.id = f.rig_id "
                "LEFT JOIN filters fl ON fl.id = f.filter_id "
                "WHERE f.image_type = 'flat' AND f.flat_set_id IS NULL AND f.rig_id IS NOT NULL "
                "AND f.date_obs_utc IS NOT NULL ORDER BY f.rig_id, f.date_obs_utc");
            json out = json::array();
            json cur;
            long long cur_rig = -1, last_epoch = 0;
            auto flush = [&]() { if (!cur.is_null()) { out.push_back(cur); cur = json(); } };
            for (const auto& r : rows) {
                if (!r[0] || !r[2] || !r[5]) continue;
                const long long rig = std::stoll(*r[0]);
                const long long epoch = std::stoll(*r[5]);
                const std::string filt = r[4] ? *r[4] : "";
                if (cur.is_null() || rig != cur_rig || (epoch - last_epoch) > gap_s) {
                    flush();
                    cur = {{"rig_id", rig}, {"rig", r[1] ? *r[1] : ""},
                           {"start_utc", *r[2]}, {"end_utc", *r[2]},
                           {"session_night", r[3] ? json(*r[3]) : json(nullptr)},
                           {"frames", 0}, {"filters", json::array()}};
                    cur_rig = rig;
                }
                cur["end_utc"] = *r[2];
                cur["frames"] = cur["frames"].get<long long>() + 1;
                if (!filt.empty()) {
                    auto& fa = cur["filters"];
                    if (std::find(fa.begin(), fa.end(), filt) == fa.end()) fa.push_back(filt);
                }
                last_epoch = epoch;
            }
            flush();
            send_json(res, json{{"gap_minutes", gap_s / 60}, {"clusters", out}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // Create a flat set from a rig + time window; assigns the flats (and any
    // master flats) captured in that window, and optionally makes it active.
    server->Post("/api/v1/equipment/flat-sets", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            const long long rig = body.value("rig_id", 0LL);
            const std::string label = body.value("label", "");
            const std::string start = body.value("start_utc", "");
            const std::string end = body.value("end_utc", "");
            if (rig <= 0 || label.empty() || start.empty() || end.empty()) {
                send_error(res, 400, "rig_id, label, start_utc and end_utc are required"); return;
            }
            auto d = db();
            std::string cols = "rig_id, label, source, captured_start_utc, captured_end_utc";
            std::string vals = std::to_string(rig) + ", '" + d.escape(label) + "', '" +
                d.escape(body.value("source", std::string("inferred"))) + "', '" +
                d.escape(start) + "', '" + d.escape(end) + "'";
            for (const char* k : {"valid_from", "valid_to"})
                if (body.contains(k) && body[k].is_string() && !body[k].get<std::string>().empty()) {
                    cols += std::string(", ") + k; vals += ", '" + d.escape(body[k].get<std::string>()) + "'";
                }
            long long set_id;
            try { set_id = d.exec("INSERT INTO flat_sets (" + cols + ") VALUES (" + vals + ")"); }
            catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            // Assign the rig's raw flats and master flats captured in the window
            // that are not already claimed by another set.
            d.exec("UPDATE frames SET flat_set_id = " + std::to_string(set_id) +
                   " WHERE rig_id = " + std::to_string(rig) + " AND flat_set_id IS NULL "
                   "AND (image_type = 'flat' OR (image_type = 'master' AND master_of = 'flat')) "
                   "AND date_obs_utc BETWEEN '" + d.escape(start) + "' AND '" + d.escape(end) + "'");
            const long long assigned = d.affected_rows();
            d.exec("UPDATE flat_sets SET frame_count = " + std::to_string(assigned) +
                   ", captured_night = (SELECT MIN(session_night) FROM frames WHERE flat_set_id = " +
                   std::to_string(set_id) + ") WHERE id = " + std::to_string(set_id));
            if (body.value("set_active", false))
                d.exec("UPDATE rigs SET active_flat_set_id = " + std::to_string(set_id) +
                       " WHERE id = " + std::to_string(rig));
            send_json(res, json{{"id", set_id}, {"label", label}, {"assigned", assigned}}, 201);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Patch(R"(/api/v1/equipment/flat-sets/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            const std::string id = std::string(req.matches[1]);
            json body = json::parse(req.body);
            auto d = db();
            if (d.query("SELECT id FROM flat_sets WHERE id = " + id).empty()) {
                send_error(res, 404, "no such flat set"); return;
            }
            std::vector<std::string> sets;
            if (body.contains("label")) {
                const std::string nm = body["label"].get<std::string>();
                if (nm.empty()) { send_error(res, 400, "label cannot be empty"); return; }
                sets.push_back("label = '" + d.escape(nm) + "'");
            }
            if (body.contains("notes"))
                sets.push_back(body["notes"].is_null() ? "notes = NULL"
                               : "notes = '" + d.escape(body["notes"].get<std::string>()) + "'");
            for (const char* k : {"valid_from", "valid_to"})
                if (body.contains(k)) {
                    if (body[k].is_null() || (body[k].is_string() && body[k].get<std::string>().empty()))
                        sets.push_back(std::string(k) + " = NULL");
                    else sets.push_back(std::string(k) + " = '" + d.escape(body[k].get<std::string>()) + "'");
                }
            if (sets.empty()) { send_error(res, 400, "nothing to update"); return; }
            std::string sql = "UPDATE flat_sets SET ";
            for (size_t i = 0; i < sets.size(); ++i) sql += (i ? ", " : "") + sets[i];
            try { d.exec(sql + " WHERE id = " + id); }
            catch (const db::DbError& de) { send_error(res, de.db_errno() == 1062 ? 409 : 500, de.what()); return; }
            send_json(res, json{{"updated", std::stoi(id)}});
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/equipment/flat-sets/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const std::string id = std::string(req.matches[1]);
            d.exec("UPDATE frames SET flat_set_id = NULL WHERE flat_set_id = " + id);
            const long long freed = d.affected_rows();
            d.exec("UPDATE rigs SET active_flat_set_id = NULL WHERE active_flat_set_id = " + id);
            d.exec("DELETE FROM flat_sets WHERE id = " + id);  // pins cascade via FK
            send_json(res, json{{"deleted", std::stoi(id)}, {"freed", freed}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // Explicit override pins: (rig + session_night) or a saved query -> a set.
    server->Post("/api/v1/equipment/flat-pins", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            const long long set_id = body.value("flat_set_id", 0LL);
            const std::string scope = body.value("scope", "");
            if (set_id <= 0) { send_error(res, 400, "flat_set_id is required"); return; }
            auto d = db();
            if (d.query("SELECT id FROM flat_sets WHERE id = " + std::to_string(set_id)).empty()) {
                send_error(res, 404, "no such flat set"); return;
            }
            if (scope == "rig_night") {
                const long long rig = body.value("rig_id", 0LL);
                const std::string night = body.value("session_night", "");
                if (rig <= 0 || night.empty()) { send_error(res, 400, "rig_id and session_night are required"); return; }
                // One pin per (rig, night); a repeat re-points it.
                d.exec("INSERT INTO light_flat_pins (flat_set_id, scope, rig_id, session_night) VALUES (" +
                       std::to_string(set_id) + ", 'rig_night', " + std::to_string(rig) + ", '" +
                       d.escape(night) + "') ON DUPLICATE KEY UPDATE flat_set_id = VALUES(flat_set_id)");
            } else if (scope == "saved_query") {
                const long long q = body.value("saved_query_id", 0LL);
                if (q <= 0) { send_error(res, 400, "saved_query_id is required"); return; }
                d.exec("DELETE FROM light_flat_pins WHERE scope = 'saved_query' AND saved_query_id = " + std::to_string(q));
                d.exec("INSERT INTO light_flat_pins (flat_set_id, scope, saved_query_id) VALUES (" +
                       std::to_string(set_id) + ", 'saved_query', " + std::to_string(q) + ")");
            } else { send_error(res, 400, "scope must be rig_night or saved_query"); return; }
            send_json(res, json{{"ok", true}}, 201);
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Delete(R"(/api/v1/equipment/flat-pins/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            auto d = db();
            const std::string id = std::string(req.matches[1]);
            d.exec("DELETE FROM light_flat_pins WHERE id = " + id);
            send_json(res, json{{"deleted", std::stoi(id)}});
        } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // ---- Server settings (bind/port/tls) ----------------------------------
    // The running values are what this process actually bound; the configured
    // values are the DB overrides that take effect on the next rebind. The UI
    // shows both and offers "Restart & apply".
    auto server_settings = [this]() {
        auto d = db();
        std::string cbind = cfg.bind; int cport = cfg.port; bool ctls = cfg.tls;
        if (auto v = d.get_setting("api_bind"); v && !v->empty()) cbind = *v;
        if (auto v = d.get_setting("api_port"); v && !v->empty()) { try { cport = std::stoi(*v); } catch (const std::exception&) {} }
        if (auto v = d.get_setting("api_tls"); v) ctls = (*v == "on");
        return json{{"running", {{"bind", cfg.bind}, {"port", cfg.port}, {"tls", cfg.tls}}},
                    {"configured", {{"bind", cbind}, {"port", cport}, {"tls", ctls}}},
                    {"restart_required", cbind != cfg.bind || cport != cfg.port || ctls != cfg.tls},
                    {"can_apply", static_cast<bool>(cfg.on_apply)}};
    };

    server->Get("/api/v1/settings", [this, server_settings](const httplib::Request&, httplib::Response& res) {
        try { send_json(res, server_settings()); } catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    server->Put("/api/v1/settings", [this, server_settings](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json body = json::parse(req.body);
            auto d = db();
            if (body.contains("bind") && body["bind"].is_string()) {
                const std::string b = body["bind"].get<std::string>();
                if (!valid_bind(b)) { send_error(res, 400, "invalid bind address"); return; }
                d.set_setting("api_bind", b);
            }
            if (body.contains("port")) {
                const int p = body["port"].get<int>();
                if (p < 1 || p > 65535) { send_error(res, 400, "port out of range (1-65535)"); return; }
                d.set_setting("api_port", std::to_string(p));
            }
            if (body.contains("tls")) d.set_setting("api_tls", body["tls"].get<bool>() ? "on" : "off");
            send_json(res, server_settings());
        } catch (const json::exception& e) { send_error(res, 400, std::string("invalid JSON: ") + e.what()); }
          catch (const std::exception& e) { send_error(res, 500, e.what()); }
    });

    // POST /settings/apply: send the response first, then ask the daemon to
    // rebind (on_apply raises a signal; the rebind happens a beat later, so this
    // response still flushes to the caller on the old listener).
    server->Post("/api/v1/settings/apply", [this, server_settings](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(req, res)) return;
        try {
            json j = server_settings();
            j["applying"] = static_cast<bool>(cfg.on_apply);
            send_json(res, j);
        } catch (const std::exception& e) { send_error(res, 500, e.what()); return; }
        if (cfg.on_apply) cfg.on_apply();
    });

    // GET /interfaces: the host's bindable addresses, for the Server tab's
    // bind picker. Always offers all-interfaces and localhost.
    server->Get("/api/v1/interfaces", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        arr.push_back({{"name", "all interfaces"}, {"address", "0.0.0.0"}});
        arr.push_back({{"name", "localhost"}, {"address", "127.0.0.1"}});
        struct ifaddrs* ifa = nullptr;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                char host[NI_MAXHOST];
                if (getnameinfo(p->ifa_addr, sizeof(struct sockaddr_in), host, sizeof(host),
                                nullptr, 0, NI_NUMERICHOST) == 0 && std::string(host) != "127.0.0.1")
                    arr.push_back({{"name", p->ifa_name ? p->ifa_name : ""}, {"address", host}});
            }
            freeifaddrs(ifa);
        }
        send_json(res, arr);
    });

    // ---- static web UI at / ----
    if (!cfg.web_root.empty()) {
        server->set_mount_point("/", cfg.web_root);
    }
}

bool HttpServer::Impl::try_start_scan(std::vector<db::RootRow> roots,
                                      starbase::extract::HeaderMapping mapping) {
    std::lock_guard<std::mutex> lk(scan_mtx);
    if (scan_state.active) return false;
    if (scan_thread.joinable()) scan_thread.join();  // reap a finished run
    scan_state = ScanState{};
    scan_state.active = true;
    scan_state.total_roots = static_cast<int>(roots.size());
    scan_state.started = std::chrono::steady_clock::now();
    scan_cancel.store(false);
    auto dbc = cfg.db;
    scan_thread = std::thread([this, roots = std::move(roots), mapping = std::move(mapping),
                               dbc]() mutable {
        for (size_t i = 0; i < roots.size(); ++i) {
            if (scan_cancel.load()) break;
            { std::lock_guard<std::mutex> lk(scan_mtx);
              scan_state.root = roots[i].label; scan_state.done_roots = static_cast<int>(i);
              scan_state.seen = scan_state.added = scan_state.updated = scan_state.skipped =
                  scan_state.settling = scan_state.errored = scan_state.frames = scan_state.sidecars = 0; }
            starbase::scan::ScanConfig sc;
            sc.settle_seconds = roots[i].settle_seconds;
            sc.case_sensitive = roots[i].case_sensitive;
            sc.stop = &scan_cancel;
            sc.on_progress = [this](const starbase::scan::ScanProgress& p) {
                std::lock_guard<std::mutex> lk(scan_mtx);
                scan_state.seen = p.files_seen; scan_state.added = p.files_added;
                scan_state.updated = p.files_updated; scan_state.skipped = p.files_skipped;
                scan_state.settling = p.files_settling; scan_state.errored = p.files_error;
                scan_state.frames = p.frames_written; scan_state.sidecars = p.artifacts_recorded;
            };
            try {
                const auto st = starbase::scan::scan_root(dbc, roots[i], mapping, {}, sc);
                std::lock_guard<std::mutex> lk(scan_mtx);
                scan_state.results.push_back({{"root", roots[i].label}, {"added", st.files_added},
                    {"updated", st.files_updated}, {"unchanged", st.files_skipped},
                    {"frames", st.frames_written}, {"sidecars", st.artifacts_recorded},
                    {"error", st.files_error}, {"ms", st.duration_ms}});
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(scan_mtx);
                scan_state.results.push_back({{"root", roots[i].label}, {"failed", std::string(e.what())}});
            }
        }
        std::lock_guard<std::mutex> lk(scan_mtx);
        scan_state.active = false; scan_state.finished = true; scan_state.root = "";
        scan_state.done_roots = scan_state.total_roots;
    });
    return true;
}

void HttpServer::Impl::scheduler_loop() {
    // How often to look for a due root. 60s by default; SB_SCHEDULER_TICK_S tunes
    // it (mainly for tests and unusually short intervals).
    int tick = 60;
    if (const char* v = std::getenv("SB_SCHEDULER_TICK_S")) { const int t = std::atoi(v); if (t > 0) tick = t; }
    starbase::log_info("scan scheduler: on (checks every " + std::to_string(tick) +
                       "s, honouring each root's scan_interval_s)");
    while (!scheduler_stop.load()) {
        for (int i = 0; i < tick && !scheduler_stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (scheduler_stop.load()) break;
        { std::lock_guard<std::mutex> lk(scan_mtx); if (scan_state.active) continue; }
        try {
            auto d = db();
            // Enabled roots with a positive interval that are never-scanned or overdue.
            auto due = d.query(
                "SELECT label FROM roots WHERE enabled = 1 AND scan_interval_s > 0 "
                "AND (last_scan_end IS NULL OR "
                "     last_scan_end < UTC_TIMESTAMP() - INTERVAL scan_interval_s SECOND) "
                "ORDER BY (last_scan_end IS NULL) DESC, last_scan_end ASC");
            std::vector<db::RootRow> roots;
            for (const auto& r : due)
                if (r[0]) if (auto rr = d.find_root_by_label(*r[0])) roots.push_back(*rr);
            if (roots.empty()) continue;
            const size_t n = roots.size();
            if (try_start_scan(std::move(roots), starbase::index::load_mapping(d)))
                starbase::log_info("scan scheduler: rescanning " + std::to_string(n) + " due root(s)");
        } catch (const std::exception& e) {
            starbase::log_warn(std::string("scan scheduler: ") + e.what());
        }
    }
}

void HttpServer::Impl::ensure_schema() {
    if (cfg.schema_file.empty()) return;
    try {
        auto d = db();
        const bool fresh = (d.schema_version() == 0);
        // schema.sql is entirely CREATE ... IF NOT EXISTS / CREATE OR REPLACE
        // VIEW, so applying it every start builds a fresh database and adds any
        // tables introduced since the last release (the migration path).
        d.apply_script(cfg.schema_file);
        if (fresh) {
            starbase::log_info("initialized the StarBase database schema");
            // Seed the generic config (header mappings, value aliases, default
            // calibration rules) only on a brand-new database, so it is present
            // out of the box without clobbering later operator edits. Equipment
            // (rigs/sites/telescopes) is site-specific and is left to the
            // operator. seed.sql is INSERT IGNORE, hence safe.
            if (!cfg.seed_file.empty()) {
                try { d.apply_script(cfg.seed_file);
                      starbase::log_info("seeded header mappings and calibration rules"); }
                catch (const std::exception& e) {
                    starbase::log_warn(std::string("could not seed defaults (") + e.what() +
                                       "); run 'starbasectl db-seed' to load them");
                }
            }
        }
    } catch (const std::exception& e) {
        starbase::log_warn(std::string("could not ensure the database schema (") + e.what() +
                           "); run 'starbasectl db-init' if this is a fresh install");
    }
}

void HttpServer::Impl::ensure_default_admin() {
    try {
        auto d = db();
        auto r = d.query("SELECT COUNT(*) FROM users");
        if (!r.empty() && r[0][0] && std::stoll(*r[0][0]) > 0) return;
        const auto hp = starbase::auth::hash_password("admin");
        d.exec("INSERT INTO users (username, pwd_hash, pwd_salt, pwd_iterations, role, must_change_password) "
               "VALUES ('admin', UNHEX('" + hp.hash_hex + "'), UNHEX('" + hp.salt_hex + "'), " +
               std::to_string(hp.iterations) + ", 'admin', 1)");
        starbase::log_warn("seeded default admin user 'admin' / 'admin' - log in and change the "
                           "password immediately (Users tab or the account menu)");
    } catch (const std::exception& e) {
        starbase::log_warn(std::string("could not seed default admin: ") + e.what());
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
    impl_->ensure_schema();
    impl_->ensure_default_admin();
    if (!impl_->server->bind_to_port(bind.c_str(), port))
        throw std::runtime_error("cannot bind API to " + bind + ":" + std::to_string(port) +
                                 " (in use?)");
    impl_->thread = std::thread([this] { impl_->server->listen_after_bind(); });
    const std::string scheme = tls ? "https" : "http";
    starbase::log_info("API listening on " + scheme + "://" + bind + ":" + std::to_string(port) +
                       (impl_->cfg.web_root.empty() ? "" : " (web UI at /)"));

    // Interval scanner: rescan each enabled root on its scan_interval_s.
    impl_->scheduler_stop.store(false);
    if (impl_->cfg.scan_scheduler)
        impl_->scheduler_thread = std::thread([this] { impl_->scheduler_loop(); });
    else
        starbase::log_info("scan scheduler: off (scanning is manual)");
}

void HttpServer::stop() {
    // Stop the scheduler first so it cannot launch a new scan mid-teardown, then
    // abort a running scan (scan_cancel is atomic; the scanner polls it) and reap
    // its thread before tearing the server down.
    if (impl_) {
        impl_->scheduler_stop.store(true);
        if (impl_->scheduler_thread.joinable()) impl_->scheduler_thread.join();
        impl_->scan_cancel.store(true);
        if (impl_->scan_thread.joinable()) impl_->scan_thread.join();
    }
    if (impl_ && impl_->server && impl_->server->is_running()) impl_->server->stop();
    if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace starbase::api
