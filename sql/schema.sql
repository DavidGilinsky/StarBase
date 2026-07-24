-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/schema.sql
-- Purpose:       MariaDB schema: roots and files, frames and their full FITS
--                headers, the equipment registry (sites/cameras/rigs/filters),
--                tags and collections, saved queries, calibration rules, the
--                job engine, and auth.
-- Created:       2026-07-23
-- Last Modified: 2026-07-23
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
-- StarBase database schema (MariaDB / MySQL).
--
--   (load order: setup.sql first, then this file)
--
-- Conventions:
--   * All timestamps are UTC; the application supplies UTC values.
--   * Stored units are SI (degrees, seconds, mm, um, deg C).
--   * frame_keywords keeps every header card verbatim, so any keyword not
--     promoted to a column is still queryable and can be backfilled later
--     without recreating tables. Same reasoning as NightWatcher2's raw_line.
--   * Sizing note: the reference archive is ~8.4 TB of mostly 62 MP frames,
--     which is on the order of 70k frames and ~7M header cards. Everything
--     here is sized for InnoDB comfortably handling 10x that.

USE starbase;

-- ---------------------------------------------------------------------------
-- Schema versioning
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS schema_version (
    version     INT          NOT NULL,
    applied_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    note        VARCHAR(255) NULL,
    PRIMARY KEY (version)
) ENGINE=InnoDB;

INSERT IGNORE INTO schema_version (version, note) VALUES (1, 'initial schema');

-- ---------------------------------------------------------------------------
-- Equipment registry
--
-- These tables exist because the FITS headers cannot be trusted to name the
-- equipment consistently. TELESCOP is usually the *mount*, not the optics.
-- TheSkyX writes a generic INSTRUME of 'ASICamera'. NINA writes the full
-- marketing name. So StarBase normalizes: raw header strings are matched
-- against alias patterns to resolve a camera, and camera + focal length
-- resolves a rig. Everything is editable in the UI; nothing is compiled in.
-- ---------------------------------------------------------------------------

-- Observatory sites. Used for the local observing-night rollover and for
-- attributing frames to a site when the header carries coordinates.
CREATE TABLE IF NOT EXISTS sites (
    id             INT          NOT NULL AUTO_INCREMENT,
    name           VARCHAR(64)  NOT NULL,
    latitude       DECIMAL(9,6) NULL,
    longitude      DECIMAL(9,6) NULL,          -- signed, east positive
    elevation_m    DECIMAL(7,1) NULL,
    timezone       VARCHAR(64)  NULL,          -- IANA tz, e.g. 'America/Phoenix'
    utc_offset_h   DECIMAL(4,2) NOT NULL DEFAULT 0,  -- used for night rollover
    match_tol_deg  DECIMAL(5,3) NOT NULL DEFAULT 0.050,
    sqm_sensor     VARCHAR(32)  NULL,          -- NightWatcher2 sensor id, e.g. 'DSN036'
    is_default     TINYINT(1)   NOT NULL DEFAULT 0,
    notes          TEXT         NULL,
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_sites_name (name)
) ENGINE=InnoDB;

-- Normalized camera models. The archive keys calibration on the base model
-- ('ASI6200'), not the marketing string; colour vs mono comes from BAYERPAT.
CREATE TABLE IF NOT EXISTS cameras (
    id             INT          NOT NULL AUTO_INCREMENT,
    model          VARCHAR(64)  NOT NULL,      -- canonical, e.g. 'ASI6200'
    vendor         VARCHAR(64)  NULL,
    sensor         VARCHAR(64)  NULL,
    is_color       TINYINT(1)   NULL,
    bayer_pattern  VARCHAR(8)   NULL,          -- 'RGGB', 'GRBG', ... NULL = mono
    pixel_size_um  DECIMAL(6,3) NULL,
    width_px       INT          NULL,
    height_px      INT          NULL,
    notes          TEXT         NULL,
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_cameras_model (model)
) ENGINE=InnoDB;

