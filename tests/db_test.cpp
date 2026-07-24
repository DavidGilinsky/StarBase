// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/db_test.cpp
// Purpose:       Integration tests for the MariaDB layer: schema application,
//                idempotency, and root registration.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// Requires a live MariaDB. Skips itself (exit 0) unless SB_TEST_DB_NAME is set,
// so `ctest` stays useful on a machine without a database while CI exercises it
// against a real server.
//
//   SB_TEST_DB_NAME=starbase_test SB_DB_USER=... SB_DB_PASSWORD=... ctest
//
// The named database is MODIFIED: the test drops and recreates its own rows.
// Point it at a scratch database, never at a live index.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "database.hpp"

namespace {

namespace db = starbase::db;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

std::string find_sql(const std::string& name) {
    for (const std::string base : {"sql/", "../sql/", "../../sql/"}) {
        const std::string p = base + name;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) {
        std::cout << "db_test: SB_TEST_DB_NAME not set; skipping\n";
        return 0;
    }

    db::DbConfig cfg = db::DbConfig::from_env();
    cfg.database = test_db;

    const std::string schema = find_sql("schema.sql");
    const std::string seed = find_sql("seed.sql");
    if (schema.empty() || seed.empty()) {
        std::cerr << "db_test: cannot locate sql/schema.sql and sql/seed.sql\n";
        return 1;
    }

    try {
        db::Database d(cfg);
        std::cout << "db_test: connected to " << d.server_version() << "\n";

        // ---- schema ----
        const int stmts = d.apply_script(schema);
        check(stmts > 1, "schema.sql executed multiple statements");
        check(d.table_exists("roots"), "roots table created");
        check(d.table_exists("frames"), "frames table created");
        check(d.table_exists("frame_keywords"), "frame_keywords table created");
        check(d.schema_version() >= 1, "schema_version recorded");

        // The daemon's db/init endpoint and the package postinst both re-apply
        // this to an existing database, so it must be safe to run twice.
        d.apply_script(schema);
        check(d.tables().size() >= 25, "schema is idempotent and complete");

        // ---- seed ----
        d.apply_script(seed);
        const size_t after_first = d.tables().size();
        d.apply_script(seed);
        check(d.tables().size() == after_first, "seed.sql is idempotent");

        // ---- roots ----
        d.remove_root("test-root");  // leftovers from an interrupted run

        db::RootFields f;
        f.fs_type = "nfs4";
        f.case_sensitive = false;
        f.watch_mode = "poll";
        f.scan_interval_s = 1800;
        const int id = d.add_root("test-root", "/tmp/starbase-test-root", f);
        check(id > 0, "add_root returns an id");

        auto found = d.find_root_by_label("test-root");
        check(found.has_value(), "find_root_by_label finds the new root");
        if (found) {
            check(found->path == "/tmp/starbase-test-root", "root path round-trips");
            check(found->fs_type == "nfs4", "fs_type round-trips");
            check(!found->case_sensitive, "case_sensitive=false round-trips");
            check(found->watch_mode == "poll", "watch_mode round-trips");
            check(found->scan_interval_s == 1800, "scan_interval_s round-trips");
            check(found->enabled, "enabled defaults to true");
            check(!found->writable, "writable defaults to false");
            check(found->last_scan_status == "never", "last_scan_status defaults to never");
        }

        // A duplicate label or path must be refused, not silently accepted:
        // two roots over one tree would double-index every file.
        bool threw = false;
        try {
            d.add_root("test-root", "/tmp/other-path", db::RootFields{});
        } catch (const db::DbError&) { threw = true; }
        check(threw, "duplicate label is rejected");

        threw = false;
        try {
            d.add_root("other-label", "/tmp/starbase-test-root", db::RootFields{});
        } catch (const db::DbError&) { threw = true; }
        check(threw, "duplicate path is rejected");

        db::RootFields upd;
        upd.enabled = false;
        upd.writable = true;
        check(d.update_root("test-root", upd), "update_root reports a change");
        found = d.find_root_by_label("test-root");
        check(found && !found->enabled, "enabled updated");
        check(found && found->writable, "writable updated");

        check(d.update_root("test-root", db::RootFields{}) == false,
              "update with no fields reports no change");
        check(d.update_root("no-such-root", upd) == false,
              "update of a missing root reports no change");

        check(d.remove_root("test-root"), "remove_root reports removal");
        check(!d.find_root_by_label("test-root").has_value(), "root is gone");
        check(!d.remove_root("test-root"), "removing a missing root reports nothing");

        // A bad script must name the file it failed on.
        threw = false;
        try { d.apply_script("/nonexistent/does-not-exist.sql"); }
        catch (const db::DbError&) { threw = true; }
        check(threw, "apply_script throws on a missing file");

    } catch (const std::exception& e) {
        std::cerr << "db_test: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) {
        std::cout << "db_test: all checks passed\n";
        return 0;
    }
    std::cerr << "db_test: " << g_failures << " check(s) failed\n";
    return 1;
}
