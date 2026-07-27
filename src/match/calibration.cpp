// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/match/calibration.cpp
// Purpose:       Implementation of the calibration matcher.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "calibration.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace starbase::match {
namespace {

using json = nlohmann::json;

std::optional<std::string> cell(const db::Database::Row& r, size_t i) {
    return (i < r.size() && r[i]) ? r[i] : std::optional<std::string>{};
}
std::optional<int> as_int(const std::optional<std::string>& s) {
    return s ? std::optional<int>(std::stoi(*s)) : std::nullopt;
}
std::optional<double> as_dbl(const std::optional<std::string>& s) {
    return s ? std::optional<double>(std::stod(*s)) : std::nullopt;
}

// One field's matching spec from match_json: "exact" or {"exact":true,
// "null_ok":true} or {"tol":X}.
struct FieldSpec {
    enum Kind { Exact, ExactNullOk, Tol } kind = Exact;
    double tol = 0;
};
FieldSpec parse_spec(const json& v) {
    FieldSpec s;
    if (v.is_string()) { s.kind = FieldSpec::Exact; return s; }
    if (v.is_object()) {
        if (v.contains("tol")) { s.kind = FieldSpec::Tol; s.tol = v["tol"].get<double>(); }
        else if (v.value("null_ok", false)) s.kind = FieldSpec::ExactNullOk;
    }
    return s;
}

// The light-side value and a display name for one match field.
struct FieldVal {
    bool has = false;
    bool is_num = false;
    long long ival = 0;
    double dval = 0;
    std::string sval;
    std::string label;
};

FieldVal light_value(const std::string& field, const LightKey& L) {
    FieldVal v;
    // Populate both ival and dval for a numeric value, so downstream formatting
    // never reads an unset field (which would compare against 0).
    auto seti = [&](const std::optional<int>& o) {
        if (o) { v.has = true; v.is_num = true; v.ival = *o; v.dval = *o; }
    };
    auto setd = [&](const std::optional<double>& o) {
        if (o) { v.has = true; v.is_num = true; v.dval = *o; v.ival = static_cast<long long>(*o); }
    };
    auto sets = [&](const std::optional<std::string>& o) { if (o) { v.has = true; v.sval = *o; } };
    if (field == "camera_id") seti(L.camera_id);
    else if (field == "rig_id") seti(L.rig_id);
    else if (field == "filter_id") seti(L.filter_id);
    else if (field == "gain") seti(L.gain);
    else if (field == "offset_adu") seti(L.offset_adu);
    else if (field == "binx") seti(L.binx);
    else if (field == "biny") seti(L.biny);
    else if (field == "set_temp_c") setd(L.set_temp_c);
    else if (field == "exposure_s") setd(L.exposure_s);
    else if (field == "rotator_deg") setd(L.rotator_deg);
    else if (field == "readout_mode") sets(L.readout_mode);
    v.label = field;
    return v;
}

// Fields allowed in a rule, so a malformed rule cannot inject a column.
bool is_match_field(const std::string& f) {
    static const char* ok[] = {"camera_id",  "rig_id",     "filter_id", "gain",
                               "offset_adu", "binx",       "biny",      "set_temp_c",
                               "exposure_s", "rotator_deg", "readout_mode"};
    for (const char* k : ok) if (f == k) return true;
    return false;
}

std::string trim_num(double d) {
    std::ostringstream o;
    if (d == std::floor(d)) o << static_cast<long long>(d); else o << d;
    return o.str();
}

}  // namespace

