// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/util/fsinfo.hpp
// Purpose:       Filesystem probing for a monitored root: filesystem type, case
//                sensitivity, and whether inotify can be trusted there.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace starbase::fs {

// Filesystem type as named in /proc/mounts ("ext4", "xfs", "nfs4", "cifs"),
// resolved by longest-prefix match on the mount point. Empty if it cannot be
// determined.
std::string detect_fs_type(const std::string& path);

// True when inotify can be relied on for this filesystem.
//
// It cannot be on any network filesystem: an inotify watch only reports changes
// made through the local kernel, so writes performed by another host are
// invisible. The scheduled sweep is authoritative everywhere; this only decides
// whether watching is a useful accelerator on top of it.
bool supports_inotify(const std::string& fs_type);

// True when the filesystem distinguishes case.
//
// Probed by taking an existing entry in the directory and attempting to resolve
// it with its case inverted; if that succeeds, the filesystem folds case. This
// is read-only, so it works on a root mounted read-only. Defaults to true (the
// safe assumption: hashing distinct strings distinctly) when the directory is
// empty or unreadable, or when no entry contains a cased letter.
bool detect_case_sensitive(const std::string& path);

}  // namespace starbase::fs
