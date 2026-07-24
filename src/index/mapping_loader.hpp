// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/index/mapping_loader.hpp
// Purpose:       Load the header mapping (header_map, header_value_map) from the
//                database into the in-memory HeaderMapping the resolver uses, so
//                the mapping is driven by the editable database copy rather than
//                the compiled-in defaults.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include "database.hpp"
#include "resolver.hpp"

namespace starbase::index {

// Build a HeaderMapping from the database's header_map and header_value_map
// tables. Keywords and value rules come back in priority order (lowest first),
// matching the resolver's first-match-wins expectation. Disabled header_map
// rows are skipped. Throws db::DbError on a query failure.
//
// The database copy is authoritative; HeaderMapping::defaults() is the fallback
// when the tables are empty (a fresh database that has not been seeded).
extract::HeaderMapping load_mapping(db::Database& db);

}  // namespace starbase::index
