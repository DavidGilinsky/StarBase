// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/fits/xisf_reader.cpp
// Purpose:       Implementation of the XISF header extractor.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// The XISF monolithic layout is fixed and small: an 8-byte signature
// ("XISF0100"), a little-endian uint32 header length, 4 reserved bytes, then an
// XML header of that length. We only need the <Image> element attributes and
// its <FITSKeyword> children, so a focused scanner over that XML is enough and
// keeps the third-party surface to just httplib + nlohmann. No PixInsight code
// is linked or vendored; we read the public spec's bytes ourselves.
// ---------------------------------------------------------------------------
#include "xisf_reader.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace starbase::fits {
namespace {

constexpr char kSignature[] = "XISF0100";
// A malformed length must not make us try to allocate gigabytes.
constexpr std::uint32_t kMaxHeaderBytes = 64u * 1024u * 1024u;

// Decode the five predefined XML entities. XISF attribute values are otherwise
// literal, so this is all the entity handling the header needs.
std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0)  { out += '<'; i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0)  { out += '>'; i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
        }
        out += s[i++];
    }
    return out;
}

// Strip the FITS string-literal wrapping XISF preserves in FITSKeyword values:
// a leading and trailing single quote, doubled internal quotes ('' -> '), and
// trailing padding. Numeric values (no wrapping quotes) pass through trimmed.
std::string strip_fits_value(const std::string& raw) {
    std::string s = raw;
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        s = s.substr(1, s.size() - 2);
        // Collapse the FITS-doubled single quote.
        std::string t;
        for (size_t i = 0; i < s.size(); ++i) {
            t += s[i];
            if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') ++i;
        }
        s = t;
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

// Read the value of attribute `name` from an element's attribute text (the span
// between the tag name and its closing '>' or '/>'). Handles single- or
// double-quoted values. Returns nullopt if the attribute is absent.
bool attr(const std::string& elem, const std::string& name, std::string& out) {
    size_t pos = 0;
    while ((pos = elem.find(name, pos)) != std::string::npos) {
        // Require a word boundary before the name and '=' (with optional space)
        // after it, so "name" does not match inside another attribute.
        const bool left_ok = (pos == 0) ||
            (!std::isalnum(static_cast<unsigned char>(elem[pos - 1])) &&
             elem[pos - 1] != '_' && elem[pos - 1] != '-' && elem[pos - 1] != ':');
        size_t q = pos + name.size();
        while (q < elem.size() && std::isspace(static_cast<unsigned char>(elem[q]))) ++q;
        if (left_ok && q < elem.size() && elem[q] == '=') {
            ++q;
            while (q < elem.size() && std::isspace(static_cast<unsigned char>(elem[q]))) ++q;
            if (q < elem.size() && (elem[q] == '"' || elem[q] == '\'')) {
                const char quote = elem[q++];
                const size_t end = elem.find(quote, q);
                if (end != std::string::npos) {
                    out = xml_unescape(elem.substr(q, end - q));
                    return true;
                }
            }
        }
        pos += name.size();
    }
    return false;
}

// sampleFormat -> FITS BITPIX. Only used for display; the fingerprint never
// depends on it, so a best-effort mapping is fine.
int bitpix_for(const std::string& fmt) {
    if (fmt == "UInt8") return 8;
    if (fmt == "UInt16") return 16;
    if (fmt == "UInt32") return 32;
    if (fmt == "UInt64") return 64;
    if (fmt == "Float32" || fmt == "Complex32") return -32;
    if (fmt == "Float64" || fmt == "Complex64") return -64;
    return 0;
}

// Remove XML comment spans so a scan for element tags never matches inside a
// <!-- ... --> block (XISF prefaces the root with a provenance comment).
std::string strip_comments(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    for (size_t i = 0; i < xml.size();) {
        if (xml.compare(i, 4, "<!--") == 0) {
            const size_t end = xml.find("-->", i + 4);
            if (end == std::string::npos) break;
            i = end + 3;
        } else {
            out += xml[i++];
        }
    }
    return out;
}

// Long is at least 32-bit; geometry dimensions fit comfortably.
long to_long(const std::string& s) {
    try { return std::stol(s); } catch (...) { return 0; }
}

}  // namespace

bool is_xisf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char sig[8];
    if (!in.read(sig, 8)) return false;
    for (int i = 0; i < 8; ++i) if (sig[i] != kSignature[i]) return false;
    return true;
}

