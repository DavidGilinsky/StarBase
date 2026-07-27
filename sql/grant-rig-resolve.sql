-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/grant-rig-resolve.sql
-- Purpose:       Grant nightwatcher-ingest read access to StarBase's rig
--                resolution view (v_rig_resolve), the one and only cross-tool
--                coupling. Run as root AFTER both StarBase and NightWatcher are
--                installed: the 'nightwatcher' user must already exist.
-- Created:       2026-07-26
-- Last Modified: 2026-07-26
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
--
-- Usage (idempotent; re-run after either project is reinstalled):
--   sudo mariadb < /usr/local/starbase/sql/grant-rig-resolve.sql
--
-- The dependency points consumer -> StarBase only: this grants the consumer's
-- existing user SELECT on one StarBase view. StarBase gains no access to, and no
-- knowledge of, the nightwatcher database. See docs/INTEGRATIONS.md.

GRANT SELECT ON starbase.v_rig_resolve TO 'nightwatcher'@'localhost';
GRANT SELECT ON starbase.v_rig_resolve TO 'nightwatcher'@'127.0.0.1';
FLUSH PRIVILEGES;

-- Stricter alternative: instead of granting to the shared 'nightwatcher' user,
-- create a dedicated read-only user scoped to just this view, and point the
-- consumer at it.
--   CREATE USER IF NOT EXISTS 'nwingest_ro'@'localhost' IDENTIFIED BY 'change-me';
--   GRANT SELECT ON starbase.v_rig_resolve TO 'nwingest_ro'@'localhost';
--   FLUSH PRIVILEGES;
