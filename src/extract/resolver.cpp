// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/extract/resolver.cpp
// Purpose:       Implementation of the header-to-frame resolver.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "resolver.hpp"

#include "canon.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <regex>
#include <sstream>

namespace starbase::extract {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::optional<double> to_double(const std::string& s) {
    try {
        size_t pos = 0;
        const double d = std::stod(s, &pos);
        // Trailing non-numeric characters (a unit suffix, a stray letter) mean
        // this was not a clean number; reject rather than silently truncate.
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        return pos == s.size() ? std::optional<double>(d) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int> to_int_round(const std::string& s) {
    if (auto d = to_double(s)) return static_cast<int>(std::lround(*d));
    return std::nullopt;
}

}  // namespace

const char* to_string(ImageType t) {
    switch (t) {
        case ImageType::Light:    return "light";
        case ImageType::Dark:     return "dark";
        case ImageType::Flat:     return "flat";
        case ImageType::Bias:     return "bias";
        case ImageType::DarkFlat: return "darkflat";
        case ImageType::Master:   return "master";
        case ImageType::Unknown:  return "unknown";
    }
    return "unknown";
}

std::optional<std::string> resolve_raw(const fits::Hdu& hdu,
                                       const HeaderMapping& mapping,
                                       const std::string& field) {
    const auto it = mapping.keywords.find(field);
    if (it == mapping.keywords.end()) return std::nullopt;
    for (const auto& kw : it->second) {
        if (auto v = hdu.get(kw)) {
            const std::string t = trim(*v);
            if (!t.empty()) return t;
        }
    }
    return std::nullopt;
}

std::string normalize_value(const HeaderMapping& mapping, const std::string& field,
                            const std::string& raw) {
    const auto it = mapping.values.find(field);
    if (it == mapping.values.end()) return raw;
    const std::string lraw = lower(raw);
    for (const auto& rule : it->second) {  // already in priority order
        switch (rule.mode) {
            case ValueRule::Mode::Exact:
                if (lraw == lower(rule.raw)) return rule.normalized;
                break;
            case ValueRule::Mode::Contains:
                if (lraw.find(lower(rule.raw)) != std::string::npos) return rule.normalized;
                break;
            case ValueRule::Mode::Regex:
                try {
                    if (std::regex_search(raw, std::regex(rule.raw, std::regex::icase)))
                        return rule.normalized;
                } catch (const std::regex_error&) { /* skip a bad pattern */ }
                break;
        }
    }
    return raw;
}

std::optional<double> parse_coord(const std::string& value, bool is_ra) {
    const std::string s = trim(value);
    if (s.empty()) return std::nullopt;

    // A clean decimal is degrees as-is (how NINA writes RA/DEC).
    if (auto d = to_double(s)) return d;

    // Sexagesimal: sign, then three fields separated by spaces or colons.
    static const std::regex re(R"(^([+-]?)\s*(\d+)[\s:]+(\d+)[\s:]+([\d.]+))");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return std::nullopt;
    const double sign = (m[1] == "-") ? -1.0 : 1.0;
    const double a = std::stod(m[2]);
    const double b = std::stod(m[3]);
    const double c = std::stod(m[4]);
    double deg = sign * (a + b / 60.0 + c / 3600.0);
    // Sexagesimal RA is in hours; convert to degrees. Sexagesimal DEC is degrees.
    if (is_ra) deg *= 15.0;
    return deg;
}

std::optional<std::string> night_of(const std::string& date_obs_utc,
                                    double utc_offset_hours) {
    // Parse the leading YYYY-MM-DDThh:mm:ss; ignore any sub-second tail.
    std::tm tm{};
    int y, mo, d, h, mi;
    double se = 0;
    if (std::sscanf(date_obs_utc.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &se) < 5 &&
        std::sscanf(date_obs_utc.c_str(), "%d-%d-%d %d:%d:%lf", &y, &mo, &d, &h, &mi, &se) < 5)
        return std::nullopt;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = static_cast<int>(se);

    // Work in UTC seconds (timegm is UTC; it ignores the local zone).
    std::time_t utc = timegm(&tm);
    if (utc == static_cast<std::time_t>(-1)) return std::nullopt;

    // Shift to local time, then back up 12 h so the night is labelled by the
    // civil date it began on (noon-to-noon).
    const auto local = utc + static_cast<std::time_t>(std::lround(utc_offset_hours * 3600.0));
    const auto shifted = local - 12 * 3600;
    std::tm out{};
    gmtime_r(&shifted, &out);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", out.tm_year + 1900, out.tm_mon + 1,
                  out.tm_mday);
    return std::string(buf);
}

namespace {

// Trim ASI's 7 sub-second digits (and any 'Z') to a stable stored form.
std::string normalize_date_obs(const std::string& raw) {
    std::string s = trim(raw);
    if (!s.empty() && (s.back() == 'Z' || s.back() == 'z')) s.pop_back();
    // Keep at most 6 fractional digits.
    const auto dot = s.find('.');
    if (dot != std::string::npos && s.size() - dot - 1 > 6) s = s.substr(0, dot + 7);
    return s;
}

ImageType classify(const std::string& normalized) {
    if (normalized == "light")    return ImageType::Light;
    if (normalized == "dark")     return ImageType::Dark;
    if (normalized == "flat")     return ImageType::Flat;
    if (normalized == "bias")     return ImageType::Bias;
    if (normalized == "darkflat") return ImageType::DarkFlat;
    if (normalized == "master")   return ImageType::Master;
    return ImageType::Unknown;
}

}  // namespace

ResolvedFrame resolve(const fits::Hdu& hdu, const HeaderMapping& mapping,
                      const SiteContext& site) {
    ResolvedFrame f;

    // ---- Frame type ----
    if (auto raw = resolve_raw(hdu, mapping, "image_type"))
        f.image_type = classify(normalize_value(mapping, "image_type", *raw));

    // ---- Time and observing night ----
    if (auto raw = resolve_raw(hdu, mapping, "date_obs")) {
        f.has_date = true;
        f.date_obs_utc = normalize_date_obs(*raw);
        f.session_night = night_of(*f.date_obs_utc, site.utc_offset_hours);
    }

    // ---- Target ----
    // Normalize the object too: NINA's Flat Wizard stamps OBJECT=FlatWizard,
    // which maps to empty and is suppressed rather than stored as a target.
    if (auto raw = resolve_raw(hdu, mapping, "object")) {
        const std::string norm = normalize_value(mapping, "object", *raw);
        if (!norm.empty()) {
            f.object = norm;
            // Catalog-canonical form (M101 -> "M 101"); skip placeholders.
            const auto c = starbase::names::canonicalize(norm);
            if (!c.placeholder) f.object_canonical = c.canonical;
        }
    }
    if (auto raw = resolve_raw(hdu, mapping, "ra"))  f.ra_deg = parse_coord(*raw, /*is_ra=*/true);
    if (auto raw = resolve_raw(hdu, mapping, "dec")) f.dec_deg = parse_coord(*raw, /*is_ra=*/false);

    // ---- Equipment (raw values; id resolution is a registry join) ----
    f.instrume_raw = resolve_raw(hdu, mapping, "instrument");
    f.telescope_raw = resolve_raw(hdu, mapping, "telescope");
    if (auto raw = resolve_raw(hdu, mapping, "focal_len_mm")) f.focal_len_mm = to_double(*raw);

    // ---- Filter, with the type-dependent default ----
    if (auto raw = resolve_raw(hdu, mapping, "filter")) {
        const std::string norm = normalize_value(mapping, "filter", *raw);
        if (!norm.empty()) f.filter_raw = norm;  // a placeholder maps to empty
    }
    // A light or flat with no usable filter is treated as CLEAR (the light path
    // still passed through open air); darks and bias have no filter at all, so
    // they are left null. Mirrors nightwatcher-ingest's resolve.filter default.
    if (!f.filter_raw &&
        (f.image_type == ImageType::Light || f.image_type == ImageType::Flat)) {
        f.filter_raw = "CLEAR";
        f.filter_defaulted = true;
    }

    // ---- Capture parameters ----
    if (auto raw = resolve_raw(hdu, mapping, "exposure_s")) f.exposure_s = to_double(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "gain"))       f.gain = to_int_round(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "offset_adu")) f.offset_adu = to_int_round(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "binx"))       f.binx = to_int_round(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "biny"))       f.biny = to_int_round(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "set_temp_c")) f.set_temp_c = to_double(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "ccd_temp_c")) f.ccd_temp_c = to_double(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "airmass"))    f.airmass = to_double(*raw);

    // Sky brightness (SQM), when the header carries it. Only the value is
    // required; the sensor id, reading time, and offset are recorded when present.
    if (auto raw = resolve_raw(hdu, mapping, "sqm"))        f.sqm_mag_arcsec2 = to_double(*raw);
    if (auto raw = resolve_raw(hdu, mapping, "sqm_sensor")) f.sqm_sensor = *raw;
    if (auto raw = resolve_raw(hdu, mapping, "sqm_time")) {
        const std::string t = normalize_date_obs(*raw);
        if (!t.empty()) f.sqm_time_utc = t;
    }
    if (auto raw = resolve_raw(hdu, mapping, "sqm_dt_s"))   f.sqm_dt_s = to_int_round(*raw);

    // Geometry is authoritative from the image itself, not a header card.
    if (hdu.naxis1 > 0) f.naxis1 = static_cast<int>(hdu.naxis1);
    if (hdu.naxis2 > 0) f.naxis2 = static_cast<int>(hdu.naxis2);

    if (auto raw = resolve_raw(hdu, mapping, "pier_side"))
        f.pier_side = normalize_value(mapping, "pier_side", *raw);
    if (auto raw = resolve_raw(hdu, mapping, "row_order"))
        f.row_order = normalize_value(mapping, "row_order", *raw);

    return f;
}

}  // namespace starbase::extract
