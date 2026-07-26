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
#include <unordered_map>
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

// Messier <-> NGC/IC cross-reference. Lets a canonical form and a search move
// between the catalogs (NGC 224 <-> M 31). Objects with no NGC/IC designation
// (M40 = Winnecke 4, M45 = Melotte 22) are deliberately absent: there is no
// equivalence to offer, so they stay "M 40" / "M 45".
struct Xref { int m; const char* cat; const char* num; };
const std::vector<Xref>& messier_xref() {
    static const std::vector<Xref> x = {
        {1,"NGC","1952"},  {2,"NGC","7089"},  {3,"NGC","5272"},  {4,"NGC","6121"},
        {5,"NGC","5904"},  {6,"NGC","6405"},  {7,"NGC","6475"},  {8,"NGC","6523"},
        {9,"NGC","6333"},  {10,"NGC","6254"}, {11,"NGC","6705"}, {12,"NGC","6218"},
        {13,"NGC","6205"}, {14,"NGC","6402"}, {15,"NGC","7078"}, {16,"NGC","6611"},
        {17,"NGC","6618"}, {18,"NGC","6613"}, {19,"NGC","6273"}, {20,"NGC","6514"},
        {21,"NGC","6531"}, {22,"NGC","6656"}, {23,"NGC","6494"}, {24,"IC","4715"},
        {25,"IC","4725"},  {26,"NGC","6694"}, {27,"NGC","6853"}, {28,"NGC","6626"},
        {29,"NGC","6913"}, {30,"NGC","7099"}, {31,"NGC","224"},  {32,"NGC","221"},
        {33,"NGC","598"},  {34,"NGC","1039"}, {35,"NGC","2168"}, {36,"NGC","1960"},
        {37,"NGC","2099"}, {38,"NGC","1912"}, {39,"NGC","7092"}, {41,"NGC","2287"},
        {42,"NGC","1976"}, {43,"NGC","1982"}, {44,"NGC","2632"}, {46,"NGC","2437"},
        {47,"NGC","2422"}, {48,"NGC","2548"}, {49,"NGC","4472"}, {50,"NGC","2323"},
        {51,"NGC","5194"}, {52,"NGC","7654"}, {53,"NGC","5024"}, {54,"NGC","6715"},
        {55,"NGC","6809"}, {56,"NGC","6779"}, {57,"NGC","6720"}, {58,"NGC","4579"},
        {59,"NGC","4621"}, {60,"NGC","4649"}, {61,"NGC","4303"}, {62,"NGC","6266"},
        {63,"NGC","5055"}, {64,"NGC","4826"}, {65,"NGC","3623"}, {66,"NGC","3627"},
        {67,"NGC","2682"}, {68,"NGC","4590"}, {69,"NGC","6637"}, {70,"NGC","6681"},
        {71,"NGC","6838"}, {72,"NGC","6981"}, {73,"NGC","6994"}, {74,"NGC","628"},
        {75,"NGC","6864"}, {76,"NGC","650"},  {77,"NGC","1068"}, {78,"NGC","2068"},
        {79,"NGC","1904"}, {80,"NGC","6093"}, {81,"NGC","3031"}, {82,"NGC","3034"},
        {83,"NGC","5236"}, {84,"NGC","4374"}, {85,"NGC","4382"}, {86,"NGC","4406"},
        {87,"NGC","4486"}, {88,"NGC","4501"}, {89,"NGC","4552"}, {90,"NGC","4569"},
        {91,"NGC","4548"}, {92,"NGC","6341"}, {93,"NGC","2447"}, {94,"NGC","4736"},
        {95,"NGC","3351"}, {96,"NGC","3368"}, {97,"NGC","3587"}, {98,"NGC","4192"},
        {99,"NGC","4254"}, {100,"NGC","4321"},{101,"NGC","5457"},{102,"NGC","5866"},
        {103,"NGC","581"}, {104,"NGC","4594"},{105,"NGC","3379"},{106,"NGC","4258"},
        {107,"NGC","6171"},{108,"NGC","3556"},{109,"NGC","3992"},{110,"NGC","205"},
    };
    return x;
}

// "M 31" -> "NGC 224"
const std::unordered_map<std::string, std::string>& m_to_alt() {
    static const auto t = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& e : messier_xref())
            m["M " + std::to_string(e.m)] = std::string(e.cat) + " " + e.num;
        return m;
    }();
    return t;
}

// "NGC 224" -> "M 31"
const std::unordered_map<std::string, std::string>& alt_to_m() {
    static const auto t = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& e : messier_xref())
            m[std::string(e.cat) + " " + e.num] = "M " + std::to_string(e.m);
        return m;
    }();
    return t;
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
            // Prefer the Messier designation when one exists (NGC 224 -> M 31),
            // so an object with both names collapses to a single canonical form.
            if (c.catalog == "NGC" || c.catalog == "IC") {
                const auto it = alt_to_m().find(c.canonical);
                if (it != alt_to_m().end()) { c.canonical = it->second; c.catalog = "M"; }
            }
            c.changed = (c.canonical != s);
            return c;
        }
    }
    return c;  // unrecognized: leave as tidied input
}

std::vector<std::string> designations(const std::string& raw) {
    const Canon c = canonicalize(raw);
    std::vector<std::string> out{c.canonical};
    // canonicalize() already collapses NGC/IC -> M, so a Messier canonical with
    // a cross-reference yields both designations from either input spelling.
    if (c.catalog == "M") {
        const auto it = m_to_alt().find(c.canonical);
        if (it != m_to_alt().end()) out.push_back(it->second);
    }
    return out;
}

}  // namespace starbase::names
