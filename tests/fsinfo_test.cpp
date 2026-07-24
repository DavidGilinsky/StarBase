// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/fsinfo_test.cpp
// Purpose:       Unit tests for filesystem type, inotify suitability, and
//                case-sensitivity probing.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "fsinfo.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

void test_fs_type() {
    // "/" is always a mount point, so a type must come back for it.
    const std::string root = starbase::fs::detect_fs_type("/");
    check(!root.empty(), "detect_fs_type(\"/\") returns a type");
    std::cout << "  /            -> " << root << "\n";

    // /proc is its own filesystem and must not be reported as whatever "/" is.
    const std::string proc = starbase::fs::detect_fs_type("/proc");
    check(proc == "proc", "detect_fs_type(\"/proc\") == proc");

    // Longest-prefix matching must not confuse a prefix string with a path
    // component: /proc must not match a hypothetical /pro mount.
    const std::string procsys = starbase::fs::detect_fs_type("/proc/self");
    check(procsys == "proc", "nested path resolves to the nearest mount");

    // A nonexistent path still resolves via its existing ancestor.
    const std::string missing = starbase::fs::detect_fs_type("/nonexistent-xyz/deeper");
    check(!missing.empty(), "nonexistent path falls back to an ancestor mount");
}

void test_inotify_support() {
    check(starbase::fs::supports_inotify("ext4"), "ext4 supports inotify");
    check(starbase::fs::supports_inotify("xfs"), "xfs supports inotify");
    check(starbase::fs::supports_inotify("btrfs"), "btrfs supports inotify");

    // The whole point: a network filesystem cannot report another host's writes.
    check(!starbase::fs::supports_inotify("nfs"), "nfs does not support inotify");
    check(!starbase::fs::supports_inotify("nfs4"), "nfs4 does not support inotify");
    check(!starbase::fs::supports_inotify("cifs"), "cifs does not support inotify");
    check(!starbase::fs::supports_inotify("smb3"), "smb3 does not support inotify");
    // autofs sits in front of the real filesystem until triggered.
    check(!starbase::fs::supports_inotify("autofs"), "autofs does not support inotify");
    // Unknown means unknown; do not promise what we cannot deliver.
    check(!starbase::fs::supports_inotify(""), "unknown fs type is not watchable");
}

void test_case_sensitivity() {
    namespace stdfs = std::filesystem;
    const auto dir = stdfs::temp_directory_path() /
                     ("starbase_case_test_" + std::to_string(::getpid()));
    std::error_code ec;
    stdfs::create_directories(dir, ec);

    // An empty directory yields no evidence, so it must assume case-sensitive:
    // hashing distinct strings distinctly is the safe default.
    check(starbase::fs::detect_case_sensitive(dir.string()),
          "empty directory assumed case-sensitive");

    { std::ofstream f(dir / "MixedCase.fits"); f << "x"; }
    // On Linux /tmp this is ext4/tmpfs, both case-sensitive.
    const bool cs = starbase::fs::detect_case_sensitive(dir.string());
    const std::string t = starbase::fs::detect_fs_type(dir.string());
    std::cout << "  " << dir.string() << " (" << t << ") case-sensitive: "
              << (cs ? "yes" : "no") << "\n";
    check(cs, "tmp directory reports case-sensitive");

    // An unreadable or missing directory must not throw; it defaults to true.
    check(starbase::fs::detect_case_sensitive("/nonexistent-xyz-abc"),
          "missing directory defaults to case-sensitive");

    stdfs::remove_all(dir, ec);
}

}  // namespace

int main() {
    test_fs_type();
    test_inotify_support();
    test_case_sensitivity();

    if (g_failures == 0) {
        std::cout << "fsinfo_test: all checks passed\n";
        return 0;
    }
    std::cerr << "fsinfo_test: " << g_failures << " check(s) failed\n";
    return 1;
}
