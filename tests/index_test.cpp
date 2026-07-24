// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/index_test.cpp
// Purpose:       Integration test for the frame store: index a file end to end
//                and verify the files, frames, and frame_keywords rows, plus
//                idempotency on re-index.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// DB-gated like db_test: skips (exit 0) unless SB_TEST_DB_NAME is set. Writes a
// synthetic FITS, so it needs no archive access.
// ---------------------------------------------------------------------------
#include <fitsio.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <unistd.h>

#include "database.hpp"
#include "fits_reader.hpp"
#include "frame_store.hpp"
#include "mapping_loader.hpp"

namespace {

namespace db = starbase::db;
namespace fits = starbase::fits;
namespace idx = starbase::index;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}

std::string find_sql(const std::string& name) {
    for (const std::string base : {"sql/", "../sql/", "../../sql/"})
        if (std::filesystem::exists(base + name)) return base + name;
    return {};
}

std::string write_synthetic() {
    std::string path = "/tmp/starbase_index_test_" + std::to_string(::getpid()) + ".fits";
    std::remove(path.c_str());
    fitsfile* f = nullptr;
    int status = 0;
    ffinit(&f, path.c_str(), &status);
    long naxes[2] = {6248, 4176};
    ffcrim(f, USHORT_IMG, 2, naxes, &status);
    char imagetyp[] = "LIGHT";  ffpky(f, TSTRING, "IMAGETYP", imagetyp, "", &status);
    char dateobs[] = "2026-07-03T04:24:23.98"; ffpky(f, TSTRING, "DATE-OBS", dateobs, "", &status);
    char instrume[] = "ZWO ASI2600MC Air"; ffpky(f, TSTRING, "INSTRUME", instrume, "", &status);
    char object[] = "NGC 7000"; ffpky(f, TSTRING, "OBJECT", object, "", &status);
    char filter[] = "CLEAR"; ffpky(f, TSTRING, "FILTER", filter, "", &status);
    double exptime = 120.0; ffpky(f, TDOUBLE, "EXPTIME", &exptime, "", &status);
    int gain = 100; ffpky(f, TINT, "GAIN", &gain, "", &status);
    int off = 50; ffpky(f, TINT, "OFFSET", &off, "", &status);
    int xbin = 1; ffpky(f, TINT, "XBINNING", &xbin, "", &status);
    ffpcom(f, "a comment", &status);
    ffclos(f, &status);
    if (status) { std::cerr << "synthetic FITS write failed " << status << "\n"; std::exit(2); }
    return path;
}

