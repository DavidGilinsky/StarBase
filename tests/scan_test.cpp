// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/scan_test.cpp
// Purpose:       Integration test for the threaded sweep: index a temporary
//                tree, honouring ignore globs, the settle gate, error recording,
//                and cheap change detection on re-scan.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// DB-gated like db_test: skips (exit 0) unless SB_TEST_DB_NAME is set.
// ---------------------------------------------------------------------------
#include <fitsio.h>
#include <sys/stat.h>
#include <utime.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "database.hpp"
#include "mapping_loader.hpp"
#include "scanner.hpp"

namespace {

namespace db = starbase::db;
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

void write_fits(const std::string& path, const char* imagetyp, const char* object) {
    stdfs::create_directories(stdfs::path(path).parent_path());
    std::remove(path.c_str());
    fitsfile* f = nullptr; int status = 0;
    ffinit(&f, path.c_str(), &status);
    long naxes[2] = {64, 48};
    ffcrim(f, USHORT_IMG, 2, naxes, &status);
    char it[32]; std::snprintf(it, sizeof it, "%s", imagetyp);
    ffpky(f, TSTRING, "IMAGETYP", it, "", &status);
    char ob[64]; std::snprintf(ob, sizeof ob, "%s", object);
    ffpky(f, TSTRING, "OBJECT", ob, "", &status);
    char dobs[] = "2026-07-03T04:24:23"; ffpky(f, TSTRING, "DATE-OBS", dobs, "", &status);
    double exp = 120.0; ffpky(f, TDOUBLE, "EXPTIME", &exp, "", &status);
    ffclos(f, &status);
    if (status) { std::cerr << "fits write failed " << status << "\n"; std::exit(2); }
}

// Backdate mtime so the settle gate does not defer freshly-written test files.
void backdate(const std::string& path, int seconds_ago) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return;
    struct utimbuf ut;
    ut.actime = st.st_atime;
    ut.modtime = ::time(nullptr) - seconds_ago;
    ::utime(path.c_str(), &ut);
}

