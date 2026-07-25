// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/action/action.cpp
// Purpose:       Implementation of the action engine.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "action.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "query.hpp"

namespace starbase::action {
namespace {

namespace stdfs = std::filesystem;
using json = nlohmann::json;

std::optional<std::string> cell(const db::Database::Row& r, size_t i) {
    return (i < r.size() && r[i]) ? r[i] : std::optional<std::string>{};
}

const char* kFrameSelect =
    "SELECT f.id, f.file_id, fl.root_id, CONCAT(r.path,'/',fl.rel_path), fl.rel_path, "
    "fl.filename, f.image_type FROM frames f JOIN files fl ON fl.id = f.file_id "
    "JOIN roots r ON r.id = fl.root_id ";

std::vector<FrameRef> rows_to_refs(const std::vector<db::Database::Row>& rows) {
    std::vector<FrameRef> out;
    for (const auto& r : rows) {
        FrameRef f;
        f.frame_id = std::stoll(cell(r, 0).value_or("0"));
        f.file_id = std::stoll(cell(r, 1).value_or("0"));
        f.root_id = std::stoi(cell(r, 2).value_or("0"));
        f.abs_path = cell(r, 3).value_or("");
        f.rel_path = cell(r, 4).value_or("");
        f.filename = cell(r, 5).value_or("");
        f.image_type = cell(r, 6).value_or("unknown");
        out.push_back(std::move(f));
    }
    return out;
}

// A filesystem-safe leaf name: keep it, but never let it escape its directory.
std::string safe_name(const std::string& name) {
    std::string s = name;
    for (char& c : s) if (c == '/' || c == '\0') c = '_';
    if (s == "." || s == ".." || s.empty()) s = "_";
    return s;
}

// Insert "_<n>" before the extension.
std::string with_suffix(const std::string& name, long long n) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return name + "_" + std::to_string(n);
    return name.substr(0, dot) + "_" + std::to_string(n) + name.substr(dot);
}

// The registered root whose path is a path-component prefix of `path`, longest
// first, so a moved file can be re-anchored in the index. Returns {root_id,
// root_path} or {0, ""}.
std::pair<int, std::string> root_for(db::Database& db, const std::string& path) {
    auto rows = db.query("SELECT id, path FROM roots ORDER BY LENGTH(path) DESC");
    for (const auto& r : rows) {
        const std::string rp = cell(r, 1).value_or("");
        if (rp.empty()) continue;
        if (path.compare(0, rp.size(), rp) == 0 &&
            (path.size() == rp.size() || path[rp.size()] == '/'))
            return {std::stoi(*r[0]), rp};
    }
    return {0, ""};
}

long long create_job(db::Database& db, const std::string& type, bool dry_run, int total) {
    return db.exec("INSERT INTO jobs (type, status, dry_run, total_items) VALUES ('" +
                   db.escape(type) + "', 'running', " + (dry_run ? "1" : "0") + ", " +
                   std::to_string(total) + ")");
}

void record_item(db::Database& db, long long job_id, const ItemResult& it) {
    db.exec("INSERT INTO job_items (job_id, frame_id, action, src_path, dst_path, status, detail) "
            "VALUES (" + std::to_string(job_id) + ", " + std::to_string(it.frame_id) + ", '" +
            db.escape(it.action) + "', '" + db.escape(it.src) + "', '" + db.escape(it.dst) +
            "', '" + db.escape(it.status) + "', " +
            (it.detail.empty() ? "NULL" : "'" + db.escape(it.detail) + "'") + ")");
}

void finish_job(db::Database& db, long long job_id, const JobResult& jr) {
    db.exec("UPDATE jobs SET status = '" + std::string(jr.failed ? "failed" : "done") +
            "', done_items = " + std::to_string(jr.done) + ", failed_items = " +
            std::to_string(jr.failed) + ", finished_at = UTC_TIMESTAMP() WHERE id = " +
            std::to_string(job_id));
}

}  // namespace

std::vector<FrameRef> resolve_by_ids(db::Database& db, const std::vector<long long>& ids) {
    if (ids.empty()) return {};
    std::string list;
    for (size_t i = 0; i < ids.size(); ++i) list += (i ? "," : "") + std::to_string(ids[i]);
    return rows_to_refs(db.query(std::string(kFrameSelect) + "WHERE f.id IN (" + list + ")"));
}

std::vector<FrameRef> resolve_by_filter(db::Database& db, const std::string& filter_json,
                                        long limit) {
    json ast = filter_json.empty() ? json::object() : json::parse(filter_json);
    // Reuse the query compiler, but it targets v_frames; re-select the join
    // columns for the ids it returns.
    const std::string where = starbase::query::compile_filter(ast, db);
    auto ids = db.query("SELECT frame_id FROM v_frames WHERE " + where +
                        " ORDER BY date_obs_utc DESC LIMIT " + std::to_string(limit));
    std::vector<long long> id_list;
    for (const auto& r : ids) if (r[0]) id_list.push_back(std::stoll(*r[0]));
    return resolve_by_ids(db, id_list);
}

