// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/auth/password.hpp
// Purpose:       Password hashing (PBKDF2-HMAC-SHA256) and secure random tokens
//                for web-UI/API authentication. Mirrors the NightWatcher2 auth
//                pattern, adapted to StarBase's users schema which stores the
//                salt, hash, and iteration count as separate columns.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase::auth {

// A hashed password, ready to store: salt and derived key as lowercase hex
// (for UNHEX into the VARBINARY columns), plus the iteration count.
struct Hashed {
    std::string salt_hex;
    std::string hash_hex;
    int iterations = 0;
};

// Hash a password with a fresh random salt.
Hashed hash_password(const std::string& password);

// Verify a password against stored salt/hash/iterations (hex as read back with
// HEX()). Constant-time comparison; false on any parse error.
bool verify_password(const std::string& password, const std::string& salt_hex,
                     const std::string& hash_hex, int iterations);

// Cryptographically-random lowercase hex string of `nbytes` bytes (session
// tokens are 32 bytes -> 64 chars, matching sessions.token CHAR(64)).
std::string random_hex(int nbytes);

}  // namespace starbase::auth
