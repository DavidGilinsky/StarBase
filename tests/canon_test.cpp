// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/canon_test.cpp
// Purpose:       Tests for offline target-name canonicalization: catalog
//                spacing, separators, idempotence, placeholders, and leaving
//                unrecognized names untouched. Pure; no database.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <iostream>
#include <string>

#include "canon.hpp"

namespace {
namespace n = starbase::names;
int g_failures = 0;
void eq(const std::string& raw, const std::string& want) {
    const auto c = n::canonicalize(raw);
    if (c.canonical != want) {
        std::cerr << "FAIL: '" << raw << "' -> '" << c.canonical << "', want '" << want << "'\n";
        ++g_failures;
    }
}
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
}  // namespace

int main() {
    // ---- catalog spacing and separators ----
    eq("M31", "M 31");
    eq("M 31", "M 31");            // idempotent
    eq("M101", "M 101");
    eq("m 13", "M 13");           // case-folded acronym
    eq("Messier 42", "M 42");
    eq("NGC7000", "NGC 7000");
    eq("NGC_7000", "NGC 7000");
    eq("ngc-7000", "NGC 7000");
    eq("NGC 7000", "NGC 7000");
    eq("ic434", "IC 434");
    eq("IC 434", "IC 434");
    eq("Sh2-199", "Sh2-199");     // idempotent, dash separator
    eq("sh2 199", "Sh2-199");
    eq("Sharpless 199", "Sh2-199");
    eq("Abell 2634", "Abell 2634");
    eq("aco2634", "Abell 2634");
    eq("b33", "B 33");            // Barnard
    eq("barnard 33", "B 33");
    eq("ngc7000a", "NGC 7000A");  // trailing component letter, uppercased

    // ---- catalog reported, changed flag ----
    {
        auto c = n::canonicalize("m31");
        check(c.catalog == "M", "catalog acronym reported");
        check(c.changed, "M31 is a change");
    }
    check(!n::canonicalize("M 31").changed, "already-canonical is not a change");

    // ---- names we must NOT mangle ----
    eq("Icarus", "Icarus");           // 'ic' must not swallow a word
    eq("Barnard's Galaxy", "Barnard's Galaxy");
    eq("North America Nebula", "North America Nebula");
    eq("SAC Abell 2634", "SAC Abell 2634");  // prefix not at the start
    eq("vdB 142", "vdB 142");
    eq("  NGC   6960 ", "NGC 6960");   // trimmed and collapsed

    // ---- placeholders ----
    check(n::canonicalize("Target").placeholder, "'Target' is a placeholder");
    check(n::canonicalize("FlatWizard").placeholder, "'FlatWizard' is a placeholder");
    check(n::canonicalize("").placeholder, "empty is a placeholder");
    check(!n::canonicalize("M 31").placeholder, "a real object is not a placeholder");

    if (g_failures == 0) { std::cout << "canon_test: all checks passed\n"; return 0; }
    std::cerr << "canon_test: " << g_failures << " check(s) failed\n";
    return 1;
}
