// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/mapping_loader.cpp
// Purpose:       Implementation of the database-to-HeaderMapping loader.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "mapping_loader.hpp"

namespace starbase::index {
namespace {

extract::ValueRule::Mode parse_mode(const std::string& s) {
    if (s == "contains") return extract::ValueRule::Mode::Contains;
    if (s == "regex")    return extract::ValueRule::Mode::Regex;
    return extract::ValueRule::Mode::Exact;
}

}  // namespace

extract::HeaderMapping load_mapping(db::Database& db) {
    extract::HeaderMapping m;

    // Keywords, already ordered so the first present candidate wins.
    for (const auto& row : db.query(
             "SELECT field, keyword FROM header_map "
             "WHERE enabled = 1 ORDER BY field, priority, id")) {
        if (row.size() < 2 || !row[0] || !row[1]) continue;
        m.keywords[*row[0]].push_back(*row[1]);
    }

    // Value rules, ordered so the first matching rule wins.
    for (const auto& row : db.query(
             "SELECT field, raw_value, normalized, match_mode "
             "FROM header_value_map ORDER BY field, priority, id")) {
        if (row.size() < 4 || !row[0] || !row[1] || !row[2]) continue;
        extract::ValueRule rule;
        rule.raw = *row[1];
        rule.normalized = *row[2];
        rule.mode = parse_mode(row[3].value_or("exact"));
        m.values[*row[0]].push_back(std::move(rule));
    }

    // A database that has not been seeded yields nothing useful; fall back to
    // the compiled-in defaults so the resolver still works out of the box.
    if (m.keywords.empty()) return extract::HeaderMapping::defaults();
    return m;
}

}  // namespace starbase::index
