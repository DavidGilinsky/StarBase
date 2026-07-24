// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/tls_cert.hpp
// Purpose:       Ensure a self-signed TLS certificate + private key exist on
//                disk, generating an RSA-2048 / SHA-256 pair when missing, so
//                the daemon can serve HTTPS on a LAN without an external CA.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase::api {

// Ensure a usable certificate/key pair exists at the given paths. If both files
// are already present they are left untouched. Otherwise a self-signed cert
// (CN=common_name, SANs for localhost + 127.0.0.1 + the CN) and its RSA key are
// generated, creating parent directories as needed; the key file is written
// mode 0600. On success, `info` is filled with a human-readable summary
// (fingerprint + path). Returns true if the pair exists afterward, false (with
// `info` describing the error) on failure.
bool ensure_self_signed_cert(const std::string& cert_path, const std::string& key_path,
                             const std::string& common_name, std::string& info);

}  // namespace starbase::api