JobResult stage(db::Database& db, const ActionConfig& cfg, const std::vector<FrameRef>& frames) {
    JobResult jr;
    jr.type = "stage";
    jr.total = static_cast<int>(frames.size());
    const long long job_id = create_job(db, "stage", false, jr.total);
    jr.job_id = job_id;

    const stdfs::path base = stdfs::path(cfg.staging_root) / ("job-" + std::to_string(job_id));
    jr.root = base.string();
    std::error_code ec;

    for (const auto& f : frames) {
        ItemResult it;
        it.frame_id = f.frame_id;
        it.src = f.abs_path;
        const stdfs::path dir = base / (f.image_type.empty() ? "unknown" : f.image_type);
        stdfs::create_directories(dir, ec);
        stdfs::path dst = dir / safe_name(f.filename);
        if (stdfs::exists(dst, ec)) dst = dir / safe_name(with_suffix(f.filename, f.frame_id));
        it.dst = dst.string();

        it.action = cfg.link_mode;
        ec.clear();
        if (cfg.link_mode == "hardlink") stdfs::create_hard_link(f.abs_path, dst, ec);
        else if (cfg.link_mode == "copy") stdfs::copy_file(f.abs_path, dst, ec);
        else { it.action = "symlink"; stdfs::create_symlink(f.abs_path, dst, ec); }

        if (ec) { it.status = "failed"; it.detail = ec.message(); ++jr.failed; }
        else { it.status = "ok"; ++jr.done; }
        record_item(db, job_id, it);
        jr.items.push_back(std::move(it));
    }
    finish_job(db, job_id, jr);
    return jr;
}

JobResult fsop(db::Database& db, const ActionConfig& cfg, const std::string& op,
               const std::vector<FrameRef>& frames, const std::string& target_dir, bool dry_run) {
    JobResult jr;
    jr.type = op;
    jr.dry_run = dry_run;
    jr.total = static_cast<int>(frames.size());

    if (op != "copy" && op != "symlink" && op != "move" && op != "trash")
        throw std::runtime_error("unknown fsop '" + op + "'");
    const bool destructive = (op == "move" || op == "trash");
    // The destination directory: explicit for copy/symlink/move; the trash root
    // (with a per-job subdir) for trash. All four are jobs.type='fsop'; the
    // specific op is recorded per item.
    const long long job_id = dry_run ? 0 : create_job(db, "fsop", dry_run, jr.total);
    jr.job_id = job_id;
    stdfs::path dest_base = (op == "trash")
        ? stdfs::path(cfg.trash_root) / ("job-" + std::to_string(job_id ? job_id : 0))
        : stdfs::path(target_dir);
    jr.root = dest_base.string();
    std::error_code ec;

    for (const auto& f : frames) {
        ItemResult it;
        it.frame_id = f.frame_id;
        it.action = op;
        it.src = f.abs_path;
        stdfs::path dst = dest_base / safe_name(f.filename);
        it.dst = dst.string();

        if (dry_run) {
            it.status = "ok";
            it.detail = "would " + op;
            ++jr.done;
            jr.items.push_back(std::move(it));
            continue;
        }

        stdfs::create_directories(dest_base, ec);
        ec.clear();
        bool ok = false;
        if (op == "copy") { stdfs::copy_file(f.abs_path, dst, ec); ok = !ec; }
        else if (op == "symlink") { stdfs::create_symlink(f.abs_path, dst, ec); ok = !ec; }
        else {  // move or trash: rename, falling back to copy+remove across devices
            stdfs::rename(f.abs_path, dst, ec);
            if (ec) {
                ec.clear();
                if (stdfs::copy_file(f.abs_path, dst, ec) && !ec) {
                    std::error_code rmec; stdfs::remove(f.abs_path, rmec); ok = !rmec;
                }
            } else ok = true;
        }

        if (!ok) {
            it.status = "failed";
            it.detail = ec.message();
            ++jr.failed;
        } else {
            it.status = "ok";
            ++jr.done;
            if (destructive) {
                // Write the move back into the index rather than waiting for a
                // sweep. If the destination is under a known root, re-anchor the
                // files row; otherwise the file has left the index -> missing.
                auto [rid, rpath] = root_for(db, dst.string());
                if (rid) {
                    std::string newrel = dst.string().substr(rpath.size());
                    if (!newrel.empty() && newrel.front() == '/') newrel.erase(0, 1);
                    db.exec("UPDATE files SET root_id = " + std::to_string(rid) +
                            ", rel_path = '" + db.escape(newrel) + "', rel_path_hash = UNHEX(MD5('" +
                            db.escape(newrel) + "')), status = 'ok', last_seen_utc = UTC_TIMESTAMP() "
                            "WHERE id = " + std::to_string(f.file_id));
                } else {
                    db.exec("UPDATE files SET status = 'missing' WHERE id = " +
                            std::to_string(f.file_id));
                }
            }
        }
        record_item(db, job_id, it);
        jr.items.push_back(std::move(it));
    }
    if (job_id) finish_job(db, job_id, jr);
    return jr;
}

std::string export_frames(db::Database& db, const std::vector<FrameRef>& frames,
                          const std::string& format) {
    (void)db;
    if (format == "paths") {
        std::string out;
        for (const auto& f : frames) out += f.abs_path + "\n";
        return out;
    }
    if (format == "json") {
        json arr = json::array();
        for (const auto& f : frames)
            arr.push_back({{"frame_id", f.frame_id}, {"image_type", f.image_type},
                           {"filename", f.filename}, {"path", f.abs_path}});
        return arr.dump(2);
    }
    // csv (default)
    std::ostringstream o;
    o << "frame_id,image_type,filename,path\n";
    auto q = [](const std::string& s) {
        return (s.find(',') != std::string::npos || s.find('"') != std::string::npos)
                   ? "\"" + std::string([&] { std::string t; for (char c : s) { if (c=='"') t+='"'; t+=c; } return t; }()) + "\""
                   : s;
    };
    for (const auto& f : frames)
        o << f.frame_id << "," << f.image_type << "," << q(f.filename) << "," << q(f.abs_path) << "\n";
    return o.str();
}

}  // namespace starbase::action
