// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/action/action.hpp
// Purpose:       The action engine: stage a set of frames into a WBPP-ready
//                tree, run filesystem operations (copy/symlink/move/trash) with
//                a dry-run and a trash directory instead of unlink, and export a
//                set. Every action is an audited job with per-item results.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database.hpp"

namespace starbase::action {

struct ActionConfig {
    std::string staging_root = "/var/lib/starbase/staging";
    std::string trash_root = "/var/lib/starbase/trash";
    std::string link_mode = "symlink";  // symlink | hardlink | copy (staging default)
};

// One frame the action operates on, resolved from the database.
struct FrameRef {
    long long   frame_id = 0;
    long long   file_id = 0;
    int         root_id = 0;
    std::string abs_path;
    std::string rel_path;
    std::string filename;
    std::string image_type;   // light | dark | flat | bias | darkflat | master | unknown
};

// Resolve a set of frames from explicit ids or a filter AST (JSON string; empty
// or "{}" matches nothing here -- an action must be given an explicit set).
std::vector<FrameRef> resolve_by_ids(db::Database& db, const std::vector<long long>& ids);
std::vector<FrameRef> resolve_by_filter(db::Database& db, const std::string& filter_json,
                                        long limit = 100000);

struct ItemResult {
    long long   frame_id = 0;
    std::string action;   // symlink | hardlink | copy | move | trash | export
    std::string src;
    std::string dst;
    std::string status;   // ok | skipped | failed
    std::string detail;
};

struct JobResult {
    long long   job_id = 0;   // 0 for a dry run (nothing recorded)
    std::string type;
    bool        dry_run = false;
    int         total = 0, done = 0, failed = 0, skipped = 0;
    std::string root;         // e.g. the staging directory created
    std::vector<ItemResult> items;
};

// Build a staging tree: staging_root/job-<id>/<image_type>/<name>, one entry per
// frame via symlink, hardlink, or copy. Non-destructive. Names collide-proofed
// by appending the frame id. This is the substrate the WBPP handoff consumes.
JobResult stage(db::Database& db, const ActionConfig& cfg, const std::vector<FrameRef>& frames);

// Filesystem operation over a set. `op` is copy | symlink | move | trash.
//   - copy/symlink target a directory (non-destructive).
//   - move relocates into a target directory; trash relocates into trash_root.
//   - move/trash are destructive and default to a dry run: with dry_run the
//     result lists exactly what WOULD happen and touches nothing.
// After a real move/trash, the index is written back: a file relocated under a
// known root has its files row updated; otherwise it is marked missing. A trash
// never calls unlink(2).
JobResult fsop(db::Database& db, const ActionConfig& cfg, const std::string& op,
               const std::vector<FrameRef>& frames, const std::string& target_dir,
               bool dry_run);

// Export a set as "paths" (newline-delimited), "csv", or "json". Returns the
// content; the caller decides where it goes.
std::string export_frames(db::Database& db, const std::vector<FrameRef>& frames,
                          const std::string& format);

}  // namespace starbase::action