long long scalar(db::Database& d, const std::string& sql) {
    auto rows = d.query(sql);
    if (rows.empty() || !rows[0][0]) return -1;
    return std::stoll(*rows[0][0]);
}
std::string scalar_s(db::Database& d, const std::string& sql) {
    auto rows = d.query(sql);
    if (rows.empty() || !rows[0][0]) return {};
    return *rows[0][0];
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) {
        std::cout << "index_test: SB_TEST_DB_NAME not set; skipping\n";
        return 0;
    }
    const std::string schema = find_sql("schema.sql"), seed = find_sql("seed.sql");
    if (schema.empty() || seed.empty()) {
        std::cerr << "index_test: cannot locate sql/schema.sql and sql/seed.sql\n";
        return 1;
    }

    const std::string fpath = write_synthetic();
    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);
        d.apply_script(seed);

        // Clean slate for a repeatable run.
        d.exec("DELETE FROM roots WHERE label = 'idxtest'");
        db::RootFields rf;
        rf.fs_type = "ext4";
        const int root_id = d.add_root("idxtest", "/tmp", rf);

        const auto mapping = idx::load_mapping(d);
        const auto header = fits::read_header(fpath);

        idx::FileInfo info;
        info.root_id = root_id;
        info.rel_path = "lights/NGC7000/frame.fits";
        info.filename = "frame.fits";
        info.ext = ".fits";
        info.format = "fits";
        info.bucket = "lights";
        info.size_bytes = 52194240;
        info.mtime_utc = "2026-07-03 04:24:30";
        info.inode = 71322;

        const auto r1 = idx::store_file(d, info, header, mapping, {-7.0});
        check(r1.file_id > 0, "store returns a file id");
        check(r1.frames_written == 1, "one frame written");
        check(r1.keywords_written >= 9, "keywords written");

        // files row
        check(scalar(d, "SELECT COUNT(*) FROM files") == 1, "one files row");
        check(scalar_s(d, "SELECT status FROM files WHERE id = " +
                              std::to_string(r1.file_id)) == "ok", "file status ok");
        check(scalar_s(d, "SELECT bucket FROM files WHERE id = " +
                              std::to_string(r1.file_id)) == "lights", "file bucket");

        // frames row
        const long long frame_id = scalar(d, "SELECT id FROM frames WHERE file_id = " +
                                                 std::to_string(r1.file_id));
        check(frame_id > 0, "frame row exists");
        check(scalar_s(d, "SELECT image_type FROM frames WHERE id = " +
                              std::to_string(frame_id)) == "light", "frame classified light");
        check(scalar_s(d, "SELECT object FROM frames WHERE id = " +
                              std::to_string(frame_id)) == "NGC 7000", "frame object");
        check(scalar(d, "SELECT gain FROM frames WHERE id = " +
                            std::to_string(frame_id)) == 100, "frame gain");
        check(scalar_s(d, "SELECT session_night FROM frames WHERE id = " +
                              std::to_string(frame_id)) == "2026-07-02", "frame night");
        check(scalar(d, "SELECT LENGTH(fingerprint) FROM frames WHERE id = " +
                            std::to_string(frame_id)) == 16, "fingerprint is 16 bytes");
        // Geometry from the image itself.
        check(scalar(d, "SELECT naxis1 FROM frames WHERE id = " +
                            std::to_string(frame_id)) == 6248, "frame naxis1");

        // frame_keywords
        const long long kw = scalar(d, "SELECT COUNT(*) FROM frame_keywords WHERE frame_id = " +
                                            std::to_string(frame_id));
        check(kw >= 9, "keyword rows present");
        check(scalar_s(d, "SELECT value FROM frame_keywords WHERE frame_id = " +
                              std::to_string(frame_id) + " AND keyword = 'IMAGETYP'") == "LIGHT",
              "keyword value stored verbatim");

        // v_frames view resolves and assembles the path.
        check(scalar(d, "SELECT COUNT(*) FROM v_frames WHERE object = 'NGC 7000'") == 1,
              "v_frames returns the frame");

        // ---- idempotency: re-index must update in place, not duplicate ----
        const auto r2 = idx::store_file(d, info, header, mapping, {-7.0});
        check(r2.file_id == r1.file_id, "re-index keeps the same file id");
        check(scalar(d, "SELECT COUNT(*) FROM files") == 1, "still one files row");
        check(scalar(d, "SELECT COUNT(*) FROM frames") == 1, "still one frames row");
        check(scalar(d, "SELECT COUNT(*) FROM frame_keywords WHERE frame_id = " +
                            std::to_string(frame_id)) == kw, "keywords not duplicated");

        // ---- optional: store a REAL frame, exercising real-header escaping ----
        // A real 68-card header has values with embedded quotes, long strings,
        // and repeated HISTORY/COMMENT cards -- worth running through the batch
        // insert that the synthetic frame does not stress.
        if (const char* real = std::getenv("SB_TEST_FITS"); real && *real) {
            const auto rheader = fits::read_header(real);
            idx::FileInfo ri;
            ri.root_id = root_id;
            ri.rel_path = "lights/real/frame.fits";
            ri.filename = "frame.fits";
            ri.ext = ".fits";
            ri.bucket = "lights";
            const auto rr = idx::store_file(d, ri, rheader, mapping, {-7.0});
            check(rr.frames_written >= 1, "real frame stored");
            const long long rf_id = scalar(d, "SELECT id FROM frames WHERE file_id = " +
                                                   std::to_string(rr.file_id));
            const long long ncards = scalar(d, "SELECT COUNT(*) FROM frame_keywords WHERE frame_id = " +
                                                    std::to_string(rf_id));
            check(ncards == static_cast<long long>(rheader.image_hdus()[0]->cards.size()),
                  "all real header cards persisted (escaping intact)");
            std::cout << "  real frame: " << rr.keywords_written << " cards stored, type="
                      << scalar_s(d, "SELECT image_type FROM frames WHERE id = " +
                                         std::to_string(rf_id)) << "\n";
        }

        // Cleanup leaves the schema; remove our root (cascades to files/frames).
        d.remove_root("idxtest");
        check(scalar(d, "SELECT COUNT(*) FROM frames") == 0, "cascade delete via root removal");

    } catch (const std::exception& e) {
        std::cerr << "index_test: " << e.what() << "\n";
        std::remove(fpath.c_str());
        return 1;
    }
    std::remove(fpath.c_str());

    if (g_failures == 0) { std::cout << "index_test: all checks passed\n"; return 0; }
    std::cerr << "index_test: " << g_failures << " check(s) failed\n";
    return 1;
}
