// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/names/sesame.cpp
// Purpose:       CDS Sesame client (text -oI format) over HTTPS.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "sesame.hpp"

#include <httplib.h>

#include <cctype>
#include <sstream>
#include <string>

namespace starbase::names {
namespace {

// Percent-encode a query value (RFC 3986 unreserved kept, everything else %XX).
std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0f]; }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Trim and collapse internal whitespace runs (Sesame writes "M  31").
std::string collapse(const std::string& in) {
    std::string s;
    bool sp = false;
    for (char c : trim(in)) {
        if (c == ' ' || c == '\t') { sp = true; continue; }
        if (sp && !s.empty()) s += ' ';
        sp = false;
        s += c;
    }
    return s;
}

// Parse the Sesame "-oI" text body. Relevant lines:
//   %J <ra_deg> <dec_deg> ...   -> J2000 position in decimal degrees
//   %I.0 <identifier>           -> the main identifier (first one wins)
//   %C.0 <otype>                -> object type
// The first resolver that yields an identifier or a position wins.
SesameResult parse_oI(const std::string& body) {
    SesameResult r;
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.rfind("%J ", 0) == 0 && !r.has_coords) {
            std::istringstream ls(line.substr(3));
            double ra = 0, dec = 0;
            if (ls >> ra >> dec) { r.ra_deg = ra; r.dec_deg = dec; r.has_coords = true; }
        } else if (line.rfind("%I", 0) == 0 && r.name.empty()) {
            const size_t sp = line.find(' ');
            if (sp != std::string::npos) r.name = collapse(line.substr(sp + 1));
        } else if (line.rfind("%C", 0) == 0 && r.otype.empty()) {
            const size_t sp = line.find(' ');
            if (sp != std::string::npos) r.otype = trim(line.substr(sp + 1));
        }
    }
    r.ok = !r.name.empty() || r.has_coords;
    return r;
}

}  // namespace

SesameResult sesame_resolve(const std::string& raw, const std::string& base_url, int timeout_s) {
    SesameResult r;
    try {
        httplib::Client cli(base_url);
        cli.set_connection_timeout(timeout_s, 0);
        cli.set_read_timeout(timeout_s, 0);
        cli.set_follow_location(true);
        // -oI: info text; SNV: try SIMBAD, then NED, then VizieR.
        const std::string path = "/cgi-bin/nph-sesame/-oI/SNV?" + url_encode(raw);
        auto res = cli.Get(path.c_str());
        if (!res) { r.error = "network error contacting Sesame"; return r; }
        if (res->status != 200) { r.error = "Sesame returned HTTP " + std::to_string(res->status); return r; }
        r = parse_oI(res->body);
        if (!r.ok) r.error = "no match";
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
}

}  // namespace starbase::names
