// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/equipment.cpp
// Purpose:       Implementation of equipment-id resolution.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "equipment.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace starbase::index {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The canonical camera model to key calibration on: "ZWO ASI6200MC Pro" and the
// generic "ASICamera2"-style strings both reduce to "ASI6200". Empty if no ZWO
// model number is present, in which case an explicit alias is required.
std::string base_model(const std::string& instrume_raw) {
    static const std::regex re(R"(ASI\s?(\d{3,4}))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(instrume_raw, m, re)) return "ASI" + m[1].str();
    return {};
}

int read_id(db::Database& db, const std::string& sql) {
    auto rows = db.query(sql);
    if (rows.empty() || !rows[0][0]) return -1;
    return std::stoi(*rows[0][0]);
}

}  // namespace

EquipmentRegistry EquipmentRegistry::load(db::Database& db) {
    EquipmentRegistry r;

    for (const auto& row : db.query("SELECT id, model FROM cameras"))
        if (row[0] && row[1]) r.cameras_by_model[lower(*row[1])] = std::stoi(*row[0]);
    for (const auto& row : db.query(
             "SELECT pattern, camera_id FROM camera_aliases ORDER BY priority, id"))
        if (row[0] && row[1]) r.camera_aliases.push_back({*row[0], std::stoi(*row[1])});

    for (const auto& row : db.query("SELECT id, name FROM filters"))
        if (row[0] && row[1]) r.filters_by_name[lower(*row[1])] = std::stoi(*row[0]);
    for (const auto& row : db.query(
             "SELECT pattern, filter_id FROM filter_aliases ORDER BY priority, id"))
        if (row[0] && row[1]) r.filter_aliases.push_back({*row[0], std::stoi(*row[1])});

    for (const auto& row : db.query(
             "SELECT id, camera_id, focal_min_mm, focal_max_mm, site_id FROM rigs "
             "WHERE status = 'active'")) {
        if (!row[0] || !row[1] || !row[2] || !row[3]) continue;
        Rig rig;
        rig.id = std::stoi(*row[0]);
        rig.camera_id = std::stoi(*row[1]);
        rig.focal_min = std::stod(*row[2]);
        rig.focal_max = std::stod(*row[3]);
        if (row[4]) rig.site_id = std::stoi(*row[4]);
        r.rigs.push_back(rig);
    }

    // The default site drives the observing-night rollover offset. Prefer the
    // flagged default; fall back to the only site if there is exactly one.
    auto def = db.query(
        "SELECT id, utc_offset_h FROM sites ORDER BY is_default DESC, id LIMIT 1");
    if (!def.empty() && def[0][0]) {
        r.default_site_id = std::stoi(*def[0][0]);
        if (def[0][1]) r.default_site_offset_h = std::stod(*def[0][1]);
    }
    return r;
}

int EquipmentResolver::resolve_camera(const std::string& instrume_raw, db::Database& db) {
    const std::string key = lower(instrume_raw);
    if (auto it = camera_cache_.find(key); it != camera_cache_.end()) return it->second;

    int id = -1;
    // 1. Alias substring match ("ASICamera" -> ASI6200, etc.), priority order.
    for (const auto& a : reg_.camera_aliases) {
        if (key.find(lower(a.pattern)) != std::string::npos) { id = a.id; break; }
    }
    // 2. Canonical ZWO model, looked up or auto-created.
    if (id < 0) {
        const std::string model = base_model(instrume_raw);
        if (!model.empty()) {
            if (auto it = reg_.cameras_by_model.find(lower(model));
                it != reg_.cameras_by_model.end()) {
                id = it->second;
            } else {
                // Converge across workers on the unique model.
                db.exec("INSERT IGNORE INTO cameras (model, vendor) VALUES ('" +
                        db.escape(model) + "', 'ZWO')");
                id = read_id(db, "SELECT id FROM cameras WHERE model = '" +
                                     db.escape(model) + "' LIMIT 1");
            }
        }
    }
    camera_cache_[key] = id;
    return id;
}

int EquipmentResolver::resolve_filter(const std::string& filter_raw, db::Database& db) {
    if (filter_raw.empty()) return -1;
    const std::string key = lower(filter_raw);
    if (auto it = filter_cache_.find(key); it != filter_cache_.end()) return it->second;

    int id = -1;
    for (const auto& a : reg_.filter_aliases)       // 1. alias (exact, ci)
        if (key == lower(a.pattern)) { id = a.id; break; }
    if (id < 0) {                                    // 2. canonical name
        if (auto it = reg_.filters_by_name.find(key); it != reg_.filters_by_name.end())
            id = it->second;
    }
    if (id < 0) {                                    // 3. auto-create
        db.exec("INSERT IGNORE INTO filters (name) VALUES ('" + db.escape(filter_raw) + "')");
        id = read_id(db, "SELECT id FROM filters WHERE name = '" + db.escape(filter_raw) +
                             "' LIMIT 1");
    }
    filter_cache_[key] = id;
    return id;
}

EquipmentIds EquipmentResolver::resolve(const extract::ResolvedFrame& f, db::Database& db) {
    EquipmentIds ids;

    if (f.instrume_raw) {
        const int cam = resolve_camera(*f.instrume_raw, db);
        if (cam > 0) ids.camera_id = cam;
    }
    if (f.filter_raw) {
        const int filt = resolve_filter(*f.filter_raw, db);
        if (filt > 0) ids.filter_id = filt;
    }

    // A rig is a camera on an optical train: match the resolved camera and the
    // focal length against the operator's rig definitions. TELESCOP is never
    // consulted -- it is the mount as often as the optics.
    if (ids.camera_id && f.focal_len_mm) {
        for (const auto& rig : reg_.rigs) {
            if (rig.camera_id == *ids.camera_id && *f.focal_len_mm >= rig.focal_min &&
                *f.focal_len_mm <= rig.focal_max) {
                ids.rig_id = rig.id;
                if (rig.site_id) ids.site_id = rig.site_id;
                break;
            }
        }
    }
    // Site: the rig's if matched, otherwise the observatory default.
    if (!ids.site_id) ids.site_id = reg_.default_site_id;

    return ids;
}

}  // namespace starbase::index
