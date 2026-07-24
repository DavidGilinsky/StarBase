// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/scan/scanner.cpp
// Purpose:       Implementation of the threaded directory sweep.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "scanner.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include "bounded_queue.hpp"
#include "equipment.hpp"
#include "fits_reader.hpp"
#include "frame_store.hpp"
#include "logging.hpp"

namespace starbase::scan {
namespace {

namespace stdfs = std::filesystem;
namespace idx = starbase::index;
using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Exact match, or a trailing-'*' treated as a prefix. Case-insensitive, so a
// Windows client's THUMBS.DB is caught alongside Thumbs.db.
bool matches_ignore(const std::string& name, const std::vector<std::string>& globs) {
    const std::string n = lower(name);
    for (const auto& g : globs) {
        const std::string lg = lower(g);
        if (!lg.empty() && lg.back() == '*') {
            if (n.compare(0, lg.size() - 1, lg, 0, lg.size() - 1) == 0) return true;
        } else if (n == lg) {
            return true;
        }
    }
    return false;
}

std::string format_for_ext(const std::string& ext) {
    const std::string e = lower(ext);
    if (e == ".fits" || e == ".fit" || e == ".fts" || e == ".fz") return "fits";
    if (e == ".xisf") return "xisf";
    return "other";
}

bool is_indexable(const std::string& ext) { return format_for_ext(ext) != "other"; }

// First path component under the root, e.g. "lights", "calibration".
std::string bucket_for(const std::string& rel_path) {
    const auto slash = rel_path.find('/');
    const std::string top = slash == std::string::npos ? rel_path : rel_path.substr(0, slash);
    static const char* known[] = {"lights",     "calibration", "process",
                                  "review",     "quarantine",  "incoming"};
    const std::string t = lower(top);
    for (const char* k : known)
        if (t == k) return k;
    // 'callibration' (the archive's misspelled sibling) and anything else.
    if (t.rfind("calib", 0) == 0) return "calibration";
    return "other";
}

std::string utc_from_epoch(std::time_t t, long nsec) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    const long usec = (nsec / 1000) % 1000000;  // bound so the format cannot overflow
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, usec);
    return buf;
}

struct WorkItem {
    std::string abs_path;
    std::string rel_path;
    std::string filename;
    std::string ext;
};

}  // namespace

