// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/fits/fits_reader.hpp
// Purpose:       FITS header extraction via CFITSIO. Reads every header card of
//                every HDU without ever touching pixel data, and computes the
//                immutable identity fingerprint used for move/duplicate
//                detection.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace starbase::fits {

// One header card: keyword, value (quotes/padding stripped), and comment.
struct Card {
    std::string keyword;
    std::string value;
    std::string comment;
};

// One Header/Data Unit. hdu index is 0-based (primary = 0), matching the
// frames.hdu column. Pixel data is never read; only the geometry that lives in
// the header (BITPIX, NAXIS, NAXISn) is captured.
struct Hdu {
    int  index = 0;
    bool is_image = false;
    int  bitpix = 0;
    int  naxis = 0;
    long naxis1 = 0;
    long naxis2 = 0;
    std::vector<Card> cards;

    // First value for a keyword (case-insensitive), or nullopt. HIERARCH and
    // duplicate keywords keep their first occurrence, matching FITS convention.
    std::optional<std::string> get(const std::string& keyword) const;
    bool has(const std::string& keyword) const { return get(keyword).has_value(); }
};

struct RawHeader {
    std::vector<Hdu> hdus;
    // Convenience: HDUs that carry an image (NAXIS > 0). These become frames;
    // a table HDU or an empty primary does not.
    std::vector<const Hdu*> image_hdus() const;
};

class FitsError : public std::runtime_error {
public:
    explicit FitsError(const std::string& what) : std::runtime_error(what) {}
};

// Open a FITS file and read all headers. Never reads pixel data. Handles gzip
// (.gz) and Rice-tile (.fz) transparently via CFITSIO. Throws FitsError on an
// unreadable or truncated file, naming the CFITSIO status.
RawHeader read_header(const std::string& path);

// Immutable identity fingerprint of one HDU: MD5 over the header cards that
// cannot change for a given exposure --
//   DATE-OBS | INSTRUME | EXPTIME | NAXIS1 | NAXIS2 | IMAGETYP | XBINNING
// Deliberately NOT a whole-file hash: nightwatcher-ingest stamps SQM and a
// defaulted FILTER into frames after filing, and hashing terabytes over NFS on
// every sweep is not viable. Missing fields contribute an empty token, so the
// fingerprint is stable whether or not an optional card is present.
std::array<unsigned char, 16> fingerprint(const Hdu& hdu);

// Lowercase hex of a 16-byte fingerprint (for logging and SQL UNHEX round-trip).
std::string to_hex(const std::array<unsigned char, 16>& digest);

}  // namespace starbase::fits