std::optional<LightKey> light_key_for(db::Database& db, long long frame_id) {
    auto rows = db.query(
        "SELECT image_type, camera_id, rig_id, filter_id, gain, offset_adu, binx, biny, "
        "set_temp_c, exposure_s, rotator_deg, readout_mode, date_obs_utc, session_night "
        "FROM frames WHERE id = " + std::to_string(frame_id) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    const auto& r = rows[0];
    if (cell(r, 0).value_or("") != "light") return std::nullopt;
    LightKey L;
    L.camera_id = as_int(cell(r, 1));
    L.rig_id = as_int(cell(r, 2));
    L.filter_id = as_int(cell(r, 3));
    L.gain = as_int(cell(r, 4));
    L.offset_adu = as_int(cell(r, 5));
    L.binx = as_int(cell(r, 6));
    L.biny = as_int(cell(r, 7));
    L.set_temp_c = as_dbl(cell(r, 8));
    L.exposure_s = as_dbl(cell(r, 9));
    L.rotator_deg = as_dbl(cell(r, 10));
    L.readout_mode = cell(r, 11);
    L.date_obs_utc = cell(r, 12);
    L.session_night = cell(r, 13);
    return L;
}

namespace {

// The flat set a light should use, if one resolves, with a human-readable
// reason. Priority: an explicit (rig + night) pin; then, for a fixed rig (one
// with a site), the sticky set effective on-or-before the light's night; for a
// mobile rig (no site), the set captured that same night. Empty otherwise, so
// the caller falls back to the heuristic flat rule.
struct FlatSetPick { long long id; std::string reason; };

std::optional<FlatSetPick> resolve_flat_set(db::Database& db, const LightKey& L) {
    if (!L.rig_id) return std::nullopt;
    const std::string rig = std::to_string(*L.rig_id);
    const std::string night = L.session_night ? db.escape(*L.session_night) : std::string();

    // A set fits a light's filter when it is filter-agnostic (filter_id NULL) or
    // shares the filter; a filter-specific set is preferred over an agnostic one.
    // fw/fo apply to bare flat_sets queries, fw_fs to the pin join.
    std::string fw, fo, fw_fs;
    if (L.filter_id) {
        const std::string F = std::to_string(*L.filter_id);
        fw = " AND (filter_id IS NULL OR filter_id = " + F + ")";
        fw_fs = " AND (fs.filter_id IS NULL OR fs.filter_id = " + F + ")";
        fo = "(filter_id IS NOT NULL) DESC, ";
    } else {
        fw = " AND filter_id IS NULL";
        fw_fs = " AND fs.filter_id IS NULL";
    }

    // 1. An explicit (rig + night) pin wins, when it fits the light's filter.
    if (L.session_night) {
        auto p = db.query("SELECT fs.id, fs.label FROM light_flat_pins p "
                          "JOIN flat_sets fs ON fs.id = p.flat_set_id "
                          "WHERE p.scope = 'rig_night' AND p.rig_id = " + rig +
                          " AND p.session_night = '" + night + "'" + fw_fs + " LIMIT 1");
        if (!p.empty() && p[0][0])
            return FlatSetPick{std::stoll(*p[0][0]),
                "pinned flat set '" + p[0][1].value_or("") + "' for " + night};
    }

    auto rr = db.query("SELECT site_id, active_flat_set_id FROM rigs WHERE id = " + rig + " LIMIT 1");
    if (rr.empty()) return std::nullopt;
    const bool fixed = rr[0][0].has_value();  // has a site

    if (fixed) {
        if (L.session_night) {
            // An explicit validity window covering the night, filter-fit, newest first.
            auto w = db.query(
                "SELECT id, label FROM flat_sets WHERE rig_id = " + rig + " AND ("
                "(valid_from IS NOT NULL AND valid_to IS NOT NULL AND '" + night + "' BETWEEN valid_from AND valid_to) OR "
                "(valid_from IS NOT NULL AND valid_to IS NULL AND '" + night + "' >= valid_from))" + fw +
                " ORDER BY " + fo + "captured_night DESC, id DESC LIMIT 1");
            if (!w.empty() && w[0][0])
                return FlatSetPick{std::stoll(*w[0][0]),
                    "flat set '" + w[0][1].value_or("") + "' (window covers " + night + ")"};
            // Else the newest fitting set captured on or before the night.
            auto n = db.query(
                "SELECT id, label FROM flat_sets WHERE rig_id = " + rig +
                " AND (captured_night IS NULL OR captured_night <= '" + night + "')" + fw +
                " ORDER BY " + fo + "captured_night DESC, id DESC LIMIT 1");
            if (!n.empty() && n[0][0])
                return FlatSetPick{std::stoll(*n[0][0]),
                    "flat set '" + n[0][1].value_or("") + "' (newest on/before " + night + ", fixed rig)"};
        }
        // Fallback: the rig's declared active set, if it fits the filter.
        if (rr[0][1]) {
            const std::string sid = *rr[0][1];
            auto a = db.query("SELECT label FROM flat_sets WHERE id = " + sid + fw);
            if (!a.empty())
                return FlatSetPick{std::stoll(sid),
                    "active flat set '" + a[0][0].value_or("") + "' (fixed rig)"};
        }
        return std::nullopt;
    }

    // Mobile rig: the fitting set captured the same session night.
    if (L.session_night) {
        auto s = db.query("SELECT id, label FROM flat_sets WHERE rig_id = " + rig +
                          " AND captured_night = '" + night + "'" + fw +
                          " ORDER BY " + fo + "id DESC LIMIT 1");
        if (!s.empty() && s[0][0])
            return FlatSetPick{std::stoll(*s[0][0]),
                "flat set '" + s[0][1].value_or("") + "' from the same session (" + night + ")"};
    }
    return std::nullopt;
}

}  // namespace

std::vector<MatchResult> match_calibration(db::Database& db, const LightKey& L, int limit) {
    std::vector<MatchResult> out;

    auto rules = db.query(
        "SELECT name, target_type, match_json, max_age_days, prefer_masters, "
        "prefer_same_session, min_frames FROM calibration_rules "
        "WHERE enabled = 1 ORDER BY priority, id");

    for (const auto& rule : rules) {
        MatchResult mr;
        mr.rule_name = cell(rule, 0).value_or("");
        mr.target_type = cell(rule, 1).value_or("");
        const std::string match_json = cell(rule, 2).value_or("{}");
        const std::optional<int> max_age = as_int(cell(rule, 3));
        const bool prefer_masters = cell(rule, 4).value_or("0") != "0";
        const bool prefer_session = cell(rule, 5).value_or("0") != "0";
        const std::optional<int> min_frames = as_int(cell(rule, 6));

        json spec;
        try { spec = json::parse(match_json); }
        catch (const std::exception&) { mr.warning = "rule has invalid match_json"; out.push_back(mr); continue; }

        // Flat sets override the time/session heuristic: when a specific set is
        // pinned or active for this light's rig, its flats (for the light's
        // filter, masters first) are the answer, and the rule's ranking is not
        // consulted. If no set resolves, fall through to the heuristic below.
        if (mr.target_type == "flat") {
            if (auto pick = resolve_flat_set(db, L)) {
                std::string w = "f.flat_set_id = " + std::to_string(pick->id);
                if (L.filter_id)
                    w += " AND (f.filter_id = " + std::to_string(*L.filter_id) + " OR f.filter_id IS NULL)";
                auto tr = db.query("SELECT COUNT(*) FROM frames f WHERE " + w);
                mr.total = (tr.empty() || !tr[0][0]) ? 0 : std::stoll(*tr[0][0]);
                auto rows = db.query(
                    "SELECT f.id, f.image_type, f.session_night, f.date_obs_utc, fl.filename, "
                    "CONCAT(r.path,'/',fl.rel_path) FROM frames f JOIN files fl ON fl.id = f.file_id "
                    "JOIN roots r ON r.id = fl.root_id WHERE " + w +
                    " ORDER BY (f.image_type = 'master') DESC, f.date_obs_utc ASC LIMIT " +
                    std::to_string(limit));
                int rank = 0;
                for (const auto& fr : rows) {
                    Candidate c;
                    c.frame_id = std::stoll(*fr[0]);
                    c.image_type = cell(fr, 1).value_or("");
                    c.is_master = c.image_type == "master";
                    c.session_night = cell(fr, 2).value_or("");
                    c.date_obs_utc = cell(fr, 3).value_or("");
                    c.filename = cell(fr, 4).value_or("");
                    c.abs_path = cell(fr, 5).value_or("");
                    c.score = 1.0 - static_cast<double>(rank++) / (limit + 1);
                    c.reason = (c.is_master ? "master flat · " : "flat · ") + pick->reason;
                    mr.candidates.push_back(std::move(c));
                }
                mr.rule_name += " (flat set)";
                if (mr.total == 0) mr.warning = "flat set resolved, but it has no flats for this filter";
                out.push_back(std::move(mr));
                continue;
            }
        }

        // The target frames: raw calibration of this type, plus masters of it.
        std::string where = "(f.image_type = '" + db.escape(mr.target_type) +
                            "' OR (f.image_type = 'master' AND f.master_of = '" +
                            db.escape(mr.target_type) + "'))";

        // Track the reason components for whichever fields the rule matched on.
        std::vector<std::string> reason_bits;
        bool unsatisfiable = false;

        for (auto it = spec.begin(); it != spec.end(); ++it) {
            const std::string field = it.key();
            if (!is_match_field(field)) continue;  // ignore unknown rule keys
            const FieldSpec fs = parse_spec(it.value());
            const FieldVal lv = light_value(field, L);

            if (!lv.has) {
                // The light lacks this value. exact/exact_null_ok can still match
                // a calibration frame that also lacks it; a tol field is skipped.
                if (fs.kind == FieldSpec::Exact) {
                    where += " AND f." + field + " IS NULL";
                }
                continue;
            }

            const std::string col = "f." + field;
            const std::string val = lv.is_num ? trim_num(lv.dval) : "'" + db.escape(lv.sval) + "'";
            const std::string disp = lv.is_num ? val : lv.sval;

            switch (fs.kind) {
                case FieldSpec::Exact:
                    where += " AND " + col + " = " + val;
                    reason_bits.push_back(field + " " + disp);
                    break;
                case FieldSpec::ExactNullOk:
                    // A master that dropped this card (NULL) still matches.
                    where += " AND (" + col + " = " + val + " OR " + col + " IS NULL)";
                    reason_bits.push_back(field + " " + disp);
                    break;
                case FieldSpec::Tol:
                    where += " AND ABS(" + col + " - " + trim_num(lv.dval) + ") <= " + trim_num(fs.tol);
                    reason_bits.push_back(field + " " + trim_num(lv.dval) + "±" + trim_num(fs.tol));
                    break;
            }
            (void)unsatisfiable;
        }

        // Age limit relative to the light, when both have a date.
        if (max_age && L.date_obs_utc) {
            where += " AND (f.date_obs_utc IS NULL OR ABS(TIMESTAMPDIFF(DAY, f.date_obs_utc, '" +
                     db.escape(*L.date_obs_utc) + "')) <= " + std::to_string(*max_age) + ")";
        }

        // Count and fetch, ranked. Masters first (if preferred), then same
        // session (if preferred), then nearest in time.
        auto tr = db.query("SELECT COUNT(*) FROM frames f WHERE " + where);
        mr.total = (tr.empty() || !tr[0][0]) ? 0 : std::stoll(*tr[0][0]);

        std::string order;
        if (prefer_masters) order += "(f.image_type = 'master') DESC, ";
        if (prefer_session && L.session_night)
            order += "(f.session_night = '" + db.escape(*L.session_night) + "') DESC, ";
        if (L.date_obs_utc)
            order += "ABS(TIMESTAMPDIFF(SECOND, f.date_obs_utc, '" + db.escape(*L.date_obs_utc) +
                     "')) ASC, ";
        order += "f.id ASC";

        auto rows = db.query(
            "SELECT f.id, f.image_type, f.session_night, f.date_obs_utc, fl.filename, "
            "CONCAT(r.path,'/',fl.rel_path) FROM frames f JOIN files fl ON fl.id = f.file_id "
            "JOIN roots r ON r.id = fl.root_id WHERE " + where + " ORDER BY " + order +
            " LIMIT " + std::to_string(limit));

        const std::string base = [&] {
            std::string b;
            for (size_t i = 0; i < reason_bits.size(); ++i) b += (i ? ", " : "") + reason_bits[i];
            return b;
        }();

        int rank = 0;
        for (const auto& fr : rows) {
            Candidate c;
            c.frame_id = std::stoll(*fr[0]);
            c.image_type = cell(fr, 1).value_or("");
            c.is_master = c.image_type == "master";
            c.session_night = cell(fr, 2).value_or("");
            c.date_obs_utc = cell(fr, 3).value_or("");
            c.filename = cell(fr, 4).value_or("");
            c.abs_path = cell(fr, 5).value_or("");
            // Score decays with rank so the API order is also a numeric ranking.
            c.score = 1.0 - static_cast<double>(rank++) / (limit + 1);

            std::string why = c.is_master ? "master " + mr.target_type : mr.target_type;
            if (!base.empty()) why += " · " + base;
            if (prefer_session && L.session_night && c.session_night == *L.session_night)
                why += " · same night (" + c.session_night + ")";
            else if (!c.date_obs_utc.empty() && L.date_obs_utc) {
                auto d = db.query("SELECT ABS(TIMESTAMPDIFF(DAY, '" + db.escape(c.date_obs_utc) +
                                  "', '" + db.escape(*L.date_obs_utc) + "'))");
                if (!d.empty() && d[0][0]) why += " · " + *d[0][0] + " day(s) from light";
            }
            c.reason = why;
            mr.candidates.push_back(std::move(c));
        }

        if (mr.total == 0) mr.warning = "no matching " + mr.target_type + " frames found";
        else if (min_frames && mr.total < *min_frames)
            mr.warning = "only " + std::to_string(mr.total) + " " + mr.target_type +
                         " frames (rule suggests at least " + std::to_string(*min_frames) + ")";

        out.push_back(std::move(mr));
    }

    return out;
}

}  // namespace starbase::match
