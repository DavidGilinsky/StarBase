// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/auth/password.cpp
// Purpose:       OpenSSL-backed PBKDF2-HMAC-SHA256 password hashing + tokens.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "password.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace starbase::auth {
namespace {

constexpr int kIterations = 120000;
constexpr int kSaltLen = 16;   // fits users.pwd_salt VARBINARY(32)
constexpr int kHashLen = 32;   // fits users.pwd_hash VARBINARY(64)

std::string to_hex(const unsigned char* data, size_t len) {
    static const char* const kHex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s += kHex[data[i] >> 4];
        s += kHex[data[i] & 0x0f];
    }
    return s;
}

std::vector<unsigned char> from_hex(const std::string& s) {
    std::vector<unsigned char> out;
    if (s.size() % 2 != 0) return out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        try { out.push_back(static_cast<unsigned char>(std::stoi(s.substr(i, 2), nullptr, 16))); }
        catch (const std::exception&) { return {}; }
    }
    return out;
}

std::vector<unsigned char> pbkdf2(const std::string& pw, const unsigned char* salt, int saltlen,
                                  int iters, int outlen) {
    std::vector<unsigned char> out(static_cast<size_t>(outlen));
    if (PKCS5_PBKDF2_HMAC(pw.data(), static_cast<int>(pw.size()), salt, saltlen, iters,
                          EVP_sha256(), outlen, out.data()) != 1)
        throw std::runtime_error("PBKDF2 derivation failed");
    return out;
}

}  // namespace

Hashed hash_password(const std::string& password) {
    unsigned char salt[kSaltLen];
    if (RAND_bytes(salt, kSaltLen) != 1) throw std::runtime_error("RAND_bytes failed");
    const auto hash = pbkdf2(password, salt, kSaltLen, kIterations, kHashLen);
    return {to_hex(salt, kSaltLen), to_hex(hash.data(), hash.size()), kIterations};
}

bool verify_password(const std::string& password, const std::string& salt_hex,
                     const std::string& hash_hex, int iterations) {
    if (iterations < 1) return false;
    const auto salt = from_hex(salt_hex);
    const auto expected = from_hex(hash_hex);
    if (salt.empty() || expected.empty()) return false;
    const auto actual = pbkdf2(password, salt.data(), static_cast<int>(salt.size()), iterations,
                               static_cast<int>(expected.size()));
    if (actual.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < actual.size(); ++i) diff |= actual[i] ^ expected[i];
    return diff == 0;
}

std::string random_hex(int nbytes) {
    std::vector<unsigned char> b(static_cast<size_t>(nbytes));
    if (RAND_bytes(b.data(), nbytes) != 1) throw std::runtime_error("RAND_bytes failed");
    return to_hex(b.data(), b.size());
}

}  // namespace starbase::auth