-- Substring patterns matched (case-insensitively, by descending priority)
-- against the raw INSTRUME/CAMERA header value to resolve a camera.
-- Seed rows: 'ASICamera' -> ASI6200 (TheSkyX), 'ASI6200' -> ASI6200, etc.
CREATE TABLE IF NOT EXISTS camera_aliases (
    id         INT          NOT NULL AUTO_INCREMENT,
    pattern    VARCHAR(128) NOT NULL,
    camera_id  INT          NOT NULL,
    priority   INT          NOT NULL DEFAULT 100,
    PRIMARY KEY (id),
    UNIQUE KEY uk_camera_alias (pattern),
    KEY idx_camera_alias_prio (priority),
    CONSTRAINT fk_camera_alias_camera FOREIGN KEY (camera_id)
        REFERENCES cameras (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Optics. Separate from rigs because one scope may be paired with several
-- cameras, and because focal length changes with a reducer or flattener.
CREATE TABLE IF NOT EXISTS telescopes (
    id            INT          NOT NULL AUTO_INCREMENT,
    name          VARCHAR(64)  NOT NULL,
    aperture_mm   DECIMAL(7,1) NULL,
    focal_len_mm  DECIMAL(7,1) NULL,
    notes         TEXT         NULL,
    created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_telescopes_name (name)
) ENGINE=InnoDB;

-- A rig is a camera on an optical train: the unit that flats belong to.
-- Resolution rule: match camera_id, then focal_len_mm within
-- [focal_min_mm, focal_max_mm]. This mirrors what nightwatcher-ingest already
-- does, because TELESCOP is the mount and is not usable for this.
CREATE TABLE IF NOT EXISTS rigs (
    id                 INT          NOT NULL AUTO_INCREMENT,
    name               VARCHAR(64)  NOT NULL,   -- e.g. 'Askar185-ASI6200'
    camera_id          INT          NOT NULL,
    telescope_id       INT          NULL,
    site_id            INT          NULL,
    focal_min_mm       DECIMAL(7,1) NOT NULL,
    focal_max_mm       DECIMAL(7,1) NOT NULL,
    pixel_scale_arcsec DECIMAL(6,3) NULL,       -- derived, stored for grouping
    status             ENUM('active','retired') NOT NULL DEFAULT 'active',
    notes              TEXT         NULL,
    created_at         DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_rigs_name (name),
    KEY idx_rigs_resolve (camera_id, focal_min_mm, focal_max_mm),
    CONSTRAINT fk_rigs_camera FOREIGN KEY (camera_id)
        REFERENCES cameras (id) ON DELETE RESTRICT,
    CONSTRAINT fk_rigs_telescope FOREIGN KEY (telescope_id)
        REFERENCES telescopes (id) ON DELETE SET NULL,
    CONSTRAINT fk_rigs_site FOREIGN KEY (site_id)
        REFERENCES sites (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- Canonical filters. Capture apps write 'Ha', 'H-alpha', 'Halpha', 'S2', 'SII'
-- for the same physical filter, and matching flats to lights depends on getting
-- this right.
CREATE TABLE IF NOT EXISTS filters (
    id             INT          NOT NULL AUTO_INCREMENT,
    name           VARCHAR(32)  NOT NULL,       -- canonical, e.g. 'Ha'
    display_name   VARCHAR(64)  NULL,
    band           ENUM('broadband','narrowband','clear','other')
                                NOT NULL DEFAULT 'other',
    wavelength_nm  DECIMAL(7,2) NULL,
    bandwidth_nm   DECIMAL(6,2) NULL,
    sort_order     INT          NOT NULL DEFAULT 100,
    notes          TEXT         NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_filters_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS filter_aliases (
    id         INT         NOT NULL AUTO_INCREMENT,
    pattern    VARCHAR(64) NOT NULL,            -- matched case-insensitively
    filter_id  INT         NOT NULL,
    priority   INT         NOT NULL DEFAULT 100,
    PRIMARY KEY (id),
    UNIQUE KEY uk_filter_alias (pattern),
    CONSTRAINT fk_filter_alias_filter FOREIGN KEY (filter_id)
        REFERENCES filters (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Header mapping
--
-- Which FITS keywords feed which logical field, and how raw values normalize.
-- In the database rather than a config file so it is editable in the UI and
-- versioned with the rest of the index. db-init seeds it from
-- config/headermap.conf with defaults for NINA, TheSkyX, and the ASIAIR.
-- ---------------------------------------------------------------------------

-- Ordered candidate keywords per logical field. Lower priority wins.
-- e.g. field 'gain'   -> GAIN(10), GAINRAW(20)      [TheSkyX writes GAINRAW]
--      field 'offset' -> OFFSET(10), BLKLEVEL(20)
--      field 'exposure_s' -> EXPTIME(10), EXPOSURE(20)
--      field 'latitude'   -> OBSGEO-B(10), SITELAT(20), LAT-OBS(30)
CREATE TABLE IF NOT EXISTS header_map (
    id        INT         NOT NULL AUTO_INCREMENT,
    field     VARCHAR(32) NOT NULL,             -- logical field name
    keyword   VARCHAR(72) NOT NULL,             -- FITS keyword (HIERARCH-capable)
    priority  INT         NOT NULL DEFAULT 100,
    enabled   TINYINT(1)  NOT NULL DEFAULT 1,
    notes     VARCHAR(255) NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_header_map (field, keyword),
    KEY idx_header_map_field (field, priority)
) ENGINE=InnoDB;

-- Raw value -> normalized value, per field.
-- e.g. field 'image_type': 'Light Frame'->light, 'MasterDark'->master
CREATE TABLE IF NOT EXISTS header_value_map (
    id          INT          NOT NULL AUTO_INCREMENT,
    field       VARCHAR(32)  NOT NULL,
    raw_value   VARCHAR(128) NOT NULL,
    normalized  VARCHAR(64)  NOT NULL,
    match_mode  ENUM('exact','contains','regex') NOT NULL DEFAULT 'exact',
    priority    INT          NOT NULL DEFAULT 100,
    PRIMARY KEY (id),
    UNIQUE KEY uk_header_value_map (field, raw_value, match_mode),
    KEY idx_header_value_field (field, priority)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Roots and files
-- ---------------------------------------------------------------------------

-- A monitored directory tree.
--
-- watch_mode: the scheduled sweep is always authoritative. inotify is only an
-- accelerator and is unreliable on NFS and SMB, where it cannot see writes made
-- by another host at all. 'auto' enables it only for local filesystems.
CREATE TABLE IF NOT EXISTS roots (
    id                INT           NOT NULL AUTO_INCREMENT,
    label             VARCHAR(64)   NOT NULL,
    path              VARCHAR(1024) NOT NULL,   -- absolute
    path_hash         BINARY(16)    NOT NULL,   -- for a workable UNIQUE key
    enabled           TINYINT(1)    NOT NULL DEFAULT 1,
    writable          TINYINT(1)    NOT NULL DEFAULT 0,  -- gate on destructive actions
    watch_mode        ENUM('auto','inotify','poll','off') NOT NULL DEFAULT 'auto',
    -- Whether this root's filesystem distinguishes case. The reference archive
    -- lives on a ZFS dataset created with casesensitivity=insensitive, where
    -- 'Lights/M31' and 'lights/M31' are the same directory. rel_path_hash is
    -- computed over the raw string and would therefore produce two rows for one
    -- file; when this is 0 the scanner case-folds the path before hashing.
    -- Detected at scan time, overridable here.
    case_sensitive    TINYINT(1)    NOT NULL DEFAULT 1,
    scan_interval_s   INT           NOT NULL DEFAULT 3600,
    settle_seconds    INT           NOT NULL DEFAULT 30,  -- ignore files younger than this
    ignore_globs      TEXT          NULL,       -- newline separated; default: _gsdata_, *.tmp, *.part
    fs_type           VARCHAR(32)   NULL,       -- observed at scan time: ext4, nfs, cifs
    last_scan_start   DATETIME      NULL,
    last_scan_end     DATETIME      NULL,
    last_scan_status  ENUM('never','running','ok','error') NOT NULL DEFAULT 'never',
    last_scan_error   VARCHAR(512)  NULL,
    file_count        BIGINT        NOT NULL DEFAULT 0,
    created_at        DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_roots_label (label),
    UNIQUE KEY uk_roots_path (path_hash)
) ENGINE=InnoDB;

-- One row per file on disk.
--
-- Deliberately absent: st_dev. It is a client-side mount identity that changes
-- across a remount or reboot, so keying on it would make every file on the NFS
-- archive look new after a reboot. Identity is (root_id, rel_path); st_ino is
-- kept only as a cheap change-detection hint.
--
-- Files that disappear become status='missing' and are retained with their
-- last_seen_utc, so history, tags, and collection membership are not silently
-- lost when a volume is offline.
CREATE TABLE IF NOT EXISTS files (
    id               BIGINT        NOT NULL AUTO_INCREMENT,
    root_id          INT           NOT NULL,
    rel_path         VARCHAR(1024) NOT NULL,
    rel_path_hash    BINARY(16)    NOT NULL,    -- UNIQUE on a 1 KB path is not indexable
    filename         VARCHAR(255)  NOT NULL,
    ext              VARCHAR(16)   NULL,
    format           ENUM('fits','xisf','other') NOT NULL DEFAULT 'other',
    -- Archive bucket, derived from the path under the root. nightwatcher-ingest
    -- files into lights/, calibration/, process/, review/, quarantine/,
    -- incoming/; all six are indexed.
    bucket           ENUM('lights','calibration','process','review',
                          'quarantine','incoming','other') NOT NULL DEFAULT 'other',
    size_bytes       BIGINT        NULL,
    mtime_utc        DATETIME(6)   NULL,        -- NFS gives ns; MariaDB stores us
    inode            BIGINT        NULL,        -- hint only, see note above
    status           ENUM('pending','ok','error','missing') NOT NULL DEFAULT 'pending',
    error            VARCHAR(512)  NULL,
    first_seen_utc   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_utc    DATETIME      NULL,
    last_indexed_utc DATETIME      NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_files_path (root_id, rel_path_hash),
    KEY idx_files_status (status),
    KEY idx_files_bucket (root_id, bucket),
    KEY idx_files_filename (filename),
    KEY idx_files_mtime (mtime_utc),
    KEY idx_files_inode (root_id, inode),
    CONSTRAINT fk_files_root FOREIGN KEY (root_id)
        REFERENCES roots (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Frames
--
-- One row per image HDU. Everything the UI filters, sorts, or groups on is a
-- promoted column here; the complete header is still in frame_keywords.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS frames (
    id                 BIGINT       NOT NULL AUTO_INCREMENT,
    file_id            BIGINT       NOT NULL,
    hdu                INT          NOT NULL DEFAULT 0,

    -- Immutable identity fingerprint: a hash over the header cards that cannot
    -- change for a given exposure --
    --   DATE-OBS | INSTRUME | EXPTIME | NAXIS1 | NAXIS2 | IMAGETYP | XBINNING
    -- Deliberately NOT a whole-file hash: nightwatcher-ingest stamps SQM,
    -- SQMSRC, SQMTIME, SQMDT and a defaulted FILTER into frames after the fact,
    -- so a content hash would change on a file that is otherwise the same
    -- exposure. Also, hashing 8.4 TB over NFS on every sweep is not viable.
    -- This costs nothing, since the header is already parsed.
    --
    -- NOT declared UNIQUE: a genuine duplicate capture is possible in theory.
    -- Move detection requires exactly one match; ambiguity is reported, never
    -- guessed at.
    fingerprint        BINARY(16)   NOT NULL,

    -- Classification. 'master' covers MasterDark/MasterFlat/integrations, which
    -- live under process/ and which WBPP prefers over raw stacks when present.
    image_type         ENUM('light','dark','flat','bias','darkflat',
                            'master','unknown') NOT NULL DEFAULT 'unknown',
    master_of          ENUM('light','dark','flat','bias','darkflat') NULL,
    -- Why a frame was set aside rather than filed as science.
    review_reason      VARCHAR(32)  NULL,   -- focus|autofocus|slew|preview|live|failed|platesolve|short|aux
    quarantine_reason  VARCHAR(32)  NULL,   -- no-dateobs|no-object|unknown-type|unreadable

    -- Target
    object             VARCHAR(128) NULL,   -- as written in the header
    object_canonical   VARCHAR(64)  NULL,   -- after aliasing, e.g. NGC 224 -> M31
    ra_deg             DECIMAL(9,6) NULL,
    dec_deg            DECIMAL(9,6) NULL,

    -- Time. ASI cameras write 7 sub-second digits in DATE-OBS; parsing
    -- truncates to the microseconds MariaDB stores.
    date_obs_utc       DATETIME(6)  NULL,
    -- Local noon-to-noon observing night. Stored, not computed at query time:
    -- grouping by night is the single most common thing asked of this data.
    session_night      DATE         NULL,
    exposure_s         DECIMAL(10,3) NULL,

    -- Equipment, resolved
    rig_id             INT          NULL,
    camera_id          INT          NULL,
    site_id            INT          NULL,
    filter_id          INT          NULL,
    -- ...and as written, so a bad alias is diagnosable without a rescan
    filter_raw         VARCHAR(32)  NULL,
    filter_defaulted   TINYINT(1)   NOT NULL DEFAULT 0,  -- CLEAR injected, not observed
    instrume_raw       VARCHAR(128) NULL,
    telescope_raw      VARCHAR(128) NULL,   -- usually the mount; do not trust
    focal_len_mm       DECIMAL(7,1) NULL,
    focal_ratio        DECIMAL(5,2) NULL,   -- FOCRATIO
    aperture_mm        DECIMAL(7,1) NULL,   -- APTDIA
    pixel_size_um      DECIMAL(6,3) NULL,   -- XPIXSZ, 100% present
    -- EGAIN, electrons per ADU (85% present). Needed for any real noise or
    -- weighting calculation later, and it varies with gain.
    egain_e_per_adu    DECIMAL(9,6) NULL,

    -- Capture parameters. These are the calibration-matching key.
    gain               INT          NULL,
    offset_adu         INT          NULL,
    readout_mode       VARCHAR(32)  NULL,
    binx               TINYINT      NULL,
    biny               TINYINT      NULL,
    naxis1             INT          NULL,
    naxis2             INT          NULL,
    bayer_pattern      VARCHAR(8)   NULL,
    -- Both temperatures, kept separate on purpose: matching uses the setpoint,
    -- quality triage uses the actual, and a large gap between them is itself a
    -- useful thing to query for.
    ccd_temp_c         DECIMAL(5,2) NULL,
    set_temp_c         DECIMAL(5,2) NULL,

    -- Mid-exposure time. NINA writes DATE-AVG/MJD-AVG on 65% of frames; for a
    -- 300 s sub the midpoint is the honest timestamp for anything time-ordered.
    date_avg_utc       DATETIME(6)  NULL,
    mjd_obs            DECIMAL(14,8) NULL,

    -- Pier side, from PIERSIDE. Observed 33 East / 30 West in the sample, so
    -- meridian flips are routine here. Registration and WBPP grouping care,
    -- because the field is rotated 180 degrees across a flip.
    pier_side          ENUM('east','west') NULL,
    -- ROWORDER, universally TOP-DOWN in this archive. Feeds WBPP's
    -- fitsCoordinateConvention; getting it wrong flips every image vertically.
    row_order          ENUM('top-down','bottom-up') NULL,
    equinox            DECIMAL(7,2) NULL,
    -- True when the header carries a WCS solution (CTYPE1/CRVAL1/CD matrix).
    -- Only ~3% of frames are solved, and "already solved" is worth querying.
    has_wcs            TINYINT(1)   NOT NULL DEFAULT 0,

    -- Observing conditions
    airmass            DECIMAL(6,3) NULL,
    altitude_deg       DECIMAL(6,2) NULL,   -- CENTALT
    azimuth_deg        DECIMAL(6,2) NULL,   -- CENTAZ
    moon_phase         DECIMAL(5,3) NULL,
    moon_sep_deg       DECIMAL(6,2) NULL,   -- MOONANGL
    sun_angle_deg      DECIMAL(6,2) NULL,   -- SUNANGLE, for twilight triage
    -- Sky brightness, stamped into the header by nightwatcher-ingest from
    -- NightWatcher2. Promoted because "lights of M31 under darker than 21.0"
    -- is a query only this ecosystem can answer.
    sqm_mag_arcsec2    DECIMAL(6,3) NULL,
    sqm_sensor         VARCHAR(32)  NULL,
    sqm_time_utc       DATETIME     NULL,
    sqm_dt_s           INT          NULL,   -- seconds between reading and DATE-OBS

    -- Focus, rotation, guiding
    focus_pos          INT          NULL,
    focus_temp_c       DECIMAL(5,2) NULL,
    rotator_deg        DECIMAL(6,2) NULL,
    guide_rms_arcsec   DECIMAL(6,3) NULL,

    -- Mosaic support: columns populated when the header carries them, no UI yet.
    panel              VARCHAR(32)  NULL,
    session_label      VARCHAR(32)  NULL,

    -- Provenance
    observer           VARCHAR(64)  NULL,
    capture_software   VARCHAR(64)  NULL,   -- SWCREATE / PROGRAM

    created_at         DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at         DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                                    ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_frames_hdu (file_id, hdu),

    -- Move and duplicate detection.
    KEY idx_frames_fingerprint (fingerprint),

    -- The primary browse/query path: "lights of M31 in Ha, newest first".
    KEY idx_frames_light (image_type, object_canonical, filter_id, date_obs_utc),
    -- "everything shot on the night of ..."
    KEY idx_frames_night (session_night, image_type),
    -- Dark / bias / dark-flat matching.
    KEY idx_frames_calib_dark (image_type, camera_id, gain, offset_adu,
                               binx, biny, set_temp_c, exposure_s, date_obs_utc),
    -- Flat matching, which keys on the optical train rather than the camera.
    KEY idx_frames_calib_flat (image_type, rig_id, filter_id, binx, biny, date_obs_utc),
    -- Cone search prefilter. Declination first: it is the tighter constraint
    -- and, unlike RA, it does not wrap.
    KEY idx_frames_radec (dec_deg, ra_deg),
    KEY idx_frames_rig_night (rig_id, session_night),
    KEY idx_frames_exposure (exposure_s),
    KEY idx_frames_sqm (sqm_mag_arcsec2),
    KEY idx_frames_object (object_canonical),

    CONSTRAINT fk_frames_file FOREIGN KEY (file_id)
        REFERENCES files (id) ON DELETE CASCADE,
    CONSTRAINT fk_frames_rig FOREIGN KEY (rig_id)
        REFERENCES rigs (id) ON DELETE SET NULL,
    CONSTRAINT fk_frames_camera FOREIGN KEY (camera_id)
        REFERENCES cameras (id) ON DELETE SET NULL,
    CONSTRAINT fk_frames_site FOREIGN KEY (site_id)
        REFERENCES sites (id) ON DELETE SET NULL,
    CONSTRAINT fk_frames_filter FOREIGN KEY (filter_id)
        REFERENCES filters (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- Every header card, verbatim and in order, comments included. This is the
-- escape hatch: anything not promoted above is still queryable today and can be
-- promoted to a column later without a rescan.
CREATE TABLE IF NOT EXISTS frame_keywords (
    frame_id   BIGINT       NOT NULL,
    ord        SMALLINT     NOT NULL,          -- position in the header
    keyword    VARCHAR(72)  NOT NULL,          -- 8 chars for FITS, more for HIERARCH
    value      VARCHAR(256) NULL,
    comment    VARCHAR(256) NULL,
    PRIMARY KEY (frame_id, ord),
    KEY idx_frame_kw (keyword, value(64)),
    CONSTRAINT fk_frame_kw_frame FOREIGN KEY (frame_id)
        REFERENCES frames (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Sidecar and product files sitting alongside frames: capture logs, CSVs,
-- autofocus reports, XISF products. Recorded and associated, not parsed.
CREATE TABLE IF NOT EXISTS artifacts (
    id            BIGINT        NOT NULL AUTO_INCREMENT,
    root_id       INT           NOT NULL,
    rel_path      VARCHAR(1024) NOT NULL,
    rel_path_hash BINARY(16)    NOT NULL,
    filename      VARCHAR(255)  NOT NULL,
    kind          ENUM('sidecar','log','csv','product','other') NOT NULL DEFAULT 'other',
    frame_id      BIGINT        NULL,          -- associated frame, when known
    size_bytes    BIGINT        NULL,
    mtime_utc     DATETIME(6)   NULL,
    first_seen_utc DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_utc  DATETIME     NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_artifacts_path (root_id, rel_path_hash),
    KEY idx_artifacts_frame (frame_id),
    CONSTRAINT fk_artifacts_root FOREIGN KEY (root_id)
        REFERENCES roots (id) ON DELETE CASCADE,
    CONSTRAINT fk_artifacts_frame FOREIGN KEY (frame_id)
        REFERENCES frames (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Tags and collections
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS tags (
    id          INT         NOT NULL AUTO_INCREMENT,
    name        VARCHAR(64) NOT NULL,
    color       VARCHAR(16) NULL,
    description VARCHAR(255) NULL,
    created_at  DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_tags_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS frame_tags (
    frame_id   BIGINT   NOT NULL,
    tag_id     INT      NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by INT      NULL,
    PRIMARY KEY (frame_id, tag_id),
    KEY idx_frame_tags_tag (tag_id),
    CONSTRAINT fk_frame_tags_frame FOREIGN KEY (frame_id)
        REFERENCES frames (id) ON DELETE CASCADE,
    CONSTRAINT fk_frame_tags_tag FOREIGN KEY (tag_id)
        REFERENCES tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- A curated set of frames: a project, a candidate stack, a reprocessing set.
CREATE TABLE IF NOT EXISTS collections (
    id          INT          NOT NULL AUTO_INCREMENT,
    name        VARCHAR(128) NOT NULL,
    description TEXT         NULL,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                             ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_collections_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS collection_frames (
    collection_id INT      NOT NULL,
    frame_id      BIGINT   NOT NULL,
    position      INT      NOT NULL DEFAULT 0,
    added_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (collection_id, frame_id),
    KEY idx_collection_frames_frame (frame_id),
    CONSTRAINT fk_coll_frames_coll FOREIGN KEY (collection_id)
        REFERENCES collections (id) ON DELETE CASCADE,
    CONSTRAINT fk_coll_frames_frame FOREIGN KEY (frame_id)
        REFERENCES frames (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Saved queries
-- ---------------------------------------------------------------------------

-- filter_json is the filter AST the UI builds and the query engine compiles to
-- parameterized SQL. Stored as the AST, never as SQL text.
CREATE TABLE IF NOT EXISTS saved_queries (
    id           INT          NOT NULL AUTO_INCREMENT,
    name         VARCHAR(128) NOT NULL,
    description  TEXT         NULL,
    filter_json  LONGTEXT     NOT NULL,
    sort_json    LONGTEXT     NULL,
    columns_json LONGTEXT     NULL,
    owner_id     INT          NULL,
    last_run_at  DATETIME     NULL,
    last_count   BIGINT       NULL,
    created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                              ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_saved_queries_name (name)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Calibration matching
--
-- Declarative and editable, because a calibration match that cannot be
-- explained is a match that cannot be trusted. match_json holds the per-field
-- rule, e.g.
--   {"camera":"exact","gain":"exact","offset_adu":"exact",
--    "binx":"exact","biny":"exact","set_temp_c":{"tol":1.0},
--    "exposure_s":{"tol":5.0}}
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS calibration_rules (
    id                  INT          NOT NULL AUTO_INCREMENT,
    name                VARCHAR(64)  NOT NULL,
    target_type         ENUM('dark','flat','bias','darkflat') NOT NULL,
    enabled             TINYINT(1)   NOT NULL DEFAULT 1,
    priority            INT          NOT NULL DEFAULT 100,
    match_json          LONGTEXT     NOT NULL,
    max_age_days        INT          NULL,      -- NULL = no age limit
    prefer_masters      TINYINT(1)   NOT NULL DEFAULT 1,
    prefer_same_session TINYINT(1)   NOT NULL DEFAULT 0,
    min_frames          INT          NULL,      -- warn below this count
    notes               TEXT         NULL,
    created_at          DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                                     ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_calib_rules_name (name),
    KEY idx_calib_rules_type (target_type, enabled, priority)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Job engine
--
-- Every action is an audited, resumable job. Destructive types default to a dry
-- run, and deletes move to a trash directory rather than unlinking.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS jobs (
    id           BIGINT       NOT NULL AUTO_INCREMENT,
    type         ENUM('scan','stage','wbpp','fsop','tag','exec','export','verify')
                              NOT NULL,
    status       ENUM('pending','running','done','failed','cancelled')
                              NOT NULL DEFAULT 'pending',
    dry_run      TINYINT(1)   NOT NULL DEFAULT 1,
    query_id     INT          NULL,           -- saved query the set came from
    params_json  LONGTEXT     NULL,
    total_items  BIGINT       NOT NULL DEFAULT 0,
    done_items   BIGINT       NOT NULL DEFAULT 0,
    failed_items BIGINT       NOT NULL DEFAULT 0,
    created_by   INT          NULL,
    created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    started_at   DATETIME     NULL,
    finished_at  DATETIME     NULL,
    error        VARCHAR(512) NULL,
    PRIMARY KEY (id),
    KEY idx_jobs_status (status, type),
    KEY idx_jobs_created (created_at),
    CONSTRAINT fk_jobs_query FOREIGN KEY (query_id)
        REFERENCES saved_queries (id) ON DELETE SET NULL
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS job_items (
    id          BIGINT        NOT NULL AUTO_INCREMENT,
    job_id      BIGINT        NOT NULL,
    frame_id    BIGINT        NULL,
    action      VARCHAR(32)   NOT NULL,        -- move|copy|symlink|link|trash|rename|exec|stage
    src_path    VARCHAR(1024) NULL,
    dst_path    VARCHAR(1024) NULL,
    status      ENUM('pending','ok','skipped','failed') NOT NULL DEFAULT 'pending',
    detail      VARCHAR(512)  NULL,
    finished_at DATETIME      NULL,
    PRIMARY KEY (id),
    KEY idx_job_items_job (job_id, status),
    KEY idx_job_items_frame (frame_id),
    CONSTRAINT fk_job_items_job FOREIGN KEY (job_id)
        REFERENCES jobs (id) ON DELETE CASCADE,
    CONSTRAINT fk_job_items_frame FOREIGN KEY (frame_id)
        REFERENCES frames (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- Allow-listed external tools. The web UI can only run what is registered here;
-- commands are executed as an argv array with no shell interpretation.
CREATE TABLE IF NOT EXISTS tools (
    id          INT          NOT NULL AUTO_INCREMENT,
    name        VARCHAR(64)  NOT NULL,
    description VARCHAR(255) NULL,
    argv_json   LONGTEXT     NOT NULL,        -- template argv, with {path} etc.
    input_mode  ENUM('per_frame','list_file','argv') NOT NULL DEFAULT 'per_frame',
    timeout_s   INT          NOT NULL DEFAULT 300,
    enabled     TINYINT(1)   NOT NULL DEFAULT 0,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_tools_name (name)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Scan history and audit
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS scan_log (
    id             BIGINT       NOT NULL AUTO_INCREMENT,
    root_id        INT          NOT NULL,
    trigger_source ENUM('schedule','manual','watch','startup') NOT NULL,
    started_at     DATETIME     NOT NULL,
    ended_at       DATETIME     NULL,
    duration_ms    BIGINT       NULL,
    files_seen     BIGINT       NOT NULL DEFAULT 0,
    files_added    BIGINT       NOT NULL DEFAULT 0,
    files_updated  BIGINT       NOT NULL DEFAULT 0,
    files_moved    BIGINT       NOT NULL DEFAULT 0,
    files_missing  BIGINT       NOT NULL DEFAULT 0,
    files_error    BIGINT       NOT NULL DEFAULT 0,
    status         ENUM('running','ok','error','cancelled') NOT NULL DEFAULT 'running',
    error          VARCHAR(512) NULL,
    PRIMARY KEY (id),
    KEY idx_scan_log_root (root_id, started_at),
    CONSTRAINT fk_scan_log_root FOREIGN KEY (root_id)
        REFERENCES roots (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS events (
    id      BIGINT       NOT NULL AUTO_INCREMENT,
    ts_utc  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    source  VARCHAR(32)  NOT NULL,            -- scanner|api|actions|auth|pix
    level   ENUM('debug','info','warning','error') NOT NULL DEFAULT 'info',
    event   VARCHAR(64)  NOT NULL,
    detail  VARCHAR(512) NULL,
    user_id INT          NULL,
    PRIMARY KEY (id),
    KEY idx_events_ts (ts_utc),
    KEY idx_events_source (source, level)
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Authentication (NightWatcher2 pattern: PBKDF2, own users table)
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS users (
    id                   INT          NOT NULL AUTO_INCREMENT,
    username             VARCHAR(64)  NOT NULL,
    pwd_hash             VARBINARY(64) NOT NULL,
    pwd_salt             VARBINARY(32) NOT NULL,
    pwd_iterations       INT          NOT NULL,
    role                 ENUM('admin','user','readonly') NOT NULL DEFAULT 'user',
    enabled              TINYINT(1)   NOT NULL DEFAULT 1,
    must_change_password TINYINT(1)   NOT NULL DEFAULT 0,
    created_at           DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login_at        DATETIME     NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_users_username (username)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS sessions (
    token       CHAR(64)     NOT NULL,
    user_id     INT          NOT NULL,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at  DATETIME     NOT NULL,
    remote_addr VARCHAR(64)  NULL,
    user_agent  VARCHAR(255) NULL,
    PRIMARY KEY (token),
    KEY idx_sessions_user (user_id),
    KEY idx_sessions_expires (expires_at),
    CONSTRAINT fk_sessions_user FOREIGN KEY (user_id)
        REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ---------------------------------------------------------------------------
-- Views
-- ---------------------------------------------------------------------------

-- The grid the UI browses: one row per frame with equipment names resolved and
-- the absolute path assembled.
CREATE OR REPLACE VIEW v_frames AS
SELECT
    fr.id                      AS frame_id,
    f.id                       AS file_id,
    r.label                    AS root_label,
    CONCAT(r.path, '/', f.rel_path) AS abs_path,
    f.filename,
    f.bucket,
    f.size_bytes,
    f.status                   AS file_status,
    fr.image_type,
    fr.master_of,
    fr.review_reason,
    fr.quarantine_reason,
    fr.object_canonical,
    fr.object,
    fr.ra_deg,
    fr.dec_deg,
    fr.date_obs_utc,
    fr.session_night,
    fr.exposure_s,
    rg.name                    AS rig,
    cam.model                  AS camera,
    st.name                    AS site,
    COALESCE(fl.name, fr.filter_raw) AS filter,
    fr.filter_defaulted,
    fr.gain,
    fr.offset_adu,
    fr.binx,
    fr.biny,
    fr.ccd_temp_c,
    fr.set_temp_c,
    fr.airmass,
    fr.sqm_mag_arcsec2,
    fr.focus_pos,
    fr.rotator_deg,
    fr.guide_rms_arcsec
FROM frames fr
JOIN files   f   ON f.id  = fr.file_id
JOIN roots   r   ON r.id  = f.root_id
LEFT JOIN rigs      rg  ON rg.id  = fr.rig_id
LEFT JOIN cameras   cam ON cam.id = fr.camera_id
LEFT JOIN sites     st  ON st.id  = fr.site_id
LEFT JOIN filters   fl  ON fl.id  = fr.filter_id;

-- What is on hand, by night and configuration. Drives the dashboard and answers
-- "do I have flats for that session".
CREATE OR REPLACE VIEW v_frame_summary AS
SELECT
    fr.session_night,
    fr.image_type,
    fr.object_canonical,
    rg.name          AS rig,
    fl.name          AS filter,
    COUNT(*)         AS frame_count,
    SUM(fr.exposure_s) AS total_exposure_s,
    MIN(fr.date_obs_utc) AS first_obs_utc,
    MAX(fr.date_obs_utc) AS last_obs_utc
FROM frames fr
LEFT JOIN rigs    rg ON rg.id = fr.rig_id
LEFT JOIN filters fl ON fl.id = fr.filter_id
GROUP BY fr.session_night, fr.image_type, fr.object_canonical, rg.name, fl.name;
