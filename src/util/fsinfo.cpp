// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/util/fsinfo.cpp
// Purpose:       Implementation of filesystem type, inotify suitability, and
//                case-sensitivity probing.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "fsinfo.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace starbase::fs {
namespace {

// /proc/mounts escapes spaces and a few other characters as octal.
std::string unescape_mount_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            const std::string oct = s.substr(i + 1, 3);
            out.push_back(static_cast<char>(std::stoi(oct, nullptr, 8)));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Every network filesystem: inotify cannot see another host's writes.
// autofs appears at a mount point before the real filesystem is triggered, so
// it is treated as unwatchable too.
const std::array<const char*, 10> kNetworkFilesystems = {
    "nfs", "nfs4", "cifs", "smb3", "smbfs", "afs", "fuse.sshfs", "ceph",
    "glusterfs", "autofs"};

}  // namespace

std::string detect_fs_type(const std::string& path) {
    std::error_code ec;
    // Resolve symlinks and '..' so the prefix match is against a real path.
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    const std::string target = ec ? path : canonical.string();

    std::ifstream mounts("/proc/mounts");
    if (!mounts) return {};

    std::string best_type;
    size_t best_len = 0;
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream is(line);
        std::string dev, mount_point, type;
        if (!(is >> dev >> mount_point >> type)) continue;
        mount_point = unescape_mount_field(mount_point);

        // The mount point must be a path-component prefix of the target, so
        // "/astro" does not match "/astronomy".
        if (target.compare(0, mount_point.size(), mount_point) != 0) continue;
        const bool exact = target.size() == mount_point.size();
        const bool boundary = mount_point == "/" ||
                              (target.size() > mount_point.size() &&
                               target[mount_point.size()] == '/');
        if (!exact && !boundary) continue;

        // Longest match wins; a later line at equal length wins too, because
        // /proc/mounts lists a stacked mount after the one it covers.
        if (mount_point.size() >= best_len) {
            best_len = mount_point.size();
            best_type = type;
        }
    }
    return best_type;
}

bool supports_inotify(const std::string& fs_type) {
    if (fs_type.empty()) return false;  // unknown: do not promise what we cannot deliver
    return std::none_of(kNetworkFilesystems.begin(), kNetworkFilesystems.end(),
                        [&](const char* n) { return fs_type == n; });
}

bool detect_case_sensitive(const std::string& path) {
    namespace stdfs = std::filesystem;
    std::error_code ec;

    stdfs::directory_iterator it(path, stdfs::directory_options::skip_permission_denied, ec);
    if (ec) return true;

    for (const auto& entry : it) {
        const std::string name = entry.path().filename().string();

        // Invert the case of every cased character. An entry with no cased
        // letters tells us nothing, so skip it.
        std::string flipped;
        flipped.reserve(name.size());
        bool has_alpha = false;
        for (unsigned char c : name) {
            if (std::isupper(c)) {
                flipped.push_back(static_cast<char>(std::tolower(c)));
                has_alpha = true;
            } else if (std::islower(c)) {
                flipped.push_back(static_cast<char>(std::toupper(c)));
                has_alpha = true;
            } else {
                flipped.push_back(static_cast<char>(c));
            }
        }
        if (!has_alpha) continue;

        // If the case-flipped name resolves, the filesystem folds case.
        const auto probe = stdfs::path(path) / flipped;
        std::error_code probe_ec;
        if (stdfs::exists(probe, probe_ec) && !probe_ec) return false;
        return true;
    }

    return true;  // empty directory: assume case-sensitive
}

}  // namespace starbase::fs
