// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/extract/resolver_defaults.cpp
// Purpose:       Built-in default header mapping, mirroring sql/seed.sql. The
//                database copy is authoritative at runtime; this is the fallback
//                and the fixture the resolver tests run against.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "resolver.hpp"

namespace starbase::extract {

HeaderMapping HeaderMapping::defaults() {
    using M = ValueRule::Mode;
    HeaderMapping m;

    // ---- Keyword priority (candidates tried low-to-high; first present wins) ----
    m.keywords = {
        {"date_obs",     {"DATE-OBS", "DATE_OBS"}},
        {"exposure_s",   {"EXPTIME", "EXPOSURE"}},
        {"gain",         {"GAIN", "GAINRAW", "GAINADU"}},
        {"offset_adu",   {"OFFSET", "BLKLEVEL"}},
        {"binx",         {"XBINNING", "CCDXBIN"}},
        {"biny",         {"YBINNING", "CCDYBIN"}},
        {"set_temp_c",   {"SET-TEMP"}},
        {"ccd_temp_c",   {"CCD-TEMP"}},
        {"focal_len_mm", {"FOCALLEN"}},
        // INSTRUME is the camera; TELESCOP is the mount as often as the optics,
        // so it is captured raw and never trusted for rig resolution.
        {"instrument",   {"INSTRUME", "CAMERA"}},
        {"telescope",    {"TELESCOP"}},
        {"image_type",   {"IMAGETYP", "FRAME", "PICTTYPE"}},
        {"object",       {"OBJECT"}},
        // RA/DEC are decimal degrees; OBJCTRA (hours) / OBJCTDEC (degrees) are
        // the sexagesimal fallbacks, handled by parse_coord.
        {"ra",           {"RA", "OBJCTRA"}},
        {"dec",          {"DEC", "OBJCTDEC"}},
        {"filter",       {"FILTER"}},
        {"airmass",      {"AIRMASS"}},
        {"sqm",          {"SQM"}},
        {"sqm_sensor",   {"SQMSRC"}},
        {"sqm_time",     {"SQMTIME"}},
        {"sqm_dt_s",     {"SQMDT"}},
        {"pier_side",    {"PIERSIDE"}},
        {"row_order",    {"ROWORDER"}},
    };

    // ---- Value normalization, in priority order (lowest number first) ----
    // Dark-flats and masters must beat the plain type words, so they come first.
    m.values["image_type"] = {
        {"flatdark",    "darkflat", M::Contains, 4},
        {"dark flat",   "darkflat", M::Contains, 4},
        {"darkflat",    "darkflat", M::Contains, 4},
        {"Master Dark", "master",   M::Exact,    5},
        {"master",      "master",   M::Contains, 6},
        {"integration", "master",   M::Contains, 6},
        {"stack",       "master",   M::Contains, 6},
        {"LIGHT",       "light",    M::Exact,   10},
        {"Light",       "light",    M::Exact,   10},
        {"Light Frame", "light",    M::Exact,   10},
        {"DARK",        "dark",     M::Exact,   10},
        {"Dark",        "dark",     M::Exact,   10},
        {"Dark Frame",  "dark",     M::Exact,   10},
        {"FLAT",        "flat",     M::Exact,   10},
        {"Flat",        "flat",     M::Exact,   10},
        {"Flat Field",  "flat",     M::Exact,   10},
        {"Bias",        "bias",     M::Exact,   10},
        {"Bias Frame",  "bias",     M::Exact,   10},
        {"BIAS",        "bias",     M::Exact,   10},
        {"zero",        "bias",     M::Contains, 20},
    };

    // Placeholder filter values that mean "no filter in the path", not a name.
    m.values["filter"] = {
        {"!Shutter!", "", M::Exact, 10},
        {"DARK",      "", M::Exact, 10},
        {"None",      "", M::Exact, 10},
    };

    // NINA's Flat Wizard stamps its own name as the target; it is not an object.
    m.values["object"] = {
        {"FlatWizard", "", M::Exact, 10},
    };

    m.values["pier_side"] = {
        {"East", "east", M::Exact, 10},
        {"West", "west", M::Exact, 10},
    };
    m.values["row_order"] = {
        {"TOP-DOWN",  "top-down",  M::Exact, 10},
        {"BOTTOM-UP", "bottom-up", M::Exact, 10},
    };

    return m;
}

}  // namespace starbase::extract
