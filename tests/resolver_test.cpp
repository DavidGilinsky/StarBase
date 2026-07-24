// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/resolver_test.cpp
// Purpose:       Tests for the header-to-frame resolver: keyword priority, value
//                normalization, frame-type precedence, coordinate parsing, the
//                observing-night rollover, and the filter default.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "fits_reader.hpp"
#include "resolver.hpp"

namespace {

namespace fits = starbase::fits;
namespace ex = starbase::extract;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// Build an image HDU from (keyword, value) pairs.
fits::Hdu make_hdu(std::initializer_list<std::pair<std::string, std::string>> cards,
                   long nx = 9576, long ny = 6388) {
    fits::Hdu h;
    h.is_image = true;
    h.naxis = 2;
    h.naxis1 = nx;
    h.naxis2 = ny;
    for (const auto& c : cards) h.cards.push_back({c.first, c.second, ""});
    return h;
}

void test_nina_light() {
    const auto m = ex::HeaderMapping::defaults();
    ex::SiteContext site{-7.0};  // observatory local offset
    auto h = make_hdu({
        {"IMAGETYP", "LIGHT"}, {"DATE-OBS", "2025-11-27T03:32:39.6264089"},
        {"INSTRUME", "ZWO ASI6200MC Pro"}, {"TELESCOP", "Askar-185"},
        {"OBJECT", "NGC 6960"}, {"RA", "311.404464398845"}, {"DEC", "30.7237002099616"},
        {"FILTER", "CLEAR"}, {"EXPTIME", "300.0"}, {"GAIN", "100"}, {"OFFSET", "9"},
        {"XBINNING", "1"}, {"YBINNING", "1"}, {"SET-TEMP", "0.0"}, {"CCD-TEMP", "0.1"},
        {"FOCALLEN", "1295.0"}, {"PIERSIDE", "East"}, {"ROWORDER", "TOP-DOWN"},
    });
    auto f = ex::resolve(h, m, site);

    check(f.image_type == ex::ImageType::Light, "light classified");
    check(f.object.value_or("") == "NGC 6960", "object");
    check(f.ra_deg && near(*f.ra_deg, 311.404464398845), "RA decimal degrees");
    check(f.dec_deg && near(*f.dec_deg, 30.7237002099616), "DEC decimal degrees");
    check(f.exposure_s && near(*f.exposure_s, 300.0), "exposure");
    check(f.gain.value_or(-1) == 100, "gain");
    check(f.offset_adu.value_or(-1) == 9, "offset");
    check(f.binx.value_or(-1) == 1 && f.biny.value_or(-1) == 1, "binning");
    check(f.set_temp_c && near(*f.set_temp_c, 0.0), "set temp");
    check(f.ccd_temp_c && near(*f.ccd_temp_c, 0.1), "ccd temp (separate from setpoint)");
    check(f.filter_raw.value_or("") == "CLEAR" && !f.filter_defaulted, "filter present, not defaulted");
    check(f.instrume_raw.value_or("") == "ZWO ASI6200MC Pro", "instrume raw");
    check(f.telescope_raw.value_or("") == "Askar-185", "telescope raw (untrusted)");
    check(f.focal_len_mm && near(*f.focal_len_mm, 1295.0), "focal length");
    check(f.pier_side.value_or("") == "east", "pier side normalized");
    check(f.row_order.value_or("") == "top-down", "row order normalized");
    check(f.naxis1.value_or(0) == 9576 && f.naxis2.value_or(0) == 6388, "geometry from image");

    // Local 2025-11-26T20:32, minus 12h -> the night began 2025-11-26.
    check(f.session_night.value_or("") == "2025-11-26", "observing night (noon-to-noon)");
}

void test_type_precedence() {
    const auto m = ex::HeaderMapping::defaults();
    auto t = [&](const std::string& v) {
        return ex::resolve(make_hdu({{"IMAGETYP", v}}), m).image_type;
    };
    check(t("Light Frame") == ex::ImageType::Light, "Light Frame");
    check(t("Dark Frame") == ex::ImageType::Dark, "Dark Frame");
    check(t("Flat Field") == ex::ImageType::Flat, "Flat Field");
    check(t("Bias Frame") == ex::ImageType::Bias, "Bias Frame");
    // 'Master Dark' must beat the bare 'dark' spelling.
    check(t("Master Dark") == ex::ImageType::Master, "Master Dark -> master, not dark");
    check(t("MasterFlat integration") == ex::ImageType::Master, "master via contains");
    // 'flatdark' / 'dark flat' must beat both dark and flat.
    check(t("FlatDark") == ex::ImageType::DarkFlat, "FlatDark -> darkflat");
    check(t("Dark Flat") == ex::ImageType::DarkFlat, "Dark Flat -> darkflat");
    check(t("Zero") == ex::ImageType::Bias, "Zero -> bias");
    check(t("Wibble") == ex::ImageType::Unknown, "unknown spelling");
}

void test_filter_default() {
    const auto m = ex::HeaderMapping::defaults();
    // Light with no FILTER -> CLEAR, flagged as defaulted.
    auto light = ex::resolve(make_hdu({{"IMAGETYP", "LIGHT"}}), m);
    check(light.filter_raw.value_or("") == "CLEAR" && light.filter_defaulted,
          "light without filter defaults to CLEAR");
    // Flat likewise.
    auto flat = ex::resolve(make_hdu({{"IMAGETYP", "FLAT"}}), m);
    check(flat.filter_raw.value_or("") == "CLEAR" && flat.filter_defaulted,
          "flat without filter defaults to CLEAR");
    // Dark with the TheSky placeholder FILTER=!Shutter! -> null, NOT CLEAR.
    auto dark = ex::resolve(make_hdu({{"IMAGETYP", "DARK"}, {"FILTER", "!Shutter!"}}), m);
    check(!dark.filter_raw.has_value(), "dark placeholder filter -> null, not CLEAR");
    // Bias with no filter at all -> null.
    auto bias = ex::resolve(make_hdu({{"IMAGETYP", "Bias"}}), m);
    check(!bias.filter_raw.has_value(), "bias has no filter");
}

void test_thesky_and_aliases() {
    const auto m = ex::HeaderMapping::defaults();
    // TheSky writes GAINRAW and CCDXBIN and a generic INSTRUME.
    auto f = ex::resolve(make_hdu({
        {"IMAGETYP", "Light"}, {"INSTRUME", "ASICamera"},
        {"GAINRAW", "100"}, {"CCDXBIN", "2"},
        {"OBJCTRA", "20 45 42"}, {"OBJCTDEC", "+30 43 00"},
    }), m);
    check(f.gain.value_or(-1) == 100, "gain from GAINRAW fallback");
    check(f.binx.value_or(-1) == 2, "binx from CCDXBIN fallback");
    check(f.instrume_raw.value_or("") == "ASICamera", "generic instrume captured raw");
    // OBJCTRA is sexagesimal hours: 20h45m42s * 15 = 311.425 deg.
    check(f.ra_deg && near(*f.ra_deg, (20 + 45.0 / 60 + 42.0 / 3600) * 15.0, 1e-4),
          "sexagesimal RA in hours -> degrees");
    // OBJCTDEC is sexagesimal degrees.
    check(f.dec_deg && near(*f.dec_deg, 30 + 43.0 / 60 + 0.0 / 3600, 1e-4),
          "sexagesimal DEC in degrees");
}

void test_object_and_coord_helpers() {
    const auto m = ex::HeaderMapping::defaults();
    // NINA Flat Wizard target name is not an object.
    auto f = ex::resolve(make_hdu({{"IMAGETYP", "FLAT"}, {"OBJECT", "FlatWizard"}}), m);
    check(!f.object.has_value() || f.object->empty(), "FlatWizard object suppressed");

    check(ex::parse_coord("311.4", true) && near(*ex::parse_coord("311.4", true), 311.4),
          "decimal RA as degrees");
    check(ex::parse_coord("+30 43 00", false) && near(*ex::parse_coord("+30 43 00", false),
          30.716666, 1e-4), "sexagesimal DEC");
    check(ex::parse_coord("-05:30:00", false) && near(*ex::parse_coord("-05:30:00", false),
          -5.5), "negative sexagesimal with colons");
    check(!ex::parse_coord("", true).has_value(), "empty coord -> none");
}

void test_night_rollover() {
    // An evening exposure (local) belongs to the night that began that day.
    check(ex::night_of("2025-11-27T03:32:39", -7.0).value_or("") == "2025-11-26",
          "post-midnight-UTC evening frame -> prior civil date");
    // A morning exposure before local noon still belongs to the PRIOR night:
    // local 11:00 on the 27th, minus 12 h, lands on the 26th.
    check(ex::night_of("2025-11-27T18:00:00", -7.0).value_or("") == "2025-11-26",
          "local 11:00 (morning) -> prior night's label");
    // Just after local noon rolls to the new night.
    check(ex::night_of("2025-11-27T20:00:00", -7.0).value_or("") == "2025-11-27",
          "local 13:00 -> new night");
    check(!ex::night_of("garbage", -7.0).has_value(), "unparseable date -> none");
}

void test_real_frame() {
    const char* path = std::getenv("SB_TEST_FITS");
    if (!path || !*path) { std::cout << "  (SB_TEST_FITS not set; skipping real-frame test)\n"; return; }
    auto hdr = fits::read_header(path);
    auto imgs = hdr.image_hdus();
    check(!imgs.empty(), "real frame image HDU");
    if (imgs.empty()) return;
    auto f = ex::resolve(*imgs[0], ex::HeaderMapping::defaults(), ex::SiteContext{-7.0});
    check(f.has_date, "real frame has date");
    check(f.image_type != ex::ImageType::Unknown, "real frame classified");
    std::cout << "  real: type=" << ex::to_string(f.image_type)
              << " obj=" << f.object.value_or("-")
              << " filter=" << f.filter_raw.value_or("-")
              << " exp=" << (f.exposure_s ? std::to_string(*f.exposure_s) : "-")
              << " gain=" << (f.gain ? std::to_string(*f.gain) : "-")
              << " night=" << f.session_night.value_or("-")
              << " ra=" << (f.ra_deg ? std::to_string(*f.ra_deg) : "-") << "\n";
}

}  // namespace

int main() {
    test_nina_light();
    test_type_precedence();
    test_filter_default();
    test_thesky_and_aliases();
    test_object_and_coord_helpers();
    test_night_rollover();
    test_real_frame();

    if (g_failures == 0) { std::cout << "resolver_test: all checks passed\n"; return 0; }
    std::cerr << "resolver_test: " << g_failures << " check(s) failed\n";
    return 1;
}
