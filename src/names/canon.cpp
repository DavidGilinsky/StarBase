// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/names/canon.cpp
// Purpose:       Implementation of offline target-name canonicalization.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "canon.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace starbase::names {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Trim ends and collapse internal runs of whitespace to a single space.
std::string tidy(const std::string& in) {
    std::string s;
    bool sp = false;
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) { sp = !s.empty(); continue; }
        if (sp) { s += ' '; sp = false; }
        s += c;
    }
    return s;
}

// One catalog: the acronym StarBase emits, the separator between acronym and
// number, and the aliases (lowercase) that map to it.
struct Rule {
    const char* acronym;
    char sep;
    std::vector<const char*> aliases;
};

const std::vector<Rule>& rules() {
    static const std::vector<Rule> r = {
        {"M",     ' ', {"messier", "m"}},
        {"NGC",   ' ', {"ngc"}},
        {"IC",    ' ', {"ic"}},
        {"UGC",   ' ', {"ugc"}},
        {"PGC",   ' ', {"pgc"}},
        {"Arp",   ' ', {"arp"}},
        {"Abell", ' ', {"abell", "aco"}},
        {"Sh2",   '-', {"sharpless", "sh2", "sh"}},
        {"LBN",   ' ', {"lbn"}},
        {"LDN",   ' ', {"ldn"}},
        {"Cr",    ' ', {"collinder", "cr"}},
        {"Mel",   ' ', {"melotte", "mel"}},
        {"vdB",   ' ', {"vdb"}},
        {"Ced",   ' ', {"cederblad", "ced"}},
        {"C",     ' ', {"caldwell"}},
        {"B",     ' ', {"barnard", "b"}},
    };
    return r;
}

const std::array<const char*, 14> kPlaceholders = {
    "", "target", "none", "unknown", "n/a", "na", "test",
    "flatwizard", "flat", "dark", "bias", "object", "no target", "-"};

bool is_placeholder(const std::string& low) {
    for (const char* p : kPlaceholders) if (low == p) return true;
    return false;
}

// The remainder after an alias must be a catalog number: digits, optionally one
// trailing letter (NGC 7000A). Returns the normalized number (letter uppercased)
// or empty if it is not a clean number.
std::string catalog_number(const std::string& rest) {
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '_' || rest[i] == '-')) ++i;
    const std::string num = rest.substr(i);
    if (num.empty() || !std::isdigit(static_cast<unsigned char>(num[0]))) return {};
    size_t d = 0;
    while (d < num.size() && std::isdigit(static_cast<unsigned char>(num[d]))) ++d;
    if (d == 0) return {};
    if (d == num.size()) return num;                      // all digits
    if (d == num.size() - 1 && std::isalpha(static_cast<unsigned char>(num[d])))
        return num.substr(0, d) + static_cast<char>(std::toupper(static_cast<unsigned char>(num[d])));
    return {};                                            // trailing junk: not a catalog number
}

bool istarts_with(const std::string& low, const char* prefix) {
    const std::string p = prefix;
    return low.size() >= p.size() && low.compare(0, p.size(), p) == 0;
}

}  // namespace

Canon canonicalize(const std::string& raw) {
    Canon c;
    const std::string s = tidy(raw);
    const std::string low = lower(s);
    c.canonical = s;

    if (is_placeholder(low)) { c.placeholder = true; return c; }

    for (const Rule& rule : rules()) {
        for (const char* alias : rule.aliases) {
            if (!istarts_with(low, alias)) continue;
            // The character right after the alias must be a separator or a digit,
            // never another letter, so "ic" does not swallow "icarus".
            const size_t n = std::string(alias).size();
            if (n < s.size()) {
                const char nx = s[n];
                if (std::isalpha(static_cast<unsigned char>(nx))) continue;
            }
            const std::string num = catalog_number(s.substr(n));
            if (num.empty()) continue;
            c.catalog = rule.acronym;
            c.canonical = std::string(rule.acronym) + rule.sep + num;
            c.changed = (c.canonical != s);
            return c;
        }
    }
    return c;  // unrecognized: leave as tidied input
}

}  // namespace starbase::names
