-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/seed.sql
-- Purpose:       Default header keyword mapping, value normalization, and
--                calibration rules. Derived from a survey of the live archive,
--                not from the FITS standard in the abstract.
-- Created:       2026-07-23
-- Last Modified: 2026-07-23
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
-- Load after schema.sql. Idempotent (INSERT IGNORE throughout), so it is safe
-- to re-run after an upgrade to pick up new defaults without disturbing
-- anything the operator has edited in the UI.
--
-- Every mapping below was verified against real frames in
-- /astronomy/astro-imaging: 103 files sampled for keyword vocabulary and 4,000
-- for coverage. Percentages in comments are observed presence in that sample,
-- across N.I.N.A. 3.2, TheSky 10.5, ASIAIR, and PixInsight-written masters.
--
-- As with schema.sql, there is deliberately no `USE <database>` here: the
-- target is whatever database the connection already selected.

-- ---------------------------------------------------------------------------
-- Keyword priority. Lower priority wins.
-- ---------------------------------------------------------------------------

INSERT IGNORE INTO header_map (field, keyword, priority, notes) VALUES
-- Time. DATE-OBS is universal (100%). DATE-AVG is the mid-exposure instant and
-- is the better timestamp when present (65%), but DATE-OBS stays the identity
-- field because it is the one that is always there.
  ('date_obs',    'DATE-OBS',  10, '100% of sampled frames'),
  ('date_obs',    'DATE_OBS',  20, 'underscore variant'),
  ('date_avg',    'DATE-AVG',  10, 'mid-exposure, 65%'),
  ('mjd_obs',     'MJD-OBS',   10, '65%'),
  ('date_local',  'DATE-LOC',  10, 'NINA local time, 65%'),

-- Exposure. EXPTIME is universal; EXPOSURE is a NINA duplicate (83%).
  ('exposure_s',  'EXPTIME',   10, '100%'),
  ('exposure_s',  'EXPOSURE',  20, 'NINA duplicate'),

-- Gain and offset. TheSky writes GAINRAW/GAINADU rather than GAIN.
  ('gain',        'GAIN',      10, '83%'),
  ('gain',        'GAINRAW',   20, 'TheSky, 13%'),
  ('gain',        'GAINADU',   30, 'TheSky'),
  ('egain',       'EGAIN',     10, 'e-/ADU, 85%'),
  ('offset_adu',  'OFFSET',    10, '97%'),
  ('offset_adu',  'BLKLEVEL',  20, 'blacklevel variant'),

-- Geometry. TheSky writes CCDXBIN/CCDYBIN instead of XBINNING/YBINNING.
  ('binx',        'XBINNING',  10, '100%'),
  ('binx',        'CCDXBIN',   20, 'TheSky, 18%'),
  ('biny',        'YBINNING',  10, '100%'),
  ('biny',        'CCDYBIN',   20, 'TheSky, 18%'),
  ('pixel_size',  'XPIXSZ',    10, '100%'),
  ('naxis1',      'NAXIS1',    10, '100%'),
  ('naxis2',      'NAXIS2',    10, '100%'),
  ('bayer',       'BAYERPAT',  10, '100%, RGGB throughout'),
  ('row_order',   'ROWORDER',  10, '67%, TOP-DOWN throughout'),

-- Temperature. Both are kept; they are not interchangeable.
  ('set_temp_c',  'SET-TEMP',  10, 'setpoint, drives calibration matching'),
  ('ccd_temp_c',  'CCD-TEMP',  10, 'actual, drives quality triage'),

-- Optics. FOCALLEN is universal and is the reliable half of rig resolution.
  ('focal_len_mm','FOCALLEN',  10, '100%'),
  ('focal_ratio', 'FOCRATIO',  10, NULL),
  ('aperture_mm', 'APTDIA',    10, 'TheSky, 14%'),

-- Instrument. TELESCOP is NOT reliably the optics: observed as 'Askar-185'
-- (optics) from NINA but 'EQMod Mount', 'ZWO AM5', 'ZWO AM7' (the mount) from
-- other setups. Recorded as telescope_raw, never used to resolve a rig.
  ('instrument',  'INSTRUME',  10, '100%'),
  ('instrument',  'CAMERA',    20, NULL),
  ('telescope',   'TELESCOP',  10, 'mount or optics depending on app; untrusted'),
  ('readout_mode','READOUTM',  10, 'TheSky, 13%'),

