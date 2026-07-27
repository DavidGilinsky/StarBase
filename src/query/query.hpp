// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/query/query.hpp
// Purpose:       Compile a JSON filter AST into a parameterized SQL WHERE clause
//                over v_frames: comparisons and ranges on promoted columns, set
//                membership, arbitrary header-keyword predicates (compiled to an
//                EXISTS against frame_keywords), and cone search. User input
//                never reaches SQL unescaped, and only allowlisted fields and
//                operators compile.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "database.hpp"

namespace starbase::query {

// Thrown for a malformed AST, an unknown field, or an unsupported operator, so
// the API can answer 400 rather than 500.
class QueryError : public std::runtime_error {
public:
    explicit QueryError(const std::string& what) : std::runtime_error(what) {}
};

// Compile a filter AST to a SQL boolean expression over v_frames (referenced
// unqualified; callers run `... FROM v_frames WHERE <result>`). An empty or null
// AST compiles to "1=1" (match all). `db` supplies value escaping. Throws
// QueryError on anything invalid.
//
// Node shapes:
//   {"op":"and"|"or", "clauses":[ ... ]}         logical group
//   {"op":"not", "clauses":[ node ]}             negation
//   {"field":F, "op":"eq|ne|lt|lte|gt|gte|like", "value":v}
//   {"field":F, "op":"in", "value":[ ... ]}       set membership
//   {"field":F, "op":"between", "value":[lo,hi]}  inclusive range
//   {"field":F, "op":"isnull|notnull"}
//   {"field":"keyword:KW", "op":..., "value":v}   header-card predicate
//   {"op":"cone", "ra":deg, "dec":deg, "radius_deg":r}
std::string compile_filter(const nlohmann::json& ast, db::Database& db);

// Compile an optional sort spec to an ORDER BY body (without the keyword), e.g.
//   [{"field":"date_obs_utc","dir":"desc"}, {"field":"exposure_s"}]
// Defaults to "date_obs_utc DESC" when empty. Only allowlisted fields sort.
std::string compile_sort(const nlohmann::json& sort);

// The distinct promoted fields a filter AST references, in first-seen order, so a
// caller can surface exactly what was queried (e.g. as result columns). Skips
// logical combinators and cone/tag/collection nodes, and returns only names in
// the field allowlist, so each is a safe v_frames column identifier.
std::vector<std::string> filter_fields(const nlohmann::json& ast);

}  // namespace starbase::query
