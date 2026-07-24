// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/api/tls_cert.cpp
// Purpose:       Generate a self-signed RSA-2048 / SHA-256 certificate and key
//                (with localhost/127.0.0.1/CN SANs) via OpenSSL when none is
//                present, so nightwatcherd can offer HTTPS out of the box.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "tls_cert.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace starbase::api {
namespace {

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// mkdir -p for the directory portion of a file path (best effort).
void ensure_parent_dir(const std::string& file_path) {
    const auto slash = file_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    const std::string dir = file_path.substr(0, slash);
    std::string acc;
    size_t pos = 0;
    while (pos <= dir.size()) {
        const auto next = dir.find('/', pos);
        const std::string part = dir.substr(0, next == std::string::npos ? dir.size() : next);
        if (!part.empty()) ::mkdir(part.c_str(), 0755);  // ignore EEXIST/errors; caller checks writes
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

// Build the Subject Alternative Name list so the cert is valid for every
// address a client might use: loopback, the CN, this host's names, and all of
// its non-loopback interface IPs (so https://<lan-ip>:port works in a browser).
std::string build_san_list(const std::string& cn) {
    std::vector<std::string> entries;
    const auto add = [&entries](const std::string& e) {
        if (std::find(entries.begin(), entries.end(), e) == entries.end()) entries.push_back(e);
    };

    add("DNS:localhost");
    add("IP:127.0.0.1");
    add("IP:0:0:0:0:0:0:0:1");  // ::1
    if (!cn.empty() && cn != "localhost") add("DNS:" + cn);

    char hn[256] = {0};
    if (gethostname(hn, sizeof(hn) - 1) == 0 && hn[0]) add(std::string("DNS:") + hn);

    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs* p = ifaddr; p; p = p->ifa_next) {
            if (!p->ifa_addr) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;       // skip down interfaces
            if (p->ifa_flags & IFF_LOOPBACK) continue;    // loopback already covered
            char buf[INET6_ADDRSTRLEN] = {0};
            if (p->ifa_addr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) add(std::string("IP:") + buf);
            } else if (p->ifa_addr->sa_family == AF_INET6) {
                auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(p->ifa_addr);
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;  // fe80:: needs a scope id
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) add(std::string("IP:") + buf);
            }
        }
        freeifaddrs(ifaddr);
    }

    std::string out;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out += ',';
        out += entries[i];
    }
    return out;
}

std::string hex_fingerprint(X509* x509) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(x509, EVP_sha256(), md, &len) != 1) return "?";
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (unsigned int i = 0; i < len; ++i) {
        if (i) out += ':';
        out += hexd[md[i] >> 4];
        out += hexd[md[i] & 0x0F];
    }
    return out;
}

}  // namespace

bool ensure_self_signed_cert(const std::string& cert_path, const std::string& key_path,
                             const std::string& common_name, std::string& info) {
    if (file_exists(cert_path) && file_exists(key_path)) {
        info = "using existing certificate " + cert_path;
        return true;
    }

    const std::string cn = common_name.empty() ? std::string("nightwatcher") : common_name;

    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    bool ok = false;

    do {
        // ---- RSA-2048 key ----
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!kctx) { info = "EVP_PKEY_CTX_new_id failed"; break; }
        const bool keyok = EVP_PKEY_keygen_init(kctx) > 0 &&
                           EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) > 0 &&
                           EVP_PKEY_keygen(kctx, &pkey) > 0;
        EVP_PKEY_CTX_free(kctx);
        if (!keyok) { info = "RSA key generation failed"; break; }

        // ---- self-signed X.509v3 cert ----
        x509 = X509_new();
        if (!x509) { info = "X509_new failed"; break; }
        X509_set_version(x509, 2);  // version 3
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60 * 24 * 365 * 10);  // ~10 years
        if (X509_set_pubkey(x509, pkey) != 1) { info = "X509_set_pubkey failed"; break; }

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("StarBase"), -1, -1, 0);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
        X509_set_issuer_name(x509, name);  // self-signed: issuer == subject

        // Subject Alternative Names so browsers can match the host we bind on,
        // including every local interface IP (LAN access by address).
        const std::string san = build_san_list(cn);
        if (X509_EXTENSION* ext =
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str())) {
            X509_add_ext(x509, ext, -1);
            X509_EXTENSION_free(ext);
        }
        if (X509_sign(x509, pkey, EVP_sha256()) == 0) { info = "X509_sign failed"; break; }

        // ---- write key (0600) then cert ----
        ensure_parent_dir(key_path);
        ensure_parent_dir(cert_path);
        FILE* kf = std::fopen(key_path.c_str(), "wb");
        if (!kf) { info = "cannot open key file for writing: " + key_path; break; }
        const bool kwrote = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        std::fclose(kf);
        if (!kwrote) { info = "PEM_write_PrivateKey failed"; break; }
        ::chmod(key_path.c_str(), 0600);

        FILE* cf = std::fopen(cert_path.c_str(), "wb");
        if (!cf) { info = "cannot open cert file for writing: " + cert_path; break; }
        const bool cwrote = PEM_write_X509(cf, x509) == 1;
        std::fclose(cf);
        if (!cwrote) { info = "PEM_write_X509 failed"; break; }

        info = "generated self-signed certificate (CN=" + cn + ", SHA-256 " + hex_fingerprint(x509) +
               ") at " + cert_path;
        ok = true;
    } while (0);

    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    return ok;
}

}  // namespace starbase::api
