// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/action_test.cpp
// Purpose:       Tests for the action engine: staging tree layout and link
//                modes, fsop dry-run safety, trash-not-unlink with index write-
//                back, and export formats. Operates on a temporary file tree.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// DB-gated like db_test: skips (exit 0) unless SB_TEST_DB_NAME is set.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "action.hpp"
#include "database.hpp"

namespace {

namespace db = starbase::db;
namespace a = starbase::action;
namespace stdfs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
std::string find_sql(const std::string& n) {
    for (const std::string b : {"sql/", "../sql/", "../../sql/"})
        if (stdfs::exists(b + n)) return b + n;
    return {};
}
long long scalar(db::Database& d, const std::string& sql) {
    auto r = d.query(sql);
    return (r.empty() || !r[0][0]) ? -1 : std::stoll(*r[0][0]);
}

int g_seq = 0;
// Create a real file under the root and its files/frames rows; return frame id.
long long add(db::Database& d, int root_id, const stdfs::path& root, const std::string& rel,
              const std::string& type) {
    stdfs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel) << "frame " << ++g_seq;
    const std::string fn = stdfs::path(rel).filename().string();
    d.exec("INSERT INTO files (root_id, rel_path, rel_path_hash, filename, format, bucket, status) "
           "VALUES (" + std::to_string(root_id) + ", '" + rel + "', UNHEX(MD5('" + rel + "')), '" +
           fn + "', 'fits', 'lights', 'ok')");
    const long long fid = scalar(d, "SELECT id FROM files WHERE rel_path_hash=UNHEX(MD5('" + rel + "'))");
    d.exec("INSERT INTO frames (file_id, hdu, fingerprint, image_type) VALUES (" +
           std::to_string(fid) + ", 0, UNHEX(MD5('" + rel + "')), '" + type + "')");
    return scalar(d, "SELECT id FROM frames WHERE file_id=" + std::to_string(fid));
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) { std::cout << "action_test: SB_TEST_DB_NAME not set; skipping\n"; return 0; }
    const std::string schema = find_sql("schema.sql");
    if (schema.empty()) { std::cerr << "action_test: no schema.sql\n"; return 1; }

    const stdfs::path base = stdfs::temp_directory_path() / ("sbact_" + std::to_string(::getpid()));
    const stdfs::path archive = base / "archive", staging = base / "staging", trash = base / "trash";
    stdfs::remove_all(base);

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        for (const char* t : {"job_items", "jobs", "frames", "files", "roots"})
            d.exec(std::string("DELETE FROM ") + t);
        db::RootFields rf; rf.fs_type = "ext4";
        const int root = d.add_root("acttest", archive.string(), rf);

        const long long L1 = add(d, root, archive, "lights/M31/a.fits", "light");
        const long long L2 = add(d, root, archive, "lights/M31/b.fits", "light");
        const long long D1 = add(d, root, archive, "calibration/dark/d.fits", "dark");

        a::ActionConfig ac;
        ac.staging_root = staging.string();
        ac.trash_root = trash.string();
        ac.link_mode = "symlink";

        auto refs = a::resolve_by_ids(d, {L1, L2, D1});
        check(refs.size() == 3, "resolve_by_ids returns all three");

        // ---- stage: a typed symlink tree ----
        auto st = a::stage(d, ac, refs);
        check(st.job_id > 0, "stage created a job");
        check(st.done == 3 && st.failed == 0, "staged all three");
        const stdfs::path jobdir = st.root;
        check(stdfs::is_symlink(jobdir / "light" / "a.fits"), "light symlinked into light/");
        check(stdfs::is_symlink(jobdir / "dark" / "d.fits"), "dark symlinked into dark/");
        check(stdfs::read_symlink(jobdir / "light" / "a.fits") == (archive / "lights/M31/a.fits"),
              "symlink points at the real frame");
        check(scalar(d, "SELECT COUNT(*) FROM job_items WHERE job_id=" +
                            std::to_string(st.job_id)) == 3, "3 job_items recorded");

        // ---- fsop dry-run: touches nothing ----
        auto dry = a::fsop(d, ac, "trash", a::resolve_by_ids(d, {D1}), "", /*dry_run=*/true);
        check(dry.dry_run && dry.done == 1, "dry-run reports one item");
        check(dry.job_id == 0, "dry-run records no job");
        check(stdfs::exists(archive / "calibration/dark/d.fits"), "dry-run left the file in place");
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE status='ok'") == 3,
              "dry-run changed no file rows (all still ok)");

        // ---- trash: relocates to trash, never unlinks, marks the row missing ----
        auto tr = a::fsop(d, ac, "trash", a::resolve_by_ids(d, {D1}), "", /*dry_run=*/false);
        check(tr.done == 1 && tr.failed == 0, "trash moved the file");
        check(!stdfs::exists(archive / "calibration/dark/d.fits"), "file gone from the archive");
        check(stdfs::exists(stdfs::path(tr.root) / "d.fits"), "file preserved in trash (not unlinked)");
        const long long D1_file = scalar(d, "SELECT file_id FROM frames WHERE id=" + std::to_string(D1));
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE id=" + std::to_string(D1_file) +
                            " AND status='missing'") == 1, "trashed file marked missing (left the roots)");

        // ---- move within a root: re-anchors the index row ----
        stdfs::create_directories(archive / "lights/M31b");
        auto mv = a::fsop(d, ac, "move", a::resolve_by_ids(d, {L2}),
                          (archive / "lights/M31b").string(), /*dry_run=*/false);
        check(mv.done == 1, "move relocated the frame");
        check(stdfs::exists(archive / "lights/M31b/b.fits"), "file at the new location");
        const long long L2_file = scalar(d, "SELECT file_id FROM frames WHERE id=" + std::to_string(L2));
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE id=" + std::to_string(L2_file) +
                            " AND rel_path='lights/M31b/b.fits' AND status='ok'") == 1,
              "index re-anchored to the new path within the root");

        // ---- export ----
        auto lights = a::resolve_by_ids(d, {L1});
        const std::string paths = a::export_frames(d, lights, "paths");
        check(paths.find("lights/M31/a.fits") != std::string::npos, "paths export lists the file");
        const std::string csv = a::export_frames(d, lights, "csv");
        check(csv.rfind("frame_id,image_type", 0) == 0, "csv export has a header");
        const std::string js = a::export_frames(d, lights, "json");
        check(js.find("\"image_type\": \"light\"") != std::string::npos, "json export well-formed");

        d.remove_root("acttest");
    } catch (const std::exception& e) {
        std::cerr << "action_test: " << e.what() << "\n";
        stdfs::remove_all(base);
        return 1;
    }
    stdfs::remove_all(base);

    if (g_failures == 0) { std::cout << "action_test: all checks passed\n"; return 0; }
    std::cerr << "action_test: " << g_failures << " check(s) failed\n";
    return 1;
}
