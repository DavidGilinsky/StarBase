// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/match_test.cpp
// Purpose:       Tests for the calibration matcher: field matching and
//                tolerances, master preference, same-session preference for
//                flats, age limits, exclusions, and the human-readable reason.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// DB-gated like db_test: skips (exit 0) unless SB_TEST_DB_NAME is set.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "calibration.hpp"
#include "database.hpp"

namespace {

namespace db = starbase::db;
namespace m = starbase::match;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
std::string find_sql(const std::string& n) {
    for (const std::string b : {"sql/", "../sql/", "../../sql/"})
        if (std::filesystem::exists(b + n)) return b + n;
    return {};
}

int g_seq = 0;
// Insert one file + one frame with the given column values; returns frame id.
long long add_frame(db::Database& d, int root_id, std::map<std::string, std::string> cols) {
    const std::string rel = "f" + std::to_string(++g_seq) + ".fits";
    d.exec("INSERT INTO files (root_id, rel_path, rel_path_hash, filename, format, bucket, status) "
           "VALUES (" + std::to_string(root_id) + ", '" + rel + "', UNHEX(MD5('" + rel + "')), '" +
           rel + "', 'fits', 'other', 'ok')");
    auto fr = d.query("SELECT id FROM files WHERE root_id=" + std::to_string(root_id) +
                      " AND rel_path_hash=UNHEX(MD5('" + rel + "')) LIMIT 1");
    const std::string file_id = *fr[0][0];
    std::string c = "file_id, hdu, fingerprint", v = file_id + ", 0, UNHEX(MD5('" + rel + "'))";
    for (auto& [k, val] : cols) { c += ", " + k; v += ", " + val; }
    d.exec("INSERT INTO frames (" + c + ") VALUES (" + v + ")");
    auto q = d.query("SELECT id FROM frames WHERE file_id=" + file_id + " AND hdu=0");
    return std::stoll(*q[0][0]);
}

const m::MatchResult* find(const std::vector<m::MatchResult>& rs, const std::string& t) {
    for (const auto& r : rs) if (r.target_type == t) return &r;
    return nullptr;
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) { std::cout << "match_test: SB_TEST_DB_NAME not set; skipping\n"; return 0; }
    const std::string schema = find_sql("schema.sql"), seed = find_sql("seed.sql");
    if (schema.empty() || seed.empty()) { std::cerr << "match_test: no sql\n"; return 1; }

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        d.apply_script(seed);  // provides the default calibration_rules
        for (const char* t : {"frames", "files", "roots", "rigs", "cameras", "filters",
                              "telescopes", "sites"})
            d.exec(std::string("DELETE FROM ") + t);
        // Minimal registry so the frames' camera/rig/filter FKs resolve to id 1.
        d.exec("INSERT INTO sites (id, name, utc_offset_h, is_default) VALUES (1,'S',-7,1)");
        d.exec("INSERT INTO cameras (id, model) VALUES (1,'ASI6200')");
        d.exec("INSERT INTO telescopes (id, name) VALUES (1,'Scope')");
        d.exec("INSERT INTO rigs (id, name, camera_id, telescope_id, site_id, focal_min_mm, "
               "focal_max_mm) VALUES (1,'Rig',1,1,1,1280,1310)");
        d.exec("INSERT INTO filters (id, name) VALUES (1,'Ha')");
        db::RootFields rf; rf.fs_type = "ext4";
        const int root = d.add_root("matchtest", "/tmp/mt", rf);

        // Light: ASI6200(cam 1), gain 100, off 50, bin 1x1, -10C, 300s, rig 1,
        // filter 1, night 2025-11-08.
        const long long light = add_frame(d, root, {
            {"image_type", "'light'"}, {"camera_id", "1"}, {"rig_id", "1"}, {"filter_id", "1"},
            {"gain", "100"}, {"offset_adu", "50"}, {"binx", "1"}, {"biny", "1"},
            {"set_temp_c", "-10"}, {"exposure_s", "300"},
            {"date_obs_utc", "'2025-11-08 04:00:00'"}, {"session_night", "'2025-11-08'"}});