long long scalar(db::Database& d, const std::string& sql) {
    auto r = d.query(sql);
    return (r.empty() || !r[0][0]) ? -1 : std::stoll(*r[0][0]);
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) { std::cout << "scan_test: SB_TEST_DB_NAME not set; skipping\n"; return 0; }
    const std::string schema = find_sql("schema.sql"), seed = find_sql("seed.sql");
    if (schema.empty() || seed.empty()) { std::cerr << "scan_test: no sql\n"; return 1; }

    const std::string root = "/tmp/sbscan_" + std::to_string(::getpid());
    stdfs::remove_all(root);

    // A representative tree: three good frames, an ignored Windows file, an
    // ignored GoodSync staging dir, a bad .fits, and a fresh (settling) frame.
    write_fits(root + "/lights/M31/a.fits", "LIGHT", "M31");
    write_fits(root + "/lights/M31/b.fits", "LIGHT", "M31");
    write_fits(root + "/calibration/dark/d.fits", "DARK", "");
    { std::ofstream(root + "/lights/M31/Thumbs.db") << "junk"; }        // ignored name
    write_fits(root + "/_gsdata_/hidden.fits", "LIGHT", "X");           // ignored dir
    { std::ofstream(root + "/lights/M31/bad.fits") << "NOT A FITS FILE"; }  // parse error
    // Backdate every non-fresh file past the settle window, including bad.fits
    // so it is actually read (and errors) rather than merely deferred.
    for (const char* p : {"/lights/M31/a.fits", "/lights/M31/b.fits",
                          "/calibration/dark/d.fits", "/lights/M31/bad.fits"})
        backdate(root + p, 120);
    // A fresh frame (mtime now) must be deferred by the settle gate.
    write_fits(root + "/lights/M31/fresh.fits", "LIGHT", "M31");

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        d.apply_script(seed);
        d.exec("DELETE FROM roots WHERE label = 'scantest'");
        db::RootFields rf; rf.fs_type = "ext4";
        const int root_id = d.add_root("scantest", root, rf);
        const auto mapping = starbase::index::load_mapping(d);
        db::RootRow rr; rr.id = root_id; rr.label = "scantest"; rr.path = root; rr.case_sensitive = true;

        // Scope all counts to this root: the DB-gated tests share one database,
        // so a bare COUNT(*) would include other roots' rows.
        const std::string R = " root_id = " + std::to_string(root_id);
        const std::string RF =  // frames joined to their file's root
            " file_id IN (SELECT id FROM files WHERE root_id = " + std::to_string(root_id) + ")";

        starbase::scan::ScanConfig sc;
        sc.threads = 4;
        sc.settle_seconds = 30;                 // fresh.fits (mtime now) must defer
        sc.ignore_globs = {"_gsdata_", "Thumbs.db", "*.tmp"};

        const auto s1 = starbase::scan::scan_root(cfg, rr, mapping, {}, sc);
        check(s1.files_added == 3, "3 good frames added (a, b, d)");
        check(s1.files_error == 1, "bad.fits recorded as error");
        check(s1.files_settling == 1, "fresh.fits deferred by settle gate");
        check(scalar(d, "SELECT COUNT(*) FROM frames WHERE" + RF) == 3, "3 frames in db");

        // Ignore globs kept out both the Windows file and the GoodSync subtree.
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE filename='Thumbs.db' AND" + R) == 0,
              "Thumbs.db ignored");
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE rel_path LIKE '_gsdata_%' AND" + R) == 0,
              "_gsdata_ subtree pruned");
        // The bad file is a row with status=error, not a silence.
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE status='error' AND" + R) == 1,
              "error file is a row, not a silence");
        // Buckets derived from the path. Three files sit under lights/ (a, b,
        // and the error row for bad.fits); one under calibration/.
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE bucket='lights' AND" + R) == 3, "lights bucket");
        check(scalar(d, "SELECT COUNT(*) FROM files WHERE bucket='calibration' AND" + R) == 1,
              "calibration bucket");

        // ---- re-scan: unchanged files are cheap (skipped), nothing duplicated ----
        const auto s2 = starbase::scan::scan_root(cfg, rr, mapping, {}, sc);
        check(s2.files_added == 0, "re-scan adds nothing");
        check(s2.files_skipped == 3, "re-scan skips the 3 unchanged frames");
        check(scalar(d, "SELECT COUNT(*) FROM frames WHERE" + RF) == 3, "still 3 frames after re-scan");

        // ---- change detection: touch one frame, it re-indexes as updated ----
        // Give it a distinctly different mtime (60s vs the original 120s): a
        // small object-name edit stays within the same 2880-byte FITS block, so
        // size is unchanged and mtime is the only signal.
        write_fits(root + "/lights/M31/a.fits", "LIGHT", "M31_v2");
        backdate(root + "/lights/M31/a.fits", 60);
        const auto s3 = starbase::scan::scan_root(cfg, rr, mapping, {}, sc);
        check(s3.files_updated == 1, "changed frame re-indexed as updated");
        check(s3.files_skipped == 2, "other two still skipped");
        check(scalar(d, "SELECT COUNT(*) FROM frames WHERE object='M31_v2' AND" + RF) == 1,
              "changed content reflected");

        // Now that fresh.fits has aged past settle, a later scan picks it up.
        backdate(root + "/lights/M31/fresh.fits", 120);
        const auto s4 = starbase::scan::scan_root(cfg, rr, mapping, {}, sc);
        check(s4.files_added == 1, "settled frame indexed on the next pass");

        d.remove_root("scantest");
    } catch (const std::exception& e) {
        std::cerr << "scan_test: " << e.what() << "\n";
        stdfs::remove_all(root);
        return 1;
    }
    stdfs::remove_all(root);

    if (g_failures == 0) { std::cout << "scan_test: all checks passed\n"; return 0; }
    std::cerr << "scan_test: " << g_failures << " check(s) failed\n";
    return 1;
}
