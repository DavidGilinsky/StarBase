// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/fits_test.cpp
// Purpose:       Tests for the FITS header reader and identity fingerprint.
//                Hermetic by default (writes a synthetic FITS with CFITSIO and
//                reads it back); also validates a real archive frame when
//                SB_TEST_FITS names one.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <fitsio.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>

#include "fits_reader.hpp"

namespace {

namespace fits = starbase::fits;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

// Write a small synthetic FITS whose header mimics a real NINA light frame, so
// the reader is exercised without depending on the archive.
std::string write_synthetic() {
    std::string path = "/tmp/starbase_fits_test_" + std::to_string(::getpid()) + ".fits";
    std::remove(path.c_str());
    fitsfile* f = nullptr;
    int status = 0;
    ffinit(&f, path.c_str(), &status);
    long naxes[2] = {32, 24};
    ffcrim(f, SHORT_IMG, 2, naxes, &status);  // 16-bit image, header only for us

    // A representative spread of real keywords, string/float/int/logical.
    char imagetyp[] = "LIGHT";
    ffpky(f, TSTRING, "IMAGETYP", imagetyp, "Frame type", &status);
    char dateobs[] = "2025-11-27T03:32:39.626";
    ffpky(f, TSTRING, "DATE-OBS", dateobs, "UTC start", &status);
    char instrume[] = "ZWO ASI6200MC Pro";
    ffpky(f, TSTRING, "INSTRUME", instrume, "Camera", &status);
    char object[] = "NGC 6960";
    ffpky(f, TSTRING, "OBJECT", object, "Target", &status);
    double exptime = 300.0;
    ffpky(f, TDOUBLE, "EXPTIME", &exptime, "Exposure seconds", &status);
    int gain = 100;
    ffpky(f, TINT, "GAIN", &gain, "Camera gain", &status);
    int xbin = 1;
    ffpky(f, TINT, "XBINNING", &xbin, "X binning", &status);
    int logical = 1;
    ffpky(f, TLOGICAL, "SIMPLE2", &logical, "a logical card", &status);
    // A value with an embedded apostrophe, to exercise '' unescaping.
    char obs[] = "O'Brien";
    ffpky(f, TSTRING, "OBSERVER", obs, "Observer", &status);
    ffpcom(f, "a plain comment card", &status);

    ffclos(f, &status);
    if (status) { std::cerr << "synthetic FITS write failed, status " << status << "\n"; std::exit(2); }
    return path;
}

void test_synthetic() {
    const std::string path = write_synthetic();
    fits::RawHeader h = fits::read_header(path);

    check(!h.hdus.empty(), "at least one HDU");
    const auto imgs = h.image_hdus();
    check(imgs.size() == 1, "one image HDU");
    if (!imgs.empty()) {
        const fits::Hdu& hdu = *imgs[0];
        check(hdu.index == 0, "primary HDU index 0");
        check(hdu.is_image, "primary is an image");
        check(hdu.naxis == 2, "NAXIS 2");
        check(hdu.naxis1 == 32 && hdu.naxis2 == 24, "NAXIS1/2 geometry");

        // Value decoding across types.
        check(hdu.get("IMAGETYP").value_or("") == "LIGHT", "string value");
        check(hdu.get("INSTRUME").value_or("") == "ZWO ASI6200MC Pro", "value with spaces");
        check(hdu.get("EXPTIME").value_or("").rfind("300", 0) == 0, "float value");
        check(hdu.get("GAIN").value_or("") == "100", "int value");
        // Embedded apostrophe survives '' unescaping.
        check(hdu.get("OBSERVER").value_or("") == "O'Brien", "escaped quote in value");
        // Case-insensitive lookup.
        check(hdu.get("imagetyp").has_value(), "case-insensitive keyword lookup");
        check(!hdu.get("NOSUCHKEY").has_value(), "absent keyword returns nullopt");
    }
    std::remove(path.c_str());
}

void test_fingerprint_stability() {
    // Same identity cards -> same fingerprint; a changed identity card -> a
    // different one; a changed NON-identity card -> the SAME fingerprint (this
    // is the whole point: SQM/FILTER stamping must not re-key a frame).
    const std::string p1 = write_synthetic();
    fits::RawHeader a = fits::read_header(p1);
    fits::RawHeader b = fits::read_header(p1);
    const auto fa = fits::fingerprint(*a.image_hdus()[0]);
    const auto fb = fits::fingerprint(*b.image_hdus()[0]);
    check(fa == fb, "fingerprint is deterministic");
    check(fits::to_hex(fa).size() == 32, "hex fingerprint is 32 chars");
    std::remove(p1.c_str());

    // Mutate a non-identity card in memory and confirm the fingerprint holds.
    fits::Hdu mutated = *a.image_hdus()[0];
    for (auto& c : mutated.cards)
        if (c.keyword == "OBJECT") c.value = "M31";  // not an identity field
    check(fits::fingerprint(mutated) == fa, "non-identity change keeps fingerprint");

    // Mutate an identity card and confirm it changes.
    fits::Hdu reexposed = *a.image_hdus()[0];
    for (auto& c : reexposed.cards)
        if (c.keyword == "EXPTIME") c.value = "600.0";  // identity field
    check(fits::fingerprint(reexposed) != fa, "identity change alters fingerprint");
}

void test_missing_file() {
    bool threw = false;
    try { fits::read_header("/nonexistent/frame.fits"); }
    catch (const fits::FitsError&) { threw = true; }
    check(threw, "missing file throws FitsError");
}

// Optional: exercise a real archive frame end to end.
void test_real_frame() {
    const char* path = std::getenv("SB_TEST_FITS");
    if (!path || !*path) {
        std::cout << "  (SB_TEST_FITS not set; skipping real-frame test)\n";
        return;
    }
    fits::RawHeader h = fits::read_header(path);
    const auto imgs = h.image_hdus();
    check(!imgs.empty(), "real frame has an image HDU");
    if (!imgs.empty()) {
        const fits::Hdu& hdu = *imgs[0];
        // Every field the fingerprint depends on should be present in a real
        // NINA/TheSky/ASIAIR frame (100% coverage in the archive survey).
        check(hdu.has("DATE-OBS"), "real frame has DATE-OBS");
        check(hdu.has("INSTRUME"), "real frame has INSTRUME");
        check(hdu.naxis1 > 0 && hdu.naxis2 > 0, "real frame has geometry");
        std::cout << "  real frame: " << hdu.naxis1 << "x" << hdu.naxis2
                  << ", " << hdu.cards.size() << " cards, IMAGETYP="
                  << hdu.get("IMAGETYP").value_or("?")
                  << ", fingerprint=" << fits::to_hex(fits::fingerprint(hdu)) << "\n";
    }
}

}  // namespace

int main() {
    test_synthetic();
    test_fingerprint_stability();
    test_missing_file();
    test_real_frame();

    if (g_failures == 0) { std::cout << "fits_test: all checks passed\n"; return 0; }
    std::cerr << "fits_test: " << g_failures << " check(s) failed\n";
    return 1;
}