        auto dark = [&](const std::string& night, const std::string& date, int gain,
                        double temp, double exp, bool master) {
            std::map<std::string, std::string> c = {
                {"image_type", master ? "'master'" : "'dark'"},
                {"camera_id", "1"}, {"binx", "1"}, {"biny", "1"},
                {"set_temp_c", std::to_string(temp)}, {"exposure_s", std::to_string(exp)},
                {"date_obs_utc", "'" + date + "'"}, {"session_night", "'" + night + "'"}};
            if (master) c["master_of"] = "'dark'";  // masters drop gain/offset (NULL)
            else { c["gain"] = std::to_string(gain); c["offset_adu"] = "50"; }
            return add_frame(d, root, c);
        };
        const long long dNear = dark("2025-11-07", "2025-11-07 05:00:00", 100, -10, 300, false);
        dark("2025-11-01", "2025-11-01 05:00:00", 100, -10, 300, false);           // older, matches
        dark("2025-11-06", "2025-11-06 05:00:00", 200, -10, 300, false);           // wrong gain
        dark("2025-11-06", "2025-11-06 05:00:00", 100, -12.5, 300, false);         // temp out of tol
        const long long dMaster = dark("2025-11-05", "2025-11-05 05:00:00", 0, -10, 300, true);

        // Bias matching camera/gain/offset/bin.
        add_frame(d, root, {{"image_type", "'bias'"}, {"camera_id", "1"}, {"gain", "100"},
                            {"offset_adu", "50"}, {"binx", "1"}, {"biny", "1"},
                            {"date_obs_utc", "'2025-11-08 03:00:00'"}, {"session_night", "'2025-11-08'"}});

        // Two flats: same night, and an older one. Rule prefers same session.
        add_frame(d, root, {{"image_type", "'flat'"}, {"rig_id", "1"}, {"filter_id", "1"},
                            {"binx", "1"}, {"biny", "1"}, {"date_obs_utc", "'2025-11-08 18:00:00'"},
                            {"session_night", "'2025-11-08'"}});
        add_frame(d, root, {{"image_type", "'flat'"}, {"rig_id", "1"}, {"filter_id", "1"},
                            {"binx", "1"}, {"biny", "1"}, {"date_obs_utc", "'2025-11-20 18:00:00'"},
                            {"session_night", "'2025-11-20'"}});

        auto key = m::light_key_for(d, light);
        check(key.has_value(), "light_key_for a light frame");
        check(!m::light_key_for(d, dNear).has_value(), "light_key_for a dark returns none");

        const auto results = m::match_calibration(d, *key, 10);

        // ---- darks ----
        const auto* dk = find(results, "dark");
        check(dk != nullptr, "a dark result exists");
        if (dk) {
            check(dk->total == 3, "3 darks match (near, old, master); wrong gain + out-of-tol excluded");
            check(!dk->candidates.empty() && dk->candidates[0].is_master,
                  "master dark ranks first (prefer_masters)");
            check(dk->candidates.size() >= 2 && dk->candidates[1].frame_id == dNear,
                  "nearest raw dark ranks after the master");
            check(!dk->candidates.empty() && dk->candidates[0].reason.find("master dark") != std::string::npos,
                  "reason names it a master dark");
            check(!dk->candidates.empty() && dk->candidates[0].reason.find("300") != std::string::npos,
                  "reason mentions the matched exposure");
            (void)dMaster;
        }

        // ---- bias ----
        const auto* bi = find(results, "bias");
        check(bi && bi->total == 1, "one bias matches");

        // ---- flats: same session preferred ----
        const auto* fl = find(results, "flat");
        check(fl && fl->total == 2, "two flats match");
        if (fl && !fl->candidates.empty()) {
            check(fl->candidates[0].session_night == "2025-11-08", "same-session flat ranks first");
            check(fl->candidates[0].reason.find("same night") != std::string::npos,
                  "flat reason notes the same night");
        }

        // ---- dark-flats: none -> warning, not a crash ----
        const auto* df = find(results, "darkflat");
        check(df && df->total == 0 && !df->warning.empty(), "no dark-flats yields a warning");

    } catch (const std::exception& e) {
        std::cerr << "match_test: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) { std::cout << "match_test: all checks passed\n"; return 0; }
    std::cerr << "match_test: " << g_failures << " check(s) failed\n";
    return 1;
}
