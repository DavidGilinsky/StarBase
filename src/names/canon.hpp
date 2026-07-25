// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/names/canon.hpp
// Purpose:       Offline canonicalization of astronomical target names to the
//                CDS/IAU written form (M31 -> "M 31", NGC_7000 -> "NGC 7000").
//                FITS itself prescribes no OBJECT-name syntax; this normalizes
//                the common catalog designations and flags non-object
//                placeholders. Pure: no database, no network.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase::names {

struct Canon {
    std::string canonical;    // normalized name (input trimmed if no rule applied)
    std::string catalog;      // matched acronym (e.g. "NGC", "M"), empty if none
    bool placeholder = false; // a non-object placeholder (Target, FlatWizard, ...)
    bool changed = false;     // canonical differs from the raw input
};

// Canonicalize a raw OBJECT value. Recognizes the common catalogs (M, NGC, IC,
// UGC, PGC, Arp, Abell, Sh2, LBN, LDN, Cr, Mel, vdB, Ced, B, Caldwell), inserts
// the standard acronym/number spacing, and leaves anything it does not
// recognize unchanged (never a hard failure). Placeholder values resolve with
// placeholder=true and the name unchanged.
Canon canonicalize(const std::string& raw);

}  // namespace starbase::names
