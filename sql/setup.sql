-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/setup.sql
-- Purpose:       Create the StarBase database and application user. Run this
--                once, as an administrative user, before sql/schema.sql.
-- Created:       2026-07-23
-- Last Modified: 2026-07-23
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
--
-- StarBase shares the MariaDB *instance* with NightWatcher2 but nothing else:
-- its own database, its own user, no shared tables and no foreign keys across
-- the two. Either project can be dropped without touching the other.
--
-- Usage:
--   sudo mariadb < sql/setup.sql          (edit the password first, or use the
--                                          .deb, which prompts via debconf)
--   mariadb -u starbase -p starbase < sql/schema.sql
--
-- The application never reads a password from a config file; starbased and
-- starbasectl take it from the SB_DB_PASSWORD environment variable.

CREATE DATABASE IF NOT EXISTS starbase
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- Change this password. The .deb prompts for it and rewrites this statement.
CREATE USER IF NOT EXISTS 'starbase'@'localhost'
    IDENTIFIED BY 'changeme';

-- Uncomment to allow the daemon to run on a different host from the database.
-- CREATE USER IF NOT EXISTS 'starbase'@'%' IDENTIFIED BY 'changeme';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, INDEX, ALTER, REFERENCES,
    CREATE VIEW, SHOW VIEW
    ON starbase.* TO 'starbase'@'localhost';

-- StarBase reads NightWatcher2's sky-brightness readings when the operator opts
-- in to backfilling SQM for frames that nightwatcher-ingest did not stamp.
-- Read-only, one table, and entirely optional: leave it commented out if you do
-- not want the coupling.
-- GRANT SELECT ON nightwatcher.readings TO 'starbase'@'localhost';

FLUSH PRIVILEGES;
