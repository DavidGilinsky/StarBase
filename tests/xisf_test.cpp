// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/xisf_test.cpp
// Purpose:       Tests for the XISF header extractor: signature dispatch,
//                geometry -> NAXIS, FITSKeyword extraction with FITS-quote
//                stripping and XML entity decoding, multi-image files, and
//                rejection of malformed input. Builds its own XISF bytes; no
//                archive and no database required.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "fits_reader.hpp"
#include "xisf_reader.hpp"

namespace {

namespace fits = starbase::fits;
namespace stdfs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

// Wrap an XML header in the monolithic XISF envelope and write it to `path`.
void write_xisf(const stdfs::path& path, const std::string& xml) {
    std::ofstream out(path, std::ios::binary);
    out.write("XISF0100", 8);
    const std::uint32_t hlen = static_cast<std::uint32_t>(xml.size());
    unsigned char len[4] = {static_cast<unsigned char>(hlen & 0xff),
                            static_cast<unsigned char>((hlen >> 8) & 0xff),
                            static_cast<unsigned char>((hlen >> 16) & 0xff),
                            static_cast<unsigned char>((hlen >> 24) & 0xff)};
    out.write(reinterpret_cast<char*>(len), 4);
    const char reserved[4] = {0, 0, 0, 0};
    out.write(reserved, 4);
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
}

const char* kOneImage =
    "<?xml version=\"1.0\"?><!-- Created with <Image> in a comment, ignore me -->"
    "<xisf version=\"1.0\"><Image geometry=\"3072:2048:1\" sampleFormat=\"UInt16\" "
    "colorSpace=\"Gray\" location=\"attachment:4096:12582912\">"
    "<FITSKeyword name=\"IMAGETYP\" value=\"'Light Frame'\" comment=\"Type of image\"/>"
    "<FITSKeyword name=\"EXPTIME\" value=\"120.00\" comment=\"seconds\"/>"
    "<FITSKeyword name=\"INSTRUME\" value=\"'ZWO ASI6200MM &amp; cooler'\" comment=\"cam\"/>"
    "<FITSKeyword name=\"XBINNING\" value=\"1\" comment=\"bin\"/>"
    "<FITSKeyword name=\"DATE-OBS\" value=\"'2026-07-03T09:29:18.000'\" comment=\"utc\"/>"
    "<Property id=\"PCL:CFA:Pattern\" type=\"String\" value=\"RGGB\"/>"
    "</Image></xisf>";

const char* kTwoImages =
    "<xisf version=\"1.0\">"
    "<Image geometry=\"100:200:1\" sampleFormat=\"Float32\">"
    "<FITSKeyword name=\"IMAGETYP\" value=\"'Dark Frame'\" comment=\"\"/></Image>"
    "<Image geometry=\"640:480:3\" sampleFormat=\"UInt8\">"
    "<FITSKeyword name=\"IMAGETYP\" value=\"'Flat Frame'\" comment=\"\"/></Image>"
    "</xisf>";

}  // namespace

int main() {
    const stdfs::path base =
        stdfs::temp_directory_path() / ("xisf_" + std::to_string(::getpid()));
    stdfs::create_directories(base);

    try {
        // ---- single image: geometry, sample format, keyword extraction ----
        const stdfs::path f1 = base / "light.xisf";
        write_xisf(f1, kOneImage);

        check(fits::is_xisf(f1.string()), "signature detected");
        auto h = fits::read_xisf(f1.string());
        check(h.hdus.size() == 1, "one image HDU");
        const auto& hdu = h.hdus[0];
        check(hdu.naxis1 == 3072 && hdu.naxis2 == 2048, "geometry -> NAXIS1/NAXIS2");
        check(hdu.naxis == 2, "single channel is a 2-axis image");
        check(hdu.bitpix == 16, "UInt16 -> BITPIX 16");

        // FITS-quote stripping and case-insensitive lookup.
        check(hdu.get("IMAGETYP").value_or("") == "Light Frame", "quotes stripped from value");
        check(hdu.get("exptime").value_or("") == "120.00", "case-insensitive keyword lookup");
        check(hdu.get("XBINNING").value_or("") == "1", "numeric value preserved");
        // XML entity decoded inside a quoted value.
        check(hdu.get("INSTRUME").value_or("") == "ZWO ASI6200MM & cooler",
              "XML entity decoded, quotes stripped");
        // A <Property> is not a FITSKeyword and must not become a card.
        check(!hdu.has("PCL:CFA:Pattern"), "Property elements are not cards");
        // The <Image> mention inside the leading comment must be ignored.
        check(h.hdus.size() == 1, "commented-out <Image> ignored");

        // The synthesized geometry cards let the fingerprint work unchanged.
        check(hdu.get("NAXIS1").value_or("") == "3072", "NAXIS1 card synthesized");
        auto fp = fits::to_hex(fits::fingerprint(hdu));
        check(fp.size() == 32, "fingerprint computes over an XISF HDU");

        // ---- dispatch: read_header routes an XISF file to the XISF parser ----
        auto h2 = fits::read_header(f1.string());
        check(h2.hdus.size() == 1 && h2.hdus[0].naxis1 == 3072, "read_header dispatches XISF");

        // ---- two images -> two HDUs, each with its own keywords ----
        const stdfs::path f2 = base / "multi.xisf";
        write_xisf(f2, kTwoImages);
        auto hm = fits::read_xisf(f2.string());
        check(hm.hdus.size() == 2, "two <Image> elements -> two HDUs");
        check(hm.hdus[0].get("IMAGETYP").value_or("") == "Dark Frame", "first image keywords scoped");
        check(hm.hdus[1].get("IMAGETYP").value_or("") == "Flat Frame", "second image keywords scoped");
        check(hm.hdus[1].naxis == 3, "3-channel geometry is a 3-axis image");

        // ---- malformed input is rejected, not silently accepted ----
        const stdfs::path bad = base / "bad.bin";
        { std::ofstream o(bad, std::ios::binary); o << "NOTXISF0" << "garbage"; }
        check(!fits::is_xisf(bad.string()), "non-XISF signature rejected");
        bool threw = false;
        try { fits::read_xisf(bad.string()); } catch (const fits::FitsError&) { threw = true; }
        check(threw, "read_xisf throws on a bad signature");

        // An XISF envelope with no <Image> is an error, not an empty success.
        const stdfs::path empty = base / "empty.xisf";
        write_xisf(empty, "<xisf version=\"1.0\"></xisf>");
        threw = false;
        try { fits::read_xisf(empty.string()); } catch (const fits::FitsError&) { threw = true; }
        check(threw, "read_xisf throws when there is no image");

    } catch (const std::exception& e) {
        std::cerr << "xisf_test: unexpected exception: " << e.what() << "\n";
        stdfs::remove_all(base);
        return 1;
    }
    stdfs::remove_all(base);

    if (g_failures == 0) { std::cout << "xisf_test: all checks passed\n"; return 0; }
    std::cerr << "xisf_test: " << g_failures << " check(s) failed\n";
    return 1;
}