ScanStats scan_root(const db::DbConfig& db_config, const db::RootRow& root,
                    const extract::HeaderMapping& mapping,
                    const extract::SiteContext& site, const ScanConfig& cfg) {
    const auto t0 = Clock::now();
    ScanStats stats;

    int nthreads = cfg.threads;
    if (nthreads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        nthreads = static_cast<int>(std::min(hw ? hw : 4u, 16u));
    }

    BoundedQueue<WorkItem> queue(static_cast<std::size_t>(cfg.queue_depth));
    std::atomic<long> seen{0}, added{0}, updated{0}, skipped{0}, settling{0}, errored{0},
        frames{0};
    const std::string root_path = root.path;
    const long settle = cfg.settle_seconds;
    auto stop_requested = [&] { return cfg.stop && cfg.stop->load(); };

    // Load the equipment registry once and share it read-only. Its default site
    // supplies the observing-night offset: without this the night would roll at
    // UTC noon instead of the observatory's local noon, mis-labelling frames
    // taken in the evening either side of midnight UTC.
    idx::EquipmentRegistry registry;
    try {
        db::Database rdb(db_config);
        registry = idx::EquipmentRegistry::load(rdb);
    } catch (const std::exception& e) {
        log_warn(std::string("scan: could not load equipment registry (")
                 + e.what() + "); ids and night offset unresolved");
    }
    extract::SiteContext night_site = site;
    if (site.utc_offset_hours == 0.0 && registry.default_site_id)
        night_site.utc_offset_hours = registry.default_site_offset_h;

    // ---- workers ----
    auto worker = [&] {
        db::Database db(db_config);  // one connection per worker
        // READ COMMITTED, not InnoDB's default REPEATABLE READ: the store path is
        // many concurrent INSERT ... ON DUPLICATE KEY UPDATE, and REPEATABLE
        // READ's gap/next-key locks turn those into frequent insert-intention
        // deadlocks. READ COMMITTED takes far fewer gap locks and is correct here
        // (each statement reads the latest committed rows; the readback of our
        // own insert is visible within the transaction regardless).
        try { db.exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED"); }
        catch (const std::exception&) { /* fall back to the server default */ }
        idx::EquipmentResolver equip(registry);  // per-worker, caches camera/filter lookups
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        for (;;) {
            auto item = queue.pop();
            if (!item) break;
            if (stop_requested()) continue;  // drain quickly on shutdown

            struct stat st{};
            if (::stat(item->abs_path.c_str(), &st) != 0) continue;  // vanished mid-scan
            seen.fetch_add(1);

            // Settle gate: skip a file still likely being written.
            if (now - st.st_mtime < settle) { settling.fetch_add(1); continue; }

            const std::string mtime = utc_from_epoch(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);

            // Cheap change detection: an unchanged file (size + mtime) is not
            // reparsed, which is what makes a re-sweep of a large archive nearly
            // free. Only touch last_seen so 'missing' handling stays correct.
            const std::string hash_path =
                cfg.case_sensitive ? item->rel_path : lower(item->rel_path);
            const std::string hash_sql = "UNHEX(MD5('" + db.escape(hash_path) + "'))";
            try {
                auto rows = db.query(
                    "SELECT id, size_bytes, mtime_utc, status FROM files WHERE root_id = " +
                    std::to_string(root.id) + " AND rel_path_hash = " + hash_sql + " LIMIT 1");
                const bool known = !rows.empty();
                // Skip only an unchanged file that indexed cleanly before. An
                // error row is re-read every pass (it may have been fixed), and
                // a size/mtime change always re-reads.
                const bool unchanged =
                    known && rows[0][1] && rows[0][2] && rows[0][3] &&
                    *rows[0][3] == "ok" &&
                    std::stoll(*rows[0][1]) == static_cast<long long>(st.st_size) &&
                    rows[0][2]->substr(0, 19) == mtime.substr(0, 19);
                if (unchanged) {
                    db.exec("UPDATE files SET last_seen_utc = UTC_TIMESTAMP(), status='ok' "
                            "WHERE id = " + std::string(*rows[0][0]));
                    skipped.fetch_add(1);
                    continue;
                }

                // Changed or new: read, resolve, store.
                auto header = fits::read_header(item->abs_path);
                idx::FileInfo info;
                info.root_id = root.id;
                info.rel_path = item->rel_path;
                info.filename = item->filename;
                info.ext = item->ext;
                info.format = format_for_ext(item->ext);
                info.bucket = bucket_for(item->rel_path);
                info.size_bytes = static_cast<long long>(st.st_size);
                info.mtime_utc = mtime;
                info.inode = static_cast<long long>(st.st_ino);
                info.case_sensitive = cfg.case_sensitive;

                const auto sr = idx::store_file(db, info, header, mapping, night_site, &equip);
                frames.fetch_add(sr.frames_written);
                (known ? updated : added).fetch_add(1);
            } catch (const std::exception& e) {
                // A bad file is a row, not a silence: record it and move on.
                errored.fetch_add(1);
                try {
                    db.exec(
                        "INSERT INTO files (root_id, rel_path, rel_path_hash, filename, ext, "
                        "format, bucket, size_bytes, mtime_utc, status, error, last_seen_utc) "
                        "VALUES (" + std::to_string(root.id) + ", '" + db.escape(item->rel_path) +
                        "', " + hash_sql + ", '" + db.escape(item->filename) + "', '" +
                        db.escape(item->ext) + "', '" + db.escape(format_for_ext(item->ext)) +
                        "', '" + db.escape(bucket_for(item->rel_path)) + "', " +
                        std::to_string(static_cast<long long>(st.st_size)) + ", '" + mtime +
                        "', 'error', '" + db.escape(std::string(e.what()).substr(0, 500)) +
                        "', UTC_TIMESTAMP()) ON DUPLICATE KEY UPDATE status='error', "
                        "error=VALUES(error), last_seen_utc=UTC_TIMESTAMP()");
                } catch (const std::exception&) { /* best effort */ }
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    for (int i = 0; i < nthreads; ++i) pool.emplace_back(worker);

    // ---- producer: walk the tree, feed the queue ----
    std::error_code ec;
    stdfs::recursive_directory_iterator it(
        root_path, stdfs::directory_options::skip_permission_denied, ec);
    if (ec) {
        queue.close();
        for (auto& t : pool) t.join();
        throw std::runtime_error("cannot walk root '" + root.label + "' at " + root_path +
                                 ": " + ec.message());
    }
    for (stdfs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (stop_requested()) break;

        const std::string name = it->path().filename().string();
        if (matches_ignore(name, cfg.ignore_globs)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();  // prune the subtree
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (!is_indexable(ext)) continue;

        WorkItem w;
        w.abs_path = it->path().string();
        w.rel_path = stdfs::relative(it->path(), root_path, ec).generic_string();
        if (ec) { ec.clear(); w.rel_path = name; }
        w.filename = name;
        w.ext = ext;
        queue.push(std::move(w));
    }
    queue.close();
    for (auto& t : pool) t.join();

    stats.files_seen = seen.load();
    stats.files_added = added.load();
    stats.files_updated = updated.load();
    stats.files_skipped = skipped.load();
    stats.files_settling = settling.load();
    stats.files_error = errored.load();
    stats.frames_written = frames.load();
    stats.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    return stats;
}

}  // namespace starbase::scan
