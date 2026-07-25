// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/db/database.hpp
// Purpose:       RAII wrapper over MariaDB Connector/C (libmariadb): schema
//                setup, root registration, and scan bookkeeping.
// Created:       2026-07-23
// Last Modified: 2026-07-23
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace starbase::db {

// Connection parameters. The password is never defaulted in source and never
// read from the config file; it comes from SB_DB_PASSWORD.
struct DbConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "starbase";
    std::string password;
    std::string database = "starbase";

    // Build from SB_DB_HOST/PORT/USER/PASSWORD/NAME, falling back to defaults.
    static DbConfig from_env();
};

// A monitored directory tree, as stored in the `roots` table.
struct RootRow {
    int         id = 0;
    std::string label;
    std::string path;
    bool        enabled = true;
    bool        writable = false;
    bool        case_sensitive = true;
    std::string watch_mode = "auto";   // auto | inotify | poll | off
    int         scan_interval_s = 3600;
    int         settle_seconds = 30;
    std::string ignore_globs;          // comma/newline separated; empty = default
    std::string fs_type;
    std::string last_scan_status = "never";
    std::string last_scan_end;
    std::string last_scan_error;
    long long   file_count = 0;
};

// Optional fields for creating or partially editing a root. Only members that
// are set are written, so one struct drives both add-root and set-root.
struct RootFields {
    std::optional<bool>        enabled;
    std::optional<bool>        writable;
    std::optional<bool>        case_sensitive;
    std::optional<std::string> watch_mode;
    std::optional<int>         scan_interval_s;
    std::optional<int>         settle_seconds;
    std::optional<std::string> fs_type;
    std::optional<std::string> ignore_globs;
};

// Thrown for any database-level failure, carrying the server's message and the
// MySQL error number so callers can distinguish, e.g., a deadlock (1213) or a
// lock-wait timeout (1205) -- both of which are retryable -- from a real error.
class DbError : public std::runtime_error {
public:
    explicit DbError(const std::string& what, unsigned int err = 0)
        : std::runtime_error(what), errno_(err) {}
    unsigned int db_errno() const { return errno_; }
    // A transient contention error that the documented remedy is to retry.
    bool retryable() const { return errno_ == 1213 || errno_ == 1205; }

private:
    unsigned int errno_ = 0;
};

// One connection. Not thread-safe: each thread opens its own, as in the
// NightWatcher2 API layer.
class Database {
public:
    explicit Database(const DbConfig& cfg);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Server version string, e.g. "10.11.14-MariaDB".
    std::string server_version() const;

    // Escape a string for interpolation. Numeric values in this layer are
    // produced by the code, never taken from user strings.
    std::string escape(const std::string& s) const;

    // One result row: a NULL column is an empty optional.
    using Row = std::vector<std::optional<std::string>>;

    // Run a SELECT and return its rows. For STATIC queries only -- callers pass
    // literal SQL, never interpolated user input. Throws DbError on failure.
    std::vector<Row> query(const std::string& sql);

    // Run an INSERT/UPDATE/DELETE. Returns the AUTO_INCREMENT id of an INSERT
    // (0 if none). String inputs must already be escape()d by the caller.
    // Throws DbError on failure.
    long long exec(const std::string& sql);

    // Rows changed by the last exec().
    long long affected_rows() const;

    // ---- Schema ----

    // Execute a multi-statement .sql file (schema.sql, seed.sql, a migration).
    // Returns the number of statements executed. Throws DbError on the first
    // failure, naming the file. The scripts are written to be idempotent, so
    // this is safe to re-run.
    int apply_script(const std::string& path);

    bool table_exists(const std::string& name);
    std::vector<std::string> tables();
    // Highest applied schema version, or 0 if the table is absent or empty.
    int schema_version();

    // ---- Roots ----

    // Returns the new row id. Throws DbError if the label or path is taken.
    int add_root(const std::string& label, const std::string& path, const RootFields& f);
    std::vector<RootRow> list_roots();
    std::optional<RootRow> find_root_by_label(const std::string& label);
    bool update_root(const std::string& label, const RootFields& f);
    bool remove_root(const std::string& label);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace starbase::db
