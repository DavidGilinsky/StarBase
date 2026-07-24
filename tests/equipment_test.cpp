// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/equipment_test.cpp
// Purpose:       Tests for equipment-id resolution: camera by alias, base model,
//                and auto-creation; filter by alias and auto-creation; rig match
//                by camera + focal length; site default; and no duplicate
//                auto-creation under concurrency.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// DB-gated like db_test: skips (exit 0) unless SB_TEST_DB_NAME is set.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "database.hpp"
#include "equipment.hpp"
#include "resolver.hpp"

namespace {

namespace db = starbase::db;
namespace ex = starbase::extract;
namespace idx = starbase::index;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
std::string find_sql(const std::string& n) {
    for (const std::string b : {"sql/", "../sql/", "../../sql/"})
        if (std::filesystem::exists(b + n)) return b + n;
    return {};
}
long long scalar(db::Database& d, const std::string& sql) {
    auto r = d.query(sql);
    return (r.empty() || !r[0][0]) ? -1 : std::stoll(*r[0][0]);
}
ex::ResolvedFrame frame(const std::string& instrume, std::optional<double> focal,
                        std::optional<std::string> filter) {
    ex::ResolvedFrame f;
    f.instrume_raw = instrume;
    f.focal_len_mm = focal;
    f.filter_raw = filter;
    return f;
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) { std::cout << "equipment_test: SB_TEST_DB_NAME not set; skipping\n"; return 0; }
    const std::string schema = find_sql("schema.sql"), seed = find_sql("seed.sql"),
                      equip = find_sql("equipment.example.sql");
    if (schema.empty() || seed.empty() || equip.empty()) { std::cerr << "no sql\n"; return 1; }

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        d.apply_script(seed);
        // Clear equipment for a repeatable run, then load the example registry.
        for (const char* t : {"frames", "files", "rigs", "camera_aliases", "filter_aliases",
                              "cameras", "filters", "telescopes", "sites"})
            d.exec(std::string("DELETE FROM ") + t);
        d.apply_script(equip);

        const auto reg = idx::EquipmentRegistry::load(d);
        check(reg.default_site_id.has_value(), "default site loaded");
        check(reg.default_site_offset_h == -7.0, "default site offset is -7 (Phoenix)");
        check(!reg.rigs.empty(), "rigs loaded");

        idx::EquipmentResolver r(reg);

        // NINA light: base model ASI6200, focal 1295 -> Askar185 rig, filter CLEAR.
        {
            auto ids = r.resolve(frame("ZWO ASI6200MC Pro", 1295.0, "CLEAR"), d);
            check(ids.camera_id.has_value(), "NINA camera resolved (base model)");
            check(ids.rig_id.has_value(), "rig matched by camera + focal length");
            check(ids.filter_id.has_value(), "CLEAR filter resolved");
            check(ids.site_id == reg.default_site_id, "site from rig / default");
            const int cam = *ids.camera_id;
            check(scalar(d, "SELECT id FROM cameras WHERE model='ASI6200'") == cam,
                  "camera is the canonical ASI6200");
        }
        // TheSky generic INSTRUME resolves via the alias to the same body.
        {
            auto ids = r.resolve(frame("ASICamera", 1295.0, "CLEAR"), d);
            check(ids.camera_id == scalar(d, "SELECT id FROM cameras WHERE model='ASI6200'"),
                  "ASICamera alias -> ASI6200");
        }
        // Focal length outside every rig range: camera yes, rig no.
        {
            auto ids = r.resolve(frame("ZWO ASI6200MC Pro", 600.0, "CLEAR"), d);
            check(ids.camera_id.has_value(), "camera still resolved off-rig");
            check(!ids.rig_id.has_value(), "no rig for an unmatched focal length");
        }
        // Filter alias: 'H-alpha' -> Ha.
        {
            auto ids = r.resolve(frame("ZWO ASI2600MC Air", 441.0, "H-alpha"), d);
            check(ids.filter_id == scalar(d, "SELECT id FROM filters WHERE name='Ha'"),
                  "H-alpha alias -> Ha");
            check(ids.rig_id.has_value(), "WO73A rig matched at 441mm");
        }
        // Unknown camera and unknown filter are auto-created.
        {
            const long long cams0 = scalar(d, "SELECT COUNT(*) FROM cameras");
            auto ids = r.resolve(frame("ZWO ASI9999XX Pro", 1000.0, "Tri-band"), d);
            check(ids.camera_id.has_value(), "unknown camera auto-created");
            check(scalar(d, "SELECT id FROM cameras WHERE model='ASI9999'") == *ids.camera_id,
                  "auto-created camera keyed on base model ASI9999");
            check(scalar(d, "SELECT COUNT(*) FROM cameras") == cams0 + 1, "exactly one camera added");
            check(scalar(d, "SELECT id FROM filters WHERE name='Tri-band'") == *ids.filter_id,
                  "unknown filter auto-created verbatim");
        }

        // ---- concurrency: many workers resolving the SAME new camera/filter
        //      must converge on one row each, never duplicate ----
        {
            const long long cams0 = scalar(d, "SELECT COUNT(*) FROM cameras");
            std::vector<std::thread> pool;
            for (int t = 0; t < 8; ++t) {
                pool.emplace_back([&] {
                    db::Database wd(cfg);
                    idx::EquipmentResolver wr(reg);
                    for (int i = 0; i < 20; ++i)
                        wr.resolve(frame("ZWO ASI0071 Race", 700.0, "RaceFilter"), wd);
                });
            }
            for (auto& t : pool) t.join();
            check(scalar(d, "SELECT COUNT(*) FROM cameras WHERE model='ASI0071'") == 1,
                  "concurrent auto-create yields exactly one camera row");
            check(scalar(d, "SELECT COUNT(*) FROM filters WHERE name='RaceFilter'") == 1,
                  "concurrent auto-create yields exactly one filter row");
            (void)cams0;
        }

    } catch (const std::exception& e) {
        std::cerr << "equipment_test: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) { std::cout << "equipment_test: all checks passed\n"; return 0; }
    std::cerr << "equipment_test: " << g_failures << " check(s) failed\n";
    return 1;
}
