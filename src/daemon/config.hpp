// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/config.hpp
// Purpose:       Declarations for the daemon's INI-style configuration model
//                and its loader.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase {

// Parsed daemon configuration (simple INI-style file).
//
// Monitored roots are NOT configured here. They live in the `roots` table and
// are managed with `starbasectl add-root` or the web UI, so a root can be added,
// disabled, or rescanned without restarting the daemon or editing a file.
//
// Recognised sections:
//   [daemon]       log_level
//   [database]     host / port / name / user
//   [api]          bind / port / tls / tls_cert / tls_key / schema_file /
//                  seed_file / web_root
//   [scanner]      threads / queue_depth / default_scan_interval_s /
//                  default_settle_seconds
//   [actions]      staging_root / trash_root / link_mode
//   [pixinsight]   binary / wbpp_script
struct Config {
    // [daemon]
    std::string log_level = "info";

    // [database] -- the password is never stored here; it comes from
    // SB_DB_PASSWORD, supplied by a root-owned systemd EnvironmentFile.
    std::string db_host = "127.0.0.1";
    int         db_port = 3306;
    std::string db_name = "starbase";
    std::string db_user = "starbase";

    // [api]
    std::string api_bind = "127.0.0.1";
    int         api_port = 8642;  // distinct from NightWatcher2 (8080) and AirWatcher (8686)
    bool        api_tls = false;
    std::string api_tls_cert;
    std::string api_tls_key;
    std::string schema_file;   // for POST /api/v1/db/init
    std::string seed_file;     // default header mapping + calibration rules
    std::string web_root;      // static web UI directory

    // [scanner]
    // 0 means "decide at runtime" (hardware_concurrency, clamped). The archive
    // is on NFS, where the useful parallelism is bounded by RPC concurrency
    // rather than by CPU, so this is deliberately tunable.
    int scanner_threads = 0;
    int scanner_queue_depth = 4096;          // bounded: keeps memory flat on huge trees
    int default_scan_interval_s = 3600;
    int default_settle_seconds = 30;         // ignore files younger than this
    // When on, the daemon rescans each enabled root on its own scan_interval_s.
    // Off leaves scanning fully manual (UI button / starbasectl scan). A root
    // with scan_interval_s <= 0 is never auto-scanned regardless.
    bool scan_scheduler = true;

    // Applied to a new root by `add-root`, then editable per root. Covers
    // partial writes and the debris left by the other things that touch an
    // archive: Windows/SMB clients (Thumbs.db, desktop.ini, System Volume
    // Information, $RECYCLE.BIN), macOS (.DS_Store, ._*), and GoodSync
    // (_gsdata_). Newline or comma separated.
    std::string default_ignore_globs =
        "_gsdata_,*.tmp,*.part,*.partial,Thumbs.db,desktop.ini,"
        "System Volume Information,$RECYCLE.BIN,.DS_Store,._*,.zfs";

    // [actions]
    std::string staging_root = "/var/lib/starbase/staging";
    std::string trash_root = "/var/lib/starbase/trash";
    std::string link_mode = "symlink";       // symlink | hardlink | copy

    // [names] optional online target-name resolution (CDS Sesame). Off by
    // default: it is the only feature that reaches the network.
    bool sesame_enabled = false;
    std::string sesame_url = "https://cds.unistra.fr";

    // [pixinsight]
    std::string pixinsight_binary = "/opt/PixInsight/bin/PixInsight.sh";
    std::string wbpp_script =
        "/opt/PixInsight/src/scripts/BatchPreprocessing/WBPP.js";

    // Load and parse the file at `path`.
    // Throws std::runtime_error on I/O failure or a malformed line.
    static Config load(const std::string& path);
};

}  // namespace starbase
