-- ---------------------------------------------------------------------------
-- Author:        David Gilinsky
-- File:          sql/equipment.example.sql
-- Purpose:       Example equipment registry: the site, cameras, telescopes,
--                rigs, and filters for one observatory. Load after schema.sql
--                and seed.sql, then edit for your own gear. This is site-
--                specific and is NOT part of the generic seed.
-- Created:       2026-07-24
-- Last Modified: 2026-07-24
-- Version:       0.1.0
-- License:       GPL-3.0-or-later
-- ---------------------------------------------------------------------------
-- Cameras and filters auto-register from scans, so this file is optional. It is
-- what turns raw values into a rig: a rig cannot be derived from the header
-- (TELESCOP is the mount as often as the optics), so the camera-plus-focal-
-- length definitions below are the only way rig_id gets populated.
--
-- Idempotent (INSERT IGNORE), so re-loading it after an upgrade is safe.

-- One observatory site. utc_offset_h drives the noon-to-noon observing night;
-- is_default marks the site used when a frame's rig does not pin one.
INSERT IGNORE INTO sites (name, latitude, longitude, elevation_m, timezone,
                          utc_offset_h, sqm_sensor, is_default)
VALUES ('ExampleSite', 0.00000, 0.00000, 0, 'UTC', 0,
        'SQM01', 1);

-- Cameras. Colour vs mono is detected per-frame from BAYERPAT; the model is the
-- calibration key.
INSERT IGNORE INTO cameras (model, vendor, is_color, pixel_size_um) VALUES
  ('ASI6200', 'ZWO', 1, 3.76),
  ('ASI2600', 'ZWO', 1, 3.76),
  ('ASI4400', 'ZWO', 1, 3.76);

-- TheSky writes a generic INSTRUME of 'ASICamera'; alias it to the real body.
INSERT IGNORE INTO camera_aliases (pattern, camera_id, priority)
SELECT 'ASICamera', id, 10 FROM cameras WHERE model = 'ASI6200';

-- Optics.
INSERT IGNORE INTO telescopes (name, aperture_mm, focal_len_mm) VALUES
  ('Askar185', 185, 1295),
  ('WO73A',     73,  441),
  ('WOUC108',  108,  518);

-- Rigs: a camera on an optical train, matched by camera + focal length range.
INSERT IGNORE INTO rigs (name, camera_id, telescope_id, site_id,
                         focal_min_mm, focal_max_mm)
SELECT 'Askar185-ASI6200', c.id, t.id, s.id, 1280, 1310
  FROM cameras c, telescopes t, sites s
 WHERE c.model='ASI6200' AND t.name='Askar185' AND s.name='ExampleSite';
INSERT IGNORE INTO rigs (name, camera_id, telescope_id, site_id,
                         focal_min_mm, focal_max_mm)
SELECT 'WO73A-ASI2600', c.id, t.id, s.id, 435, 448
  FROM cameras c, telescopes t, sites s
 WHERE c.model='ASI2600' AND t.name='WO73A' AND s.name='ExampleSite';
INSERT IGNORE INTO rigs (name, camera_id, telescope_id, site_id,
                         focal_min_mm, focal_max_mm)
SELECT 'WOUC108-ASI2600', c.id, t.id, s.id, 512, 525
  FROM cameras c, telescopes t, sites s
 WHERE c.model='ASI2600' AND t.name='WOUC108' AND s.name='ExampleSite';

-- Filters, with the alias spellings capture apps write.
INSERT IGNORE INTO filters (name, band) VALUES
  ('Ha', 'narrowband'), ('OIII', 'narrowband'), ('SII', 'narrowband'),
  ('CLEAR', 'clear');
INSERT IGNORE INTO filter_aliases (pattern, filter_id, priority)
SELECT p, f.id, 10 FROM filters f JOIN (
  SELECT 'Ha' AS name, 'H-alpha' AS p UNION ALL SELECT 'Ha','Halpha'
  UNION ALL SELECT 'OIII','O3' UNION ALL SELECT 'SII','S2'
  UNION ALL SELECT 'CLEAR','L' UNION ALL SELECT 'CLEAR','None'
) a ON a.name = f.name;
