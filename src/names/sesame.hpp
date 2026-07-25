// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/names/sesame.hpp
// Purpose:       Optional online target-name resolution via the CDS Sesame
//                service (SIMBAD/NED/VizieR). Turns any alias into a canonical
//                identifier and J2000 coordinates. Opt-in: it is the only part
//                of StarBase that reaches the network, so it is off by default.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase::names {

struct SesameResult {
    bool ok = false;            // a resolver returned a match
    std::string name;           // canonical identifier (e.g. "NGC 7000")
    std::string otype;          // object type, when given
    double ra_deg = 0.0;
    double dec_deg = 0.0;
    bool has_coords = false;
    std::string error;          // network/HTTP error, when !ok
};

// Resolve `raw` through CDS Sesame at `base_url` (scheme://host, e.g.
// "https://cds.unistra.fr"). Queries SIMBAD, then NED, then VizieR. Never
// throws; a failure is reported in the result's error field.
SesameResult sesame_resolve(const std::string& raw, const std::string& base_url,
                            int timeout_s = 8);

}  // namespace starbase::names
