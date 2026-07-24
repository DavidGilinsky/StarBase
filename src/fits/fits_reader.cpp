// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/fits/fits_reader.cpp
// Purpose:       CFITSIO implementation of the FITS header reader and the
//                identity fingerprint.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "fits_reader.hpp"

#include <fitsio.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>

namespace starbase::fits {
namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string rstrip(const std::string& s) {
    const auto e = s.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

// CFITSIO returns a string value with its enclosing quotes and FITS padding, a
// number as text, or a logical as T/F. Strip the quotes and trailing blanks
// that FITS pads with; a doubled '' inside a quoted string is one literal quote.
std::string clean_value(const char* raw) {
    if (!raw) return {};
    std::string v(raw);
    if (!v.empty() && v.front() == '\'') {
        std::string out;
        // Drop the opening quote; a closing quote ends the string, but '' is an
        // escaped single quote and continues it.
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i] == '\'') {
                if (i + 1 < v.size() && v[i + 1] == '\'') { out.push_back('\''); ++i; }
                else break;  // closing quote
            } else {
                out.push_back(v[i]);
            }
        }
        return rstrip(out);
    }
    return rstrip(v);
}

std::string cfitsio_error(int status) {
    char msg[FLEN_ERRMSG] = {0};
    ffgerr(status, msg);
    return std::string(msg) + " (status " + std::to_string(status) + ")";
}

// RAII for the fitsfile* so every early return closes the handle.
struct FitsHandle {
    fitsfile* fptr = nullptr;
    ~FitsHandle() {
        if (fptr) { int s = 0; ffclos(fptr, &s); }
    }
};

}  // namespace

std::optional<std::string> Hdu::get(const std::string& keyword) const {
    const std::string want = upper(keyword);
    for (const auto& c : cards) {
        if (upper(c.keyword) == want) return c.value;
    }
    return std::nullopt;
}

std::vector<const Hdu*> RawHeader::image_hdus() const {
    std::vector<const Hdu*> out;
    for (const auto& h : hdus)
        if (h.is_image && h.naxis > 0) out.push_back(&h);
    return out;
}

RawHeader read_header(const std::string& path) {
    FitsHandle fh;
    int status = 0;

    // ffdkopn opens a literal disk file, bypassing CFITSIO's extended-filename
    // syntax so a path containing '[' or '(' is never misparsed. READONLY, and
    // no data block is read until we ask for pixels -- which we never do.
    if (ffdkopn(&fh.fptr, path.c_str(), READONLY, &status) != 0)
        throw FitsError("cannot open " + path + ": " + cfitsio_error(status));

    int nhdu = 0;
    if (ffthdu(fh.fptr, &nhdu, &status) != 0)
        throw FitsError("cannot count HDUs in " + path + ": " + cfitsio_error(status));

    RawHeader header;
    header.hdus.reserve(static_cast<size_t>(nhdu));

    for (int i = 1; i <= nhdu; ++i) {
        int hdutype = 0;
        if (ffmahd(fh.fptr, i, &hdutype, &status) != 0)
            throw FitsError("cannot move to HDU " + std::to_string(i) + " in " +
                            path + ": " + cfitsio_error(status));

        Hdu hdu;
        hdu.index = i - 1;  // 0-based to match frames.hdu
        hdu.is_image = (hdutype == IMAGE_HDU);

        if (hdu.is_image) {
            int bitpix = 0, naxis = 0;
            long naxes[9] = {0};
            if (ffgipr(fh.fptr, 9, &bitpix, &naxis, naxes, &status) != 0)
                throw FitsError("cannot read image params of HDU " +
                                std::to_string(i) + " in " + path + ": " +
                                cfitsio_error(status));
            hdu.bitpix = bitpix;
            hdu.naxis = naxis;
            if (naxis >= 1) hdu.naxis1 = naxes[0];
            if (naxis >= 2) hdu.naxis2 = naxes[1];
        }

        int nkeys = 0, morekeys = 0;
        if (ffghsp(fh.fptr, &nkeys, &morekeys, &status) != 0)
            throw FitsError("cannot read header space of HDU " +
                            std::to_string(i) + " in " + path + ": " +
                            cfitsio_error(status));

        hdu.cards.reserve(static_cast<size_t>(nkeys));
        for (int k = 1; k <= nkeys; ++k) {
            char keyname[FLEN_KEYWORD] = {0};
            char keyval[FLEN_VALUE] = {0};
            char comment[FLEN_COMMENT] = {0};
            if (ffgkyn(fh.fptr, k, keyname, keyval, comment, &status) != 0)
                throw FitsError("cannot read card " + std::to_string(k) +
                                " of HDU " + std::to_string(i) + " in " + path +
                                ": " + cfitsio_error(status));
            Card card;
            card.keyword = rstrip(keyname);
            // END, and the blank keywords of COMMENT/HISTORY, have no value.
            card.value = clean_value(keyval);
            card.comment = rstrip(comment);
            if (card.keyword.empty() && card.value.empty() && card.comment.empty())
                continue;  // pure padding
            hdu.cards.push_back(std::move(card));
        }

        header.hdus.push_back(std::move(hdu));
    }

    return header;
}

std::array<unsigned char, 16> fingerprint(const Hdu& hdu) {
    // Order and separator are part of the contract; changing them re-keys every
    // frame in the index, so they must stay stable across versions.
    static const char* kFields[] = {"DATE-OBS", "INSTRUME", "EXPTIME",
                                    "NAXIS1",   "NAXIS2",   "IMAGETYP",
                                    "XBINNING"};
    std::string material;
    for (const char* f : kFields) {
        if (auto v = hdu.get(f)) material += *v;
        material.push_back('|');
    }

    std::array<unsigned char, 16> digest{};
    unsigned int len = 0;
    // EVP rather than the deprecated MD5() call. MD5 is fine here: the input is
    // never adversarial, we only need to tell identical exposures apart, and the
    // 16-byte output matches the frames.fingerprint BINARY(16) column.
    EVP_Digest(material.data(), material.size(), digest.data(), &len, EVP_md5(),
               nullptr);
    return digest;
}

std::string to_hex(const std::array<unsigned char, 16>& digest) {
    static const char* h = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : digest) {
        out.push_back(h[b >> 4]);
        out.push_back(h[b & 0x0f]);
    }
    return out;
}

}  // namespace starbase::fits
