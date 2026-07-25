// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/match/calibration.hpp
// Purpose:       Match calibration frames (darks, flats, bias, dark-flats) to a
//                light requirement, using the declarative calibration_rules.
//                Every candidate carries a score and a human-readable reason:
//                a match that cannot be explained is a match that cannot be
//                trusted.
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

namespace starbase::match {

// The light-side characteristics a calibration frame is matched against. Only
// the fields a given rule names are used; the rest are ignored. date/night drive
// ranking (nearest in time, same session preferred), not matching.
struct LightKey {
    std::optional<int>         camera_id;
    std::optional<int>         rig_id;
    std::optional<int>         filter_id;
    std::optional<int>         gain;
    std::optional<int>         offset_adu;
    std::optional<int>         binx;
    std::optional<int>         biny;
    std::optional<double>      set_temp_c;
    std::optional<double>      exposure_s;
    std::optional<double>      rotator_deg;
    std::optional<std::string> readout_mode;
    std::optional<std::string> date_obs_utc;
    std::optional<std::string> session_night;
};

struct Candidate {
    long long   frame_id = 0;
    std::string image_type;   // dark | flat | bias | darkflat | master
    bool        is_master = false;
    std::string filename;
    std::string abs_path;
    std::string session_night;
    std::string date_obs_utc;
    double      score = 0;     // higher is better
    std::string reason;        // why this frame matched, in plain words
};

struct MatchResult {
    std::string target_type;   // dark | flat | bias | darkflat
    std::string rule_name;
    long long   total = 0;     // total matching frames (before the limit)
    std::vector<Candidate> candidates;
    std::string warning;       // e.g. below the rule's min_frames, or none found
};

// Load a light's key fields from its frame id. Empty if the frame is not a light
// or does not exist.
std::optional<LightKey> light_key_for(db::Database& db, long long frame_id);

// Match calibration frames for one light requirement. Runs every enabled rule
// (highest priority first) and returns one MatchResult per target type, each
// with up to `limit` ranked candidates. Masters rank above raw frames when the
// rule prefers them; within that, the rule's own session/time ranking applies.
std::vector<MatchResult> match_calibration(db::Database& db, const LightKey& light,
                                           int limit = 10);

}  // namespace starbase::match
