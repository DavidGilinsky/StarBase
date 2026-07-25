// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/extract/resolver.hpp
// Purpose:       Resolve a raw FITS header into the normalized frame fields the
//                database promotes, driven by the header-mapping tables
//                (header_map, header_value_map) seeded from the archive survey.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fits_reader.hpp"

namespace starbase::extract {

// One raw-value normalization rule, mirroring a header_value_map row.
struct ValueRule {
    std::string raw;
    std::string normalized;
    enum class Mode { Exact, Contains, Regex } mode = Mode::Exact;
    int priority = 100;
};

// The mapping the resolver applies, loaded from the database (header_map +
// header_value_map). Kept as plain in-memory data so the resolver has no
// database dependency and is testable in isolation.
struct HeaderMapping {
    // field -> candidate FITS keywords, in priority order (lowest first).
    std::map<std::string, std::vector<std::string>> keywords;
    // field -> value-normalization rules, in priority order.
    std::map<std::string, std::vector<ValueRule>> values;

    // The default mapping from sql/seed.sql, for tests and as a fallback when
    // the database has not been consulted. The database copy is authoritative.
    static HeaderMapping defaults();
};

enum class ImageType { Unknown, Light, Dark, Flat, Bias, DarkFlat, Master };
const char* to_string(ImageType t);

// The normalized fields the resolver produces from one image HDU. Optionals are
// empty when the header does not carry the value. Equipment *ids* (camera_id,
// rig_id, filter_id) are resolved separately against the registry; the resolver
// produces the raw values those lookups key on.
struct ResolvedFrame {
    ImageType   image_type = ImageType::Unknown;
    std::optional<ImageType> master_of;   // when image_type == Master

    std::optional<std::string> object;            // as written
    std::optional<std::string> object_canonical;  // catalog-canonical form
    std::optional<double>      ra_deg;            // decimal degrees
    std::optional<double>      dec_deg;

    std::optional<std::string> date_obs_utc;      // ISO-8601, sub-second trimmed
    std::optional<std::string> session_night;     // local noon-to-noon date
    std::optional<double>      exposure_s;

    std::optional<std::string> instrume_raw;      // camera, as written
    std::optional<std::string> telescope_raw;     // usually the mount; untrusted
    std::optional<double>      focal_len_mm;
    std::optional<std::string> filter_raw;
    bool                       filter_defaulted = false;  // CLEAR injected, not observed

    std::optional<int>    gain;
    std::optional<int>    offset_adu;
    std::optional<int>    binx;
    std::optional<int>    biny;
    std::optional<int>    naxis1;
    std::optional<int>    naxis2;
    std::optional<double> ccd_temp_c;
    std::optional<double> set_temp_c;
    std::optional<double> airmass;
    // Sky brightness, when the header carries it (e.g. stamped by
    // nightwatcher-ingest). sqm_mag_arcsec2 is the queryable value.
    std::optional<double>      sqm_mag_arcsec2;
    std::optional<std::string> sqm_sensor;
    std::optional<std::string> sqm_time_utc;
    std::optional<int>         sqm_dt_s;
    std::optional<std::string> pier_side;   // east | west
    std::optional<std::string> row_order;   // top-down | bottom-up

    // Whether DATE-OBS was present; a frame without it is quarantine-bound.
    bool has_date = false;
};

// The observing site whose local time defines the noon-to-noon night. Only the
// UTC offset is needed here; full site matching lives with the registry.
struct SiteContext {
    double utc_offset_hours = 0.0;
};

// Resolve one image HDU. `site` supplies the offset for the observing-night
// rollover (default UTC when unknown).
ResolvedFrame resolve(const fits::Hdu& hdu, const HeaderMapping& mapping,
                      const SiteContext& site = {});

// --- Individually testable helpers ---

// First present, non-empty header value for `field`, trying its keywords in
// priority order. Empty if none is present.
std::optional<std::string> resolve_raw(const fits::Hdu& hdu,
                                       const HeaderMapping& mapping,
                                       const std::string& field);

// Apply the value rules for `field` to a raw string. Returns the raw string
// unchanged when no rule matches.
std::string normalize_value(const HeaderMapping& mapping, const std::string& field,
                            const std::string& raw);

// Parse a coordinate to decimal degrees. `is_ra` treats a sexagesimal value as
// hours (RA), multiplying by 15; a decimal value is taken as degrees as-is,
// matching how NINA writes RA/DEC (decimal degrees) vs OBJCTRA (sexagesimal
// hours) / OBJCTDEC (sexagesimal degrees).
std::optional<double> parse_coord(const std::string& value, bool is_ra);

// Local noon-to-noon observing night (YYYY-MM-DD) for a UTC DATE-OBS and offset.
std::optional<std::string> night_of(const std::string& date_obs_utc,
                                    double utc_offset_hours);

}  // namespace starbase::extract