-- Target
  ('object',      'OBJECT',    10, '70%'),
  ('ra',          'RA',        10, 'decimal degrees, 71%'),
  ('ra',          'OBJCTRA',   20, 'sexagesimal fallback'),
  ('dec',         'DEC',       10, 'decimal degrees, 71%'),
  ('dec',         'OBJCTDEC',  20, 'sexagesimal fallback'),
  ('equinox',     'EQUINOX',   10, NULL),

-- Site. OBSGEO-* first: TheSky writes SITELONG sexagesimal with a POSITIVE
-- sign for a west longitude, which silently mirrors the observatory to the far
-- side of the planet. OBSGEO-L is a correct signed decimal.
  ('latitude',    'OBSGEO-B',  10, 'correct signed decimal'),
  ('latitude',    'SITELAT',   20, NULL),
  ('latitude',    'LAT-OBS',   30, NULL),
  ('longitude',   'OBSGEO-L',  10, 'OBSGEO first: SITELONG sign is unreliable'),
  ('longitude',   'SITELONG',  20, 'TheSky writes west as positive'),
  ('longitude',   'LONG-OBS',  30, NULL),
  ('elevation_m', 'OBSGEO-H',  10, NULL),
  ('elevation_m', 'SITEELEV',  20, NULL),
  ('elevation_m', 'ALT-OBS',   30, NULL),
  ('observatory', 'OBSERVAT',  10, '65% present'),
  ('observatory', 'SITENAME',  20, 'often present but empty'),
  ('observer',    'OBSERVER',  10, '81%'),

-- Pointing and conditions
  ('airmass',     'AIRMASS',   10, '66%'),
  ('altitude',    'CENTALT',   10, '66%'),
  ('azimuth',     'CENTAZ',    10, '66%'),
  ('pier_side',   'PIERSIDE',  10, '63 of 103; flips are routine here'),
  ('moon_sep',    'MOONANGL',  10, NULL),
  ('sun_angle',   'SUNANGLE',  10, NULL),

-- Focus and rotation
  ('focus_pos',   'FOCPOS',    10, '78%'),
  ('focus_pos',   'FOCUSPOS',  20, 'NINA duplicate'),
  ('focus_temp',  'FOCTEMP',   10, '78%'),
  ('focus_temp',  'FOCUSTEM',  20, 'NINA duplicate'),
  ('focus_temp',  'FOCTMPSC',  30, 'TheSky'),
  ('rotator_deg', 'OBJCTROT',  10, NULL),
  ('rotator_deg', 'ROTATOR',   20, 'TheSky, 15%'),

-- Filter and provenance
  ('filter',      'FILTER',    10, '90%'),
  ('software',    'SWCREATE',  10, 'NINA / TheSky, 78%'),
  ('software',    'PROGRAM',   20, 'PixInsight-written masters'),
  ('software',    'CREATOR',   30, NULL),

-- Frame type
  ('image_type',  'IMAGETYP',  10, '100%'),
  ('image_type',  'FRAME',     20, NULL),
  ('image_type',  'PICTTYPE',  30, 'TheSky, 13%'),

-- Sky brightness, stamped by nightwatcher-ingest. Absent from the existing
-- archive (0 of 4,000 sampled), so expect this to populate going forward only.
  ('sqm',         'SQM',       10, 'nightwatcher-ingest stamp'),
  ('sqm_sensor',  'SQMSRC',    10, NULL),
  ('sqm_time',    'SQMTIME',   10, NULL),
  ('sqm_dt_s',    'SQMDT',     10, NULL);

-- ---------------------------------------------------------------------------
-- Value normalization
-- ---------------------------------------------------------------------------

