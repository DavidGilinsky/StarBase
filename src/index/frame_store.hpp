// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/frame_store.hpp
// Purpose:       Persist an indexed file and its frames: upsert the files row,
//                and for each image HDU upsert a frames row with the resolved
//                fields and identity fingerprint, plus its full header verbatim
//                in frame_keywords.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "database.hpp"
#include "fits_reader.hpp"
#include "resolver.hpp"

namespace starbase::index {

// What the scanner knows about a file from stat() and its path, independent of
// its contents. rel_path is relative to the root's path.
struct FileInfo {
    int         root_id = 0;
    std::string rel_path;
    std::string filename;
    std::string ext;               // ".fits", ".xisf", ...
    std::string format = "fits";   // fits | xisf | other
    std::string bucket = "other";  // lights | calibration | process | ...
    std::optional<long long>   size_bytes;
    std::optional<std::string> mtime_utc;   // "YYYY-MM-DD HH:MM:SS[.ffffff]"
    std::optional<long long>   inode;
    // The root's case sensitivity, so rel_path_hash folds case when the
    // filesystem does. A miss here would let one file become two rows.
    bool case_sensitive = true;
};

// Result of storing one file.
struct StoreResult {
    long long file_id = 0;
    int frames_written = 0;
    int keywords_written = 0;
};

// Index one file end to end: upsert files, then for each image HDU resolve the
// header, compute the fingerprint, upsert frames (keyed on file_id+hdu), and
// replace its frame_keywords with the current header. Idempotent: re-indexing
// the same file updates in place rather than duplicating. Runs as a single
// transaction so a file is never left half-written. Throws db::DbError on
// failure.
//
// Equipment ids (camera_id, rig_id, filter_id) are left null here; resolving
// them against the registry is a separate step. The raw values they key on
// (instrume_raw, focal_len_mm, filter_raw) are stored.
StoreResult store_file(db::Database& db, const FileInfo& info,
                       const fits::RawHeader& header,
                       const extract::HeaderMapping& mapping,
                       const extract::SiteContext& site = {});

}  // namespace starbase::index