RawHeader read_xisf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw FitsError("cannot open XISF file: " + path);

    unsigned char head[16];
    if (!in.read(reinterpret_cast<char*>(head), 16))
        throw FitsError("XISF file too short for a header: " + path);
    for (int i = 0; i < 8; ++i)
        if (head[i] != static_cast<unsigned char>(kSignature[i]))
            throw FitsError("not an XISF file (bad signature): " + path);

    const std::uint32_t hlen = static_cast<std::uint32_t>(head[8]) |
                               (static_cast<std::uint32_t>(head[9]) << 8) |
                               (static_cast<std::uint32_t>(head[10]) << 16) |
                               (static_cast<std::uint32_t>(head[11]) << 24);
    if (hlen == 0 || hlen > kMaxHeaderBytes)
        throw FitsError("XISF header length out of range: " + path);

    std::string xml(hlen, '\0');
    if (!in.read(&xml[0], hlen))
        throw FitsError("XISF header truncated: " + path);
    xml = strip_comments(xml);

    RawHeader out;
    int index = 0;
    // Walk each <Image ...> element and its <FITSKeyword .../> children up to
    // the matching </Image> (or the next <Image> for self-contained encoding).
    size_t pos = 0;
    while ((pos = xml.find("<Image", pos)) != std::string::npos) {
        // Guard against "<ImageXxx"; require a delimiter after the tag name.
        const char after = pos + 6 < xml.size() ? xml[pos + 6] : '>';
        if (!std::isspace(static_cast<unsigned char>(after)) && after != '>' && after != '/') {
            pos += 6;
            continue;
        }
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) break;
        const std::string open_tag = xml.substr(pos, tag_end - pos);

        Hdu hdu;
        hdu.index = index++;
        hdu.is_image = true;

        std::string geom, fmt;
        if (attr(open_tag, "geometry", geom)) {
            std::vector<long> dims;
            size_t s = 0;
            while (s < geom.size()) {
                size_t c = geom.find(':', s);
                dims.push_back(to_long(geom.substr(s, c == std::string::npos ? c : c - s)));
                if (c == std::string::npos) break;
                s = c + 1;
            }
            if (dims.size() >= 1) hdu.naxis1 = dims[0];
            if (dims.size() >= 2) hdu.naxis2 = dims[1];
            // geometry is dim1:dim2:...:channels; >1 channel reads as 3-axis.
            const long channels = dims.empty() ? 0 : dims.back();
            hdu.naxis = (dims.size() >= 3 && channels > 1) ? 3 : 2;
        }
        if (attr(open_tag, "sampleFormat", fmt)) hdu.bitpix = bitpix_for(fmt);

        // Synthesize the geometry cards FITS would carry, so the fingerprint and
        // any NAXIS-based logic behave identically for both formats.
        hdu.cards.push_back({"NAXIS", std::to_string(hdu.naxis), "number of axes (XISF)"});
        hdu.cards.push_back({"NAXIS1", std::to_string(hdu.naxis1), "axis length (XISF geometry)"});
        hdu.cards.push_back({"NAXIS2", std::to_string(hdu.naxis2), "axis length (XISF geometry)"});

        // The scope of this image's keywords ends at </Image> or the next <Image>.
        const size_t close = xml.find("</Image>", tag_end);
        const size_t next_img = xml.find("<Image", tag_end);
        size_t scope_end = xml.size();
        if (close != std::string::npos) scope_end = close;
        if (next_img != std::string::npos && next_img < scope_end) scope_end = next_img;

        size_t k = tag_end;
        while ((k = xml.find("<FITSKeyword", k)) != std::string::npos && k < scope_end) {
            const size_t ke = xml.find('>', k);
            if (ke == std::string::npos) break;
            const std::string kw_elem = xml.substr(k, ke - k);
            std::string name, value, comment;
            attr(kw_elem, "name", name);
            attr(kw_elem, "value", value);
            attr(kw_elem, "comment", comment);
            if (!name.empty())
                hdu.cards.push_back({name, strip_fits_value(value), comment});
            k = ke;
        }

        out.hdus.push_back(std::move(hdu));
        pos = tag_end;
    }

    if (out.hdus.empty())
        throw FitsError("XISF header contains no <Image> element: " + path);
    return out;
}

}  // namespace starbase::fits
