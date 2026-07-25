// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/password_test.cpp
// Purpose:       Tests for PBKDF2 password hashing: round-trip verify, wrong
//                password rejection, fresh salt per hash, and token randomness.
//                Pure; no database.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <iostream>
#include <string>

#include "password.hpp"

namespace {
namespace auth = starbase::auth;
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
}  // namespace

int main() {
    const auto h = auth::hash_password("Correct-Horse-Battery-9");
    check(h.iterations >= 100000, "iteration count is high");
    check(h.salt_hex.size() == 32, "salt is 16 bytes (32 hex)");
    check(h.hash_hex.size() == 64, "hash is 32 bytes (64 hex)");

    check(auth::verify_password("Correct-Horse-Battery-9", h.salt_hex, h.hash_hex, h.iterations),
          "correct password verifies");
    check(!auth::verify_password("wrong", h.salt_hex, h.hash_hex, h.iterations),
          "wrong password rejected");
    check(!auth::verify_password("Correct-Horse-Battery-9", h.salt_hex, h.hash_hex, 0),
          "zero iterations rejected");
    check(!auth::verify_password("Correct-Horse-Battery-9", "", h.hash_hex, h.iterations),
          "empty salt rejected");

    // A fresh salt per call means two hashes of the same password differ.
    const auto h2 = auth::hash_password("Correct-Horse-Battery-9");
    check(h.salt_hex != h2.salt_hex, "fresh salt per hash");
    check(h.hash_hex != h2.hash_hex, "different salt -> different hash");
    check(auth::verify_password("Correct-Horse-Battery-9", h2.salt_hex, h2.hash_hex, h2.iterations),
          "second hash also verifies");

    // Session tokens: right length, and not repeating.
    const std::string t1 = auth::random_hex(32), t2 = auth::random_hex(32);
    check(t1.size() == 64, "32-byte token is 64 hex chars");
    check(t1 != t2, "tokens are random");

    if (g_failures == 0) { std::cout << "password_test: all checks passed\n"; return 0; }
    std::cerr << "password_test: " << g_failures << " check(s) failed\n";
    return 1;
}
