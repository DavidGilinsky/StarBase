// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/equipment.hpp
// Purpose:       Resolve a frame's raw equipment strings to registry ids:
//                camera_id and filter_id (auto-created on first sighting so a
//                scan is zero-config), rig_id and site_id (matched against
//                operator-defined rigs and sites).
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "database.hpp"
#include "resolver.hpp"

namespace starbase::index {

struct EquipmentIds {
    std::optional<int> camera_id;
    std::optional<int> rig_id;
    std::optional<int> filter_id;
    std::optional<int> site_id;
};

// An in-memory snapshot of the equipment registry, loaded once per scan. The
// database remains the source of truth for auto-created rows (workers converge
// via INSERT IGNORE + read-back), so a stale snapshot only ever means an extra
// round trip, never a wrong or duplicated id.
struct EquipmentRegistry {
    struct Alias { std::string pattern; int id; };  // in priority order
    struct Rig { int id; int camera_id; double focal_min, focal_max; std::optional<int> site_id; };

    std::unordered_map<std::string, int> cameras_by_model;  // key: lowercased model
    std::vector<Alias> camera_aliases;                      // substring match
    std::unordered_map<std::string, int> filters_by_name;   // key: lowercased name
    std::vector<Alias> filter_aliases;                      // exact (ci) match
    std::vector<Rig> rigs;
    std::optional<int> default_site_id;
    double default_site_offset_h = 0.0;

    static EquipmentRegistry load(db::Database& db);
};

// Resolves ids for successive frames, one instance per worker: it caches
// camera/filter lookups by raw string, so the common case (the same camera and
// filter across thousands of frames) costs one database round trip, not one per
// frame. Not thread-safe; give each thread its own, sharing the const registry.
class EquipmentResolver {
public:
    explicit EquipmentResolver(const EquipmentRegistry& reg) : reg_(reg) {}

    // Resolve every id for one resolved frame, auto-creating a camera or filter
    // that has not been seen before. `db` is the caller's own connection.
    EquipmentIds resolve(const extract::ResolvedFrame& f, db::Database& db);

private:
    int resolve_camera(const std::string& instrume_raw, db::Database& db);   // -1 if none
    int resolve_filter(const std::string& filter_raw, db::Database& db);     // -1 if none

    const EquipmentRegistry& reg_;
    std::unordered_map<std::string, int> camera_cache_;
    std::unordered_map<std::string, int> filter_cache_;
};

}  // namespace starbase::index
