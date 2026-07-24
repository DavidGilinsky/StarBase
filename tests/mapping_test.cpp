// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/mapping_test.cpp
// Purpose:       Verify the header mapping loaded from the database (seed.sql)
//                agrees with the compiled-in HeaderMapping::defaults(), both by
//                field and by resolver behaviour, so the two cannot drift apart
//                unnoticed.
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
#include <sstream>
#include <string>

#include "database.hpp"
#include "fits_reader.hpp"
#include "mapping_loader.hpp"
#include "resolver.hpp"

namespace {

namespace db = starbase::db;
namespace ex = starbase::extract;
namespace fits = starbase::fits;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

std::string find_sql(const std::string& name) {
    for (const std::string base : {"sql/", "../sql/", "../../sql/"})
        if (std::filesystem::exists(base + name)) return base + name;
    return {};
}

fits::Hdu make_hdu(std::initializer_list<std::pair<std::string, std::string>> cards) {
    fits::Hdu h;
    h.is_image = true; h.naxis = 2; h.naxis1 = 100; h.naxis2 = 80;
    for (const auto& c : cards) h.cards.push_back({c.first, c.second, ""});
    return h;
}

// A compact, comparable summary of the resolver-relevant output.
std::string summary(const ex::ResolvedFrame& f) {
    std::ostringstream o;
    o << ex::to_string(f.image_type)
      << "|obj=" << f.object.value_or("-")
      << "|filt=" << f.filter_raw.value_or("-") << (f.filter_defaulted ? "*" : "")
      << "|exp=" << (f.exposure_s ? std::to_string(*f.exposure_s) : "-")
      << "|gain=" << (f.gain ? std::to_string(*f.gain) : "-")
      << "|binx=" << (f.binx ? std::to_string(*f.binx) : "-")
      << "|ra=" << (f.ra_deg ? std::to_string(*f.ra_deg) : "-")
      << "|pier=" << f.pier_side.value_or("-");
    return o.str();
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) {
        std::cout << "mapping_test: SB_TEST_DB_NAME not set; skipping\n";
        return 0;
    }
    const std::string schema = find_sql("schema.sql"), seed = find_sql("seed.sql");
    if (schema.empty() || seed.empty()) {
        std::cerr << "mapping_test: cannot locate sql/schema.sql and sql/seed.sql\n";
        return 1;
    }

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        d.apply_script(seed);

        const ex::HeaderMapping dbmap = starbase::index::load_mapping(d);
        const ex::HeaderMapping def = ex::HeaderMapping::defaults();

        // seed.sql defines more fields than the resolver uses yet, so the DB is a
        // superset. For every field the defaults know, the DB must carry the same
        // keyword candidates in the same order.
        for (const auto& [field, kws] : def.keywords) {
            auto it = dbmap.keywords.find(field);
            check(it != dbmap.keywords.end(), "DB has field '" + field + "'");
            if (it != dbmap.keywords.end())
                check(it->second == kws, "field '" + field + "' keyword order matches seed");
        }

        // The behavioural check: resolving representative frames must give the
        // same result under the DB mapping and the compiled defaults. If seed.sql
        // and resolver_defaults.cpp drift, this fails.
        const ex::SiteContext site{-7.0};
        std::initializer_list<fits::Hdu> frames = {
            make_hdu({{"IMAGETYP", "LIGHT"}, {"DATE-OBS", "2025-11-27T03:32:39"},
                      {"OBJECT", "NGC 6960"}, {"RA", "311.4"}, {"DEC", "30.7"},
                      {"FILTER", "CLEAR"}, {"EXPTIME", "300.0"}, {"GAIN", "100"},
                      {"XBINNING", "1"}, {"PIERSIDE", "East"}}),
            make_hdu({{"IMAGETYP", "DARK"}, {"FILTER", "!Shutter!"},
                      {"GAINRAW", "100"}, {"CCDXBIN", "2"}, {"EXPTIME", "90"}}),
            make_hdu({{"IMAGETYP", "FlatDark"}, {"EXPTIME", "1.2"}}),
            make_hdu({{"IMAGETYP", "Master Dark"}}),
            make_hdu({{"IMAGETYP", "FLAT"}, {"OBJECT", "FlatWizard"}}),
            make_hdu({{"IMAGETYP", "Light"}, {"OBJCTRA", "20 45 42"},
                      {"OBJCTDEC", "+30 43 00"}}),
        };
        int n = 0;
        for (const auto& h : frames) {
            const std::string a = summary(ex::resolve(h, dbmap, site));
            const std::string b = summary(ex::resolve(h, def, site));
            check(a == b, "frame " + std::to_string(n) + " resolves identically (db vs defaults)");
            if (a != b) std::cerr << "    db:  " << a << "\n    def: " << b << "\n";
            ++n;
        }

        // Sanity: the DB genuinely carries more fields than the resolver uses
        // (latitude, focus_pos, sqm, ...), proving we loaded the full seed.
        check(dbmap.keywords.size() > def.keywords.size(),
              "DB mapping is a superset of the resolver-used fields");

    } catch (const std::exception& e) {
        std::cerr << "mapping_test: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) { std::cout << "mapping_test: all checks passed\n"; return 0; }
    std::cerr << "mapping_test: " << g_failures << " check(s) failed\n";
    return 1;
}
