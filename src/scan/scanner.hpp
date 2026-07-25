// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/scan/scanner.hpp
// Purpose:       Threaded directory sweep: one producer walks a root and feeds a
//                bounded queue; N workers stat, read, resolve, and store each
//                frame, each with its own database connection. The sweep is
//                authoritative -- correct on its own, with inotify only ever an
//                accelerator on top.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "database.hpp"
#include "resolver.hpp"

namespace starbase::scan {

struct ScanConfig {
    // 0 = auto = min(hardware_concurrency, 16). The benchmark (docs/ARCHITECTURE
    // section 4) shows header reads are NFS-latency-bound and scale to ~16
    // threads, with little gained past that and rising RPC pressure on the
    // server, so auto clamps there.
    int threads = 0;
    int queue_depth = 4096;
    // A file whose mtime is younger than this is skipped this pass: it may still
    // be mid-write, especially over SMB where the server cannot tell an NFS
    // reader the file is in flight.
    int settle_seconds = 30;
    // Names to skip (exact match or trailing-'*' prefix), covering partial
    // writes and the debris of Windows/SMB, macOS, and GoodSync writers.
    std::vector<std::string> ignore_globs;
    bool case_sensitive = true;
    // A running scan aborts promptly when this is set (SIGTERM during a sweep).
    std::atomic<bool>* stop = nullptr;
};

struct ScanStats {
    long files_seen = 0;       // FITS/XISF files the walker offered
    long files_added = 0;      // newly indexed
    long files_updated = 0;    // changed since last index
    long files_skipped = 0;    // unchanged (size+mtime match)
    long files_settling = 0;   // too young; deferred to next pass
    long files_error = 0;      // unreadable/parse failure (recorded as status=error)
    long frames_written = 0;
    long artifacts_recorded = 0;  // sidecars/logs indexed into the artifacts table
    long long duration_ms = 0;
};

// Sweep one root and index it into the database. Opens a fresh connection per
// worker (Database is not thread-safe). The header mapping and site come from
// the caller (loaded once from the DB). Does not throw for per-file failures --
// those become files_error and a files.status='error' row -- only for a failure
// that dooms the whole scan (e.g. the root path is unreadable).
ScanStats scan_root(const db::DbConfig& db_config, const db::RootRow& root,
                    const extract::HeaderMapping& mapping,
                    const extract::SiteContext& site, const ScanConfig& cfg);

}  // namespace starbase::scan