-- IMAGETYP: twelve distinct spellings observed across four applications.
INSERT IGNORE INTO header_value_map (field, raw_value, normalized, match_mode, priority) VALUES
  ('image_type', 'LIGHT',        'light',    'exact', 10),
  ('image_type', 'Light',        'light',    'exact', 10),
  ('image_type', 'Light Frame',  'light',    'exact', 10),
  ('image_type', 'DARK',         'dark',     'exact', 10),
  ('image_type', 'Dark',         'dark',     'exact', 10),
  ('image_type', 'Dark Frame',   'dark',     'exact', 10),
  ('image_type', 'FLAT',         'flat',     'exact', 10),
  ('image_type', 'Flat',         'flat',     'exact', 10),
  ('image_type', 'Flat Field',   'flat',     'exact', 10),
  ('image_type', 'Bias',         'bias',     'exact', 10),
  ('image_type', 'Bias Frame',   'bias',     'exact', 10),
  ('image_type', 'BIAS',         'bias',     'exact', 10),
  -- Masters must be tested before the plain type words, hence the lower number.
  ('image_type', 'Master Dark',  'master',   'exact',  5),
  ('image_type', 'master',       'master',   'contains', 6),
  ('image_type', 'integration',  'master',   'contains', 6),
  ('image_type', 'stack',        'master',   'contains', 6),
  -- Dark-flats: must beat both 'dark' and 'flat'.
  ('image_type', 'flatdark',     'darkflat', 'contains', 4),
  ('image_type', 'dark flat',    'darkflat', 'contains', 4),
  ('image_type', 'darkflat',     'darkflat', 'contains', 4),
  ('image_type', 'zero',         'bias',     'contains', 20);

-- FILTER: several values are placeholders meaning "no filter in the path", not
-- a filter name. TheSky writes '!Shutter!' for darks and bias; leaving that as
-- a filter would fragment every calibration group.
INSERT IGNORE INTO header_value_map (field, raw_value, normalized, match_mode, priority) VALUES
  ('filter', '!Shutter!', '', 'exact', 10),
  ('filter', 'DARK',      '', 'exact', 10),
  ('filter', 'None',      '', 'exact', 10);

-- Note deliberately NOT expressed here: the empty-filter default. An absent or
-- blanked FILTER becomes 'CLEAR' for LIGHT and FLAT frames only, and is left
-- NULL for darks and bias, which have no filter in the light path at all.
-- That rule depends on image_type, so it belongs to the extractor rather than
-- to a flat raw-value mapping; a blanket default here would invent a CLEAR
-- filter on every dark and bias and fragment the calibration groups it is
-- supposed to unify. nightwatcher-ingest draws the same distinction
-- (resolve.filter.write_header applies to lights and flats).

-- OBJECT: NINA's Flat Wizard stamps its own name as the target. Frames carrying
-- it are flats, never a target called 'FlatWizard'.
INSERT IGNORE INTO header_value_map (field, raw_value, normalized, match_mode, priority) VALUES
  ('object', 'FlatWizard', '', 'exact', 10);

INSERT IGNORE INTO header_value_map (field, raw_value, normalized, match_mode, priority) VALUES
  ('pier_side', 'East', 'east', 'exact', 10),
  ('pier_side', 'West', 'west', 'exact', 10),
  ('row_order', 'TOP-DOWN',   'top-down',   'exact', 10),
  ('row_order', 'BOTTOM-UP',  'bottom-up',  'exact', 10);

-- ---------------------------------------------------------------------------
-- Default calibration rules
-- ---------------------------------------------------------------------------

INSERT IGNORE INTO calibration_rules
    (name, target_type, priority, match_json, max_age_days, prefer_masters, prefer_same_session, min_frames, notes)
VALUES
  ('default-dark', 'dark', 10,
   '{"camera_id":"exact","gain":{"exact":true,"null_ok":true},"offset_adu":{"exact":true,"null_ok":true},"binx":"exact","biny":"exact","readout_mode":"exact","set_temp_c":{"tol":1.0},"exposure_s":{"tol":5.0}}',
   365, 1, 0, 10,
   'Tolerances resolve to the distinct observed values and match with IN(), not BETWEEN; see docs/ARCHITECTURE.md section 7. gain/offset are null_ok because PixInsight-written masters drop GAIN and OFFSET entirely: requiring them would make every master dark unmatchable.'),

  ('default-bias', 'bias', 10,
   '{"camera_id":"exact","gain":"exact","offset_adu":"exact","binx":"exact","biny":"exact","readout_mode":"exact"}',
   365, 1, 0, 20, NULL),

  ('default-flat', 'flat', 10,
   '{"rig_id":"exact","filter_id":"exact","binx":"exact","biny":"exact","rotator_deg":{"tol":5.0}}',
   30, 1, 1, 10,
   'Keyed on the rig, not the camera: a flat belongs to an optical train. Same session strongly preferred; dust moves.'),

  ('default-darkflat', 'darkflat', 10,
   '{"camera_id":"exact","gain":"exact","offset_adu":"exact","binx":"exact","biny":"exact","exposure_s":{"tol":0.5}}',
   30, 1, 1, 10, NULL);
