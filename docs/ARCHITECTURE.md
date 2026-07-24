<!--
  Author:        David Gilinsky
  File:          docs/ARCHITECTURE.md
  Purpose:       StarBase system architecture: components, data model, scanning
                 strategy, query and action engines, and the PixInsight bridge.
  Created:       2026-07-23
  Last Modified: 2026-07-23
  Version:       0.1.0
  License:       GPL-3.0-or-later
-->

# StarBase Architecture

## 1. What StarBase is

A queryable interface to astronomical image files, for presentation to other
applications (principally PixInsight WBPP) and for batch operations against
query results (tag, rename, move, copy, symlink, run a tool).

`starbased` runs as a Linux service. It monitors one or more directory trees of
FITS (and XISF) data, maintains a MariaDB index of every discovered frame and
its characteristics, and serves a web UI whose central act is: **build a query,
get a set of frames, do something with that set.**

Non-goals for v1: pixel processing, image analysis (FWHM, star counts, SNR),
thumbnail generation, and anything that opens image data rather than headers.
Hooks are left for these; see §12.

## 2. Design principles

1. **The rescan is authoritative; inotify is only an accelerator.** Storage may
   be local, NFS, or SMB, and inotify is unreliable on the latter two. A scan
   that only ever runs on the schedule must still produce a correct index.
2. **The header is the truth, and the whole header is kept.** Promoted columns
   are a fast path; every keyword is also stored verbatim so anything not
   anticipated is still queryable and backfillable. Same reasoning as
   NightWatcher2's `raw_line` columns.
3. **Header dialects are configuration, not code.** NINA, TheSkyX, and the
   ASIAIR write three different vocabularies. The keyword mapping lives in a
   data file the user can extend without rebuilding, which is the lesson
   `nightwatcher-ingest` already paid for.
4. **Never mutate the filesystem without a dry run.** Every destructive action
   previews first, is audited as a job row, and deletes to a trash directory.
5. **No coupling to NightWatcher.** Its own database (`starbase`), its own
   users, its own config tree. It reuses NightWatcher2's *patterns* and can
   share the MariaDB *instance*; nothing more.

## 3. Components

```
                       ┌──────────────────────────────────────────┐
   image trees  ─────► │  Scanner                                 │
   (local/NFS/SMB)     │   sweep threads + inotify + settle gate   │
                       └───────────────┬──────────────────────────┘
                                       │ FileRecord
                       ┌───────────────▼──────────────────────────┐
                       │  Extractors: FITS (cfitsio), XISF, sidecar│
                       │  + HeaderMap (configurable dialects)      │
                       └───────────────┬──────────────────────────┘
                                       │
   ┌───────────────┐   ┌───────────────▼──────────────────────────┐
   │ starbasectl   ├──►│  Repository (libmariadb)  ◄──►  MariaDB   │
   │   (CLI)       │   └───────────────┬──────────────────────────┘
   └───────────────┘                   │
                       ┌───────────────▼──────────────────────────┐
                       │  Query engine  │  Calibration matcher     │
                       └───────────────┬──────────────────────────┘
                       ┌───────────────▼──────────────────────────┐
                       │  Action engine: stage / fsop / exec / wbpp│
                       └───────────────┬──────────────────────────┘
                       ┌───────────────▼──────────────────────────┐
                       │  HTTP API + static SPA (cpp-httplib, TLS) │
                       └──────────────────────────────────────────┘
```

| Target | Kind | Purpose |
| --- | --- | --- |
| `starbased` | daemon | scanner, query, actions, HTTP API, web UI |
| `starbasectl` | CLI | db init, root CRUD, scan now, query, export, user admin |
| `sb_core` | static lib | config, logging, paths, time |
| `sb_fits` | static lib | FITS/XISF header extraction + header mapping |
| `sb_scan` | static lib | walker, watcher, change detection, settle gate |
| `sb_db` | static lib | MariaDB Connector/C repository layer |
| `sb_query` | static lib | filter AST → parameterized SQL, calibration matcher |
| `sb_actions` | static lib | job engine: staging, fsops, tool exec |
| `sb_pix` | static lib | PixInsight/WBPP bridge |
| `sb_auth` | static lib | PBKDF2 passwords, sessions (NightWatcher2 pattern) |
| `sb_api` | static lib | HTTP/JSON API, TLS, static file serving |

Layout mirrors NightWatcher2: `src/<subsystem>/`, `include/starbase/`,
`web/`, `sql/`, `config/`, `debian/`, `tests/`, `third_party/`.

## 4. Scanner

Two tiers, because the storage answer is "mixed".

**Sweep (authoritative).** Per root, one producer thread walks with
`std::filesystem::recursive_directory_iterator` and feeds a bounded queue; N
extractor threads consume. Runs on a per-root interval and on demand. Bounded
queue is what keeps memory flat on a tree of a million frames.

*Why threaded, and how many threads (measured, not guessed).* A header read is
network-latency-bound: it is a handful of NFS round-trips (open, read the header
blocks, close), not computation. That is exactly the workload concurrency helps,
because idle threads waiting on a round-trip overlap with others making progress.
Measured against the live archive with `tools/scan_bench` (nconnect=8 in effect):

| regime | 1 thread | 8 | 16 | 32 |
| --- | --- | --- | --- | --- |
| truly cold (milan disk) | ~90 f/s | — | ~640 f/s* | — |
| pixy-cold / milan-ARC-warm | 448 f/s | 8,580 (19x) | 13,470 (30x) | 16,800 (37x) |
| fully cached (CPU-bound) | 6,900 f/s | 26,900 (3.9x) | plateau | plateau |

(*via a process-per-file harness, so process startup caps the absolute rate; the
scaling is the point.) The lesson: in the cached/CPU-bound regime concurrency
plateaus around 4x, but the sweep's real regime is latency-bound, where it scales
to ~19x at 8 threads and ~30x at 16, with clear diminishing returns past 16. A
single-threaded cold sweep of ~70k frames is ~13 min; 8-16 workers cut that to
roughly 1-2 min. So `scanner_threads = 0` (auto) resolves to
`min(hardware_concurrency, 16)`: past ~16 the gain is small and it only adds RPC
pressure on milan's nfsd. This is why the walk is a producer/consumer pool from
the start, with the degenerate `threads=1` case reserved for debugging, not a
throwaway single-threaded implementation.

**Watch (accelerator).** Optional per-root inotify: `IN_CLOSE_WRITE`,
`IN_MOVED_TO`, `IN_MOVED_FROM`, `IN_DELETE`, and `IN_CREATE|IN_ISDIR` to add
watches for new subdirectories. Configured per root as
`watch = auto|inotify|off`; `auto` enables it only when the root's filesystem
type is local. Watch events never bypass the sweep's correctness; they only
make discovery prompt.

**Change detection.** The cheap identity is `(dev, inode, size, mtime_ns)`. If
that tuple is unchanged, the file is not reparsed; this is what makes a full
nightly sweep of a large archive nearly free. A content id (xxHash of the
header block plus the first data block) is computed on parse and stored.

**Move and rename survival.** A file whose content id matches an existing row
at a different path is an `UPDATE path`, not an `INSERT`. This matters directly:
`nightwatcher-ingest` refiles frames, and StarBase must not turn that into
duplicate rows.

**Settle gate.** A file still being written must not be indexed. `IN_CLOSE_WRITE`
covers the local case; sweeps require `mtime` older than `settle_seconds`
(default 30) or a stable size across two observations.

**Failure is a row, not a silence.** Unparseable or truncated files get
`status='error'` with the message retained, and are visible in the UI. Files
that vanish become `status='missing'` and are retained (with `last_seen`) rather
than deleted, so history and collections do not silently lose members.

### The archive has more than one writer, over more than one protocol

The reference archive is exported **both** as NFS (read by this host) and as SMB,
which is how N.I.N.A. on the observatory's Windows machine writes frames into
`incoming/nina`. `incoming/asiair` and `incoming/thesky` receive from the other
two capture applications, and GoodSync stages through `_gsdata_`. So at any
moment a frame may be mid-flight from a machine StarBase cannot see, over a
protocol it is not speaking.

Three consequences, all load-bearing:

1. **The settle gate is not a nicety.** An SMB client holding an opportunistic
   lock caches writes locally, and the server cannot tell an NFS reader that a
   file is still in flight. `settle_seconds` is the only thing between the
   scanner and a half-written 124 MB frame. It should be raised, not lowered, on
   a root that receives over SMB.
2. **This is why the dataset is `casesensitivity=insensitive`.** It is a
   deliberate choice for Windows clients, not an accident, so `case_sensitive`
   handling is permanent rather than a workaround for one odd dataset.
3. **Filesystem actions must be conservative.** StarBase cannot see SMB locks.
   A move or rename of a frame N.I.N.A. still has open would be invisible to the
   process that owns it. Roots are `writable = false` by default, destructive
   actions dry-run first, and the action engine only touches files that have
   been stable well beyond `settle_seconds`.

Default ignore globs therefore cover the debris the other writers leave:
`Thumbs.db`, `desktop.ini`, `System Volume Information`, `$RECYCLE.BIN`,
`.DS_Store`, `._*`, `_gsdata_`, and partial-write suffixes. ZFS `snapdir` is
`hidden`, so `.zfs` does not appear in `readdir` and snapshot trees are not
walked; `.zfs` is in the ignore list anyway, because that is a configuration
setting rather than a guarantee.

## 5. Metadata extraction

**FITS** via **CFITSIO** (`libcfitsio-dev`, already present at 4.3.1). NASA's
license is permissive and GPL-compatible. Open with `fits_open_diskfile`, pull
the header with `fits_hdr2str`, never touch pixels. This buys `.fits/.fit/.fts`
plus gzip (`.gz`) and Rice tile-compressed (`.fz`) transparently, which a
hand-rolled 2880-byte block reader would not. Multi-HDU files yield one `frames`
row per image HDU.

**XISF** by parsing the open XISF spec directly: 16-byte signature
(`XISF0100` + header length), then an XML header block. A small vendored pull
parser (pugixml or tinyxml2) handles it. No PixInsight code is involved.

**Sidecars** (`.txt`/`.json`/`.csv` logs written alongside frames, NINA/TSX
artifacts) are recorded in an `artifacts` table linked to the frame or the
containing session directory, not parsed for metadata in v1.

**Header mapping is a data file** (`config/headermap.conf`), listing per logical
field an ordered list of candidate keywords plus value normalizations:

```
[image_type]
keywords = IMAGETYP, FRAME, OBSTYPE
map      = "Light Frame"=light, "LIGHT"=light, "Dark Frame"=dark, "Bias Frame"=bias, \
           "Flat Frame"=flat, "FlatDark"=darkflat, "MasterDark"=dark
[filter]
keywords = FILTER, FILT-1, INSFLNAM
[gain]
keywords = GAIN, EGAIN
```

Ships with a default covering NINA, TheSkyX, and ASIAIR. Users extend it and
reload without a rebuild.

## 6. Data model (MariaDB)

Own database `starbase`, own user, on the same instance NightWatcher2 uses.
`sql/setup.sql` creates database and user; `sql/schema.sql` creates tables;
`sql/seed.sql` loads the default header mapping; `sql/migrations/` carries
versioned changes. All timestamps UTC.

**Target instance, confirmed.** MariaDB 10.11.14 on `127.0.0.1:3306`, currently
holding only `nightwatcher`. StarBase copies NightWatcher2's credential pattern
exactly: connection details in `/etc/starbase/starbase.conf`, password *only* in
a root-owned `0600` `/etc/starbase/starbase.env` as `SB_DB_PASSWORD`, pulled in
by the systemd unit's `EnvironmentFile=`. Never in the config file, never in git.

One divergence from NightWatcher2's unit is required. It runs `DynamicUser=yes`,
which is the right default, but StarBase must read an archive that is
`root:astro` and mode `0770`. The unit therefore needs
`SupplementaryGroups=astro`, which `DynamicUser` does support. Without it the
daemon sees an empty tree and reports a successful scan of nothing, which is the
worst possible failure mode.

**SQM backfill is not viable for historical frames.** NightWatcher2's `readings`
table holds 1,424 rows from a single sensor (`DSN036`) covering
2026-07-19 onward, while the image archive goes back years. Backfilling
`sqm_mag_arcsec2` from the database can only ever cover frames taken after SQM
logging started; for everything older the column stays NULL. Offer the backfill,
but scope it honestly in the UI rather than implying full coverage.

**`sql/schema.sql` is authoritative** and `sql/seed.sql` carries the default
header mapping; both keep their rationale inline. What follows is the shape and
the decisions behind it.

### Verified against the live archive

Every mapping in `seed.sql` was checked against real frames rather than the FITS
standard in the abstract: 103 files sampled for keyword vocabulary, 4,000 for
coverage and collisions, across N.I.N.A. 3.2, TheSky 10.5, two ASIAIR
generations, and PixInsight-written masters. What that changed:

- **`IMAGETYP` has twelve spellings** in one archive (`LIGHT`, `Light`,
  `Light Frame`, `DARK`, `Dark`, `Dark Frame`, `FLAT`, `Flat`, `Flat Field`,
  `Bias`, `Bias Frame`, `Master Dark`). Value normalization is not a nicety.
- **`TELESCOP` is not consistently the mount.** N.I.N.A. writes `Askar-185`
  (the optics); other rigs write `EQMod Mount`, `ZWO AM5`, `ZWO AM7`. It is
  unreliable in *both* directions, which is a stronger argument for
  camera-plus-focal-length rig resolution than "it is always the mount".
- **TheSky writes `FILTER = !Shutter!`** on darks and bias. Left alone, that
  fragments every calibration group. Normalized to empty.
- **N.I.N.A.'s Flat Wizard writes `OBJECT = FlatWizard`**, which would otherwise
  appear as a deep-sky target with 9 frames.
- **`PIERSIDE` splits 33 East / 30 West**, so meridian flips are routine and
  pier side is now a column: the field is rotated 180 degrees across a flip.
- **`ROWORDER` is `TOP-DOWN` throughout**, and feeds WBPP's
  `fitsCoordinateConvention`. Getting it wrong flips every image vertically.
- **PixInsight masters drop `GAIN` and `OFFSET` entirely** and write binning as
  a float (`1.0`). The dark rule marks those fields `null_ok`; requiring them
  would make every master dark unmatchable, which is precisely backwards given
  WBPP prefers masters.
- **Two undocumented cameras** are in use (`ZWO ASI4400MC Pro`,
  `ZWO ASI2600MC Air`) and two ASIAIR software strings (`ZWO ASIAIR Plus`,
  `ZWO 2600AIR`), none of which appear in the `nightwatcher-ingest` rig config.
- **No frame carries an `SQM` card** (0 of 4,000). The stamp is configured but
  has not landed in the existing archive, so `sqm_mag_arcsec2` will populate
  going forward only, and backfilling from NightWatcher2's `readings` table is
  worth offering.
- Headers run 41-81 cards (median 68), so `frame_keywords` is ~5M rows at
  archive scale. `HISTORY` and `COMMENT` repeat many times per file, which is
  why the primary key is `(frame_id, ord)` and not `(frame_id, keyword)`.

Sizing correction: the 8.4 TB volume is mostly `work/` (7.7 TB of PixInsight
working files). The science archive is `lights/` 386 GB plus `calibration/`
242 GB, roughly 630 GB. Which trees become roots is a configuration choice, and
`work/` probably should not be one.

| Group | Tables | Purpose |
| --- | --- | --- |
| Storage | `roots`, `files`, `artifacts` | monitored trees, one row per file, sidecars |
| Frames | `frames`, `frame_keywords` | one row per image HDU, plus every header card verbatim |
| Equipment | `sites`, `cameras`, `camera_aliases`, `telescopes`, `rigs`, `filters`, `filter_aliases` | the normalization registry |
| Mapping | `header_map`, `header_value_map` | which keyword feeds which field, and how values normalize |
| Curation | `tags`, `frame_tags`, `collections`, `collection_frames`, `saved_queries` | user-owned structure |
| Matching | `calibration_rules` | declarative dark/flat/bias rules |
| Actions | `jobs`, `job_items`, `tools` | audited job engine, allow-listed external tools |
| Ops | `scan_log`, `events`, `schema_version`, `users`, `sessions` | history, audit, migrations, auth |

Views: `v_frames` (the browse grid, equipment names resolved and absolute path
assembled) and `v_frame_summary` (counts and integration time by night, type,
target, rig, filter).

### Decisions worth defending

**No `st_dev` anywhere.** The obvious identity tuple is
`(dev, inode, size, mtime)`, and it is wrong here. `st_dev` is a *client-side*
mount identity: it changes across a remount or reboot. On an NFS archive that
would make every file look new after a reboot and trigger a full reindex.
Identity is `(root_id, rel_path)`; `st_ino` is kept only as a change hint.

**Path uniqueness is by hash.** A `UNIQUE` key on a 1 KB `VARCHAR` path exceeds
InnoDB's index limit, and a prefix index is not actually unique. `files` carries
`rel_path_hash BINARY(16)` with `UNIQUE (root_id, rel_path_hash)`, and the
readable path alongside it.

**The fingerprint is over immutable header cards, not file contents.**
`DATE-OBS | INSTRUME | EXPTIME | NAXIS1 | NAXIS2 | IMAGETYP | XBINNING`. A
whole-file hash is wrong twice over: `nightwatcher-ingest` stamps `SQM`,
`SQMSRC`, `SQMTIME`, `SQMDT` and a defaulted `FILTER` into frames *after* they
are filed, so the content hash of an unchanged exposure changes; and hashing
8.4 TB across NFS on every sweep is not viable. The fingerprint costs nothing
because the header is already parsed. It is indexed but **not** `UNIQUE`: move
detection requires exactly one match, and ambiguity is reported rather than
guessed at.

The fingerprint was then tested on 4,000 real frames. It produced 96 collisions
covering 192 files, and **every one was a genuine duplicate**: the `NGC7000/`
and `NGC_7000/` trees hold the same 96 exposures filed twice under two different
object-name sanitizations, 4.5 GB of redundant data.

That is also a direct vindication of choosing header cards over file contents.
The duplicate pairs have **identical headers and bit-identical pixel data but
differ as byte streams** (card ordering and block padding), so a whole-file hash
would have missed all 96. The header fingerprint caught them, and StarBase would
surface them on the first scan.

**Both temperatures, kept apart.** `set_temp_c` (setpoint) drives calibration
matching; `ccd_temp_c` (actual) drives quality triage. `nightwatcher-ingest`
collapses them into one template variable, which is right for naming and wrong
for matching. A large gap between the two is itself worth querying for.

**Raw values are kept next to resolved ones.** `filter_raw` beside `filter_id`,
`instrume_raw` beside `camera_id`, `telescope_raw` recorded but explicitly not
trusted (it is usually the mount). A bad alias is then diagnosable, and fixable,
without a rescan. `filter_defaulted` records that `CLEAR` was injected rather
than observed.

**`session_night` is stored, not computed.** The local noon-to-noon night is the
single most common grouping anyone asks of this data, and a computed expression
cannot be indexed usefully.

**`image_type` includes `master`.** Masters and integrations live under
`process/`, and WBPP prefers them over raw stacks, so the calibration matcher is
much stronger for indexing them. `review_reason` and `quarantine_reason` carry
the taxonomy `nightwatcher-ingest` already established (`focus`, `slew`,
`preview`, `short`, `no-dateobs`, `no-object`, ...), which makes StarBase the
place you go to *fix* a misrouted frame rather than just notice one.

**SQM is a promoted column.** "Lights of M31 taken under skies darker than 21.0"
is a query only this ecosystem can answer, since `nightwatcher-ingest` already
stamps the reading from NightWatcher2 into the header.

Sizing: at 70k frames the whole index is ~50 MB (10.5 MB data, 40 MB indexes on
`frames`). The index-to-data ratio is high by design, since this is a
read-mostly index whose entire purpose is querying; it is worth revisiting only
if a given index proves unused.

## 7. Query engine

The UI posts a **filter AST as JSON**; the engine compiles it to parameterized
SQL. User input never reaches a concatenated string. Supported predicates:

- comparisons and ranges on promoted columns
- set membership (`filter IN ('Ha','OIII')`)
- night-of and UTC date ranges
- arbitrary header predicates: `keyword['FOCUSTEM'] < -5`, compiled to an
  `EXISTS` against `frame_keywords`
- cone search: bounding-box prefilter on indexed ra/dec, then exact haversine
- tag, collection, and root membership
- file status (`ok`/`error`/`missing`)

Results paginate server-side; the grid is virtualized. Saved queries get stable
ids so they can be referenced by URL, by `starbasectl`, and by the PixInsight
pull endpoint (§9.4).

### Compile tolerances to equality, not ranges

Measured on a 70k-frame load of the schema (see §6). A calibration match
expressed the obvious way, with `set_temp_c BETWEEN -11 AND -9` and
`exposure_s BETWEEN 295 AND 305`, degrades to a range scan: once a composite
index hits a range column, nothing after it can narrow or order, so the query
examined 19,186 rows and fell back to a filesort.

But set-point temperature, exposure, gain, offset, and binning are **discrete in
practice** rather than continuous. A camera is run at a handful of setpoints and
a handful of exposures; the seeded archive has one distinct `set_temp_c` and
four distinct `exposure_s`. So the matcher resolves each tolerance against the
small set of distinct observed values first, then matches with `IN (...)`:

```sql
-- instead of  set_temp_c BETWEEN -11 AND -9
SELECT DISTINCT set_temp_c FROM frames
 WHERE image_type='dark' AND camera_id=? AND ABS(set_temp_c - ?) <= ?;
-- then         set_temp_c IN (-10)
```

Equality all the way down keeps the index usable through to `date_obs_utc`.
Same query, rewritten: **filesort eliminated, rows examined down 4x** (19,186 →
4,664, `Using index`). The dimension lookup is trivially cheap. This is a rule
for the query compiler, not an optimization to apply by hand later.

### Measured index behaviour

| Query | Index | Result |
| --- | --- | --- |
| lights by target + filter, newest first | `idx_frames_light` | `ref`, covering |
| dark match (IN-list form) | `idx_frames_calib_dark` | `ref`, covering |
| flat match by rig | `idx_frames_calib_flat` | `ref`, covering |
| cone search bounding box | `idx_frames_radec` | `range`, covering |
| header predicate via `frame_keywords` | `idx_frame_kw` | materialized, range |
| fingerprint move detection | `idx_frames_fingerprint` | `ref`, 1 row |

One caveat found: `v_frame_summary` aggregates the whole table and took ~700 ms
at 70k frames, so it scales linearly and will be several seconds at 700k. The
dashboard must either push a date range into it or maintain a summary table
incrementally. Do not let the UI call it unbounded.

## 8. Calibration matcher

Selecting lights is easy. Finding the darks, flats, bias, and dark-flats that
*belong* to them is the actual value StarBase adds, and it must be inspectable
rather than magic.

Rules are declarative and live in config:

| Target | Must match | Tolerance | Ranking |
| --- | --- | --- | --- |
| dark | instrument, gain, offset, binning, readout mode | set-temp ±`temp_tol`, exposure ±`exp_tol` | nearest in time, within `max_age_days` |
| flat | instrument, filter, binning, optical train (telescope, focal length, rotator angle), gain/offset | rotator ±`rot_tol` | same session preferred, then nearest |
| bias | instrument, gain, offset, binning, readout mode | — | nearest in time |
| dark-flat | as bias, plus flat exposure ±`exp_tol` | | nearest in time |

Masters (`MasterDark`, `MasterFlat`, ...) rank above raw stacks when present.
The matcher returns a ranked candidate list with a score **and a human-readable
reason per candidate**, and the UI shows why each was chosen and lets the user
override. A match that cannot be explained is a match that cannot be trusted.

## 9. PixInsight / WBPP bridge

**Finding: WBPP already has a first-class command-line automation API.** It is
not documented on the website but it ships in the source, in
`/opt/PixInsight/src/scripts/BatchPreprocessing/BPP-Automation.js` (WBPP 3.0.1,
PixInsight Core 1.9.4). This changes the design substantially in our favour: in
the common case StarBase does not need to write PJSR at all.

```sh
/opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
  -r="/opt/PixInsight/src/scripts/BatchPreprocessing/WBPP.js,automationMode=true,\
dir=<staging>,outputDirectory=<out>,keywords=FILTER prepost,platesolve=true,..." \
  --force-exit
```

`file=<path>` (repeatable) and `dir=<path>` (recursive) add frames;
`loadOnly` loads the configuration and opens the dialog instead of executing.
Beyond that the surface is broad: calibration tolerances, overscan, integration
combination/rejection per image type, linear pattern subtraction, plate solving,
registration and star detection, subframe weighting, frame selection with custom
formulas, and local normalization. Press Alt+A in the WBPP GUI for the full list,
or read `printAutomationHelp()` in `BPP-Automation.js`.

**One sharp edge:** arguments are split on `,` and then on `=`. A path containing
either character cannot be passed. The staging tree solves this, because
StarBase controls the names it generates.

The core of the bridge is therefore **the staging tree**, and four delivery
surfaces sit on top of it:

1. **Staging tree** (the substrate). A job builds
   `staging_root/<job>/{light,dark,flat,bias,darkflat}/…` from symlinks
   (configurable: symlink | hardlink | copy), with sanitized names.
2. **Headless CLI automation.** Render WBPP parameters from a stored
   "preprocessing profile" and invoke as above. Fully unattended.
3. **`loadOnly` handoff.** Same invocation plus `loadOnly`; WBPP opens
   pre-populated for interactive tweaking. This is the "send to my desktop"
   button, and it is the one most likely to get daily use.
4. **IPC to a running instance.** `PixInsight -x=<slot>:<script.js>` sends a
   generated PJSR file to an already-open PixInsight; `PixInsight -e` enumerates
   live instances. Use when one is already running.
5. **REST pull.** `GET /api/v1/queries/{id}/paths` returns JSON; a small
   StarBase-authored PJSR helper inside PixInsight fetches a saved query live.
   Thinnest coupling, best for people who live inside PixInsight.
6. **Plain export.** CSV / JSON / newline-delimited path list, always available.

**Two constraints worth deciding early.**

*Licensing.* PJSR and WBPP are under the PixInsight Class Library License 2.0,
not GPL. StarBase must not vendor or link PixInsight code. Generating a `.js`
file and executing the `PixInsight` binary is arm's-length and fine; our own
PJSR helper is our code and ships GPL-3.0-or-later.

*Session.* `starbased` runs as an unprivileged system service with no desktop
session, so it cannot simply launch a GUI PixInsight (`DISPLAY`, session bus,
and PixInsight's per-user settings all belong to the desktop user). Plan:

- **v1**: StarBase prepares the staging tree, renders the exact command, and
  offers it as copy-paste plus a downloadable launcher script. Zero session
  problems, immediate value.
- **v1.1**: an optional `starbase-agent` systemd `--user` unit running in the
  desktop session, which claims launch jobs from the daemon and runs them with
  the right environment.
- Whether `--automation-mode --force-exit` runs truly headless on Linux, or
  needs `xvfb-run`, is an **open item to test** before committing to a
  fully-unattended path.

The staging root must be readable by both the `starbase` service account and the
desktop user; a shared group on the staging root is the deployment answer.

## 10. Action engine

Jobs run on a worker pool; every job is an audited row with per-item results,
and is idempotent and resumable.

- `stage` — build the symlink/hardlink/copy tree (§9.1)
- `wbpp` — stage, then hand off by one of the surfaces above
- `fsop` — move / copy / rename / symlink / trash, driven by a **template
  renamer** sharing `nightwatcher-ingest`'s variable vocabulary
  (`{object}`, `{night}`, `{filter}`, `{exposure}`, `{gain}`, `{binning}`, …)
- `tag` — bulk tag/untag, collection membership
- `exec` — run an **allow-listed** external tool over the result set, passed as
  an argv array or a list file

Rules: dry-run always available and default for destructive types; deletes go to
a trash directory, never `unlink`; roots are `writable = false` by default;
`exec` is allowlist-only with no shell interpretation, configured in root-owned
`/etc/starbase/`. Successful fsops write back into `files` so the index stays
true immediately rather than waiting for the next sweep.

## 11. API, web UI, and security

REST under `/api/v1`, JSON in and out, served by vendored cpp-httplib with
OpenSSL, same as NightWatcher2. Static SPA in `web/` (vanilla JS, no build
step). Tabs: Dashboard, Browse/Query, Frame detail, Collections & Tags, Actions,
Roots & Settings, Users, Database.

Security posture, inherited from NightWatcher2 and tightened because this daemon
mutates filesystems:

- bind `127.0.0.1` by default; self-signed TLS generated on first start when
  enabled; when bound off-localhost, **reads require auth too**
- PBKDF2 password hashing, session cookies, seeded `admin`/`admin` with a
  forced change prompt; optional `SB_API_TOKEN` for scripts
- DB password from `SB_DB_PASSWORD`, never in a config file or in git
- runs as an unprivileged `starbase` user under systemd hardening
  (`ProtectSystem=strict`, `NoNewPrivileges`, explicit `ReadWritePaths` for the
  state and staging directories, `ReadOnlyPaths` for image roots)

## 12. Deliberately deferred

`frame_stats` table and a pluggable analyzer interface are designed in but not
implemented: FWHM, eccentricity, SNR, star counts, background median, thumbnails.
These require reading pixels, which changes the I/O profile of a scan entirely,
so they belong behind an explicit opt-in and a separate worker pool.

## 13. Build and packaging

CMake ≥ 3.16, C++17, warnings as an interface target, self-contained install
prefix `/usr/local/starbase`, options `SB_BUILD_TESTS` and `SB_WITH_DB`;
vendored `third_party/httplib` and `nlohmann`; CPack `.deb` with debconf
prompting for the database password, initial root paths, and bind/port; systemd
unit; `sql/` installed for `POST /api/v1/db/init`. GitHub Actions for build and
test. Straight transcription of the NightWatcher2 setup.

Dependencies, all verified present on this host: MariaDB 10.11 (running),
`libmariadb` 3.3.17, `cfitsio` 4.3.1, OpenSSL, CMake, pkg-config.
PixInsight Core 1.9.4 with WBPP 3.0.1 at `/opt/PixInsight`.

## 14. Milestones

| # | Deliverable |
| --- | --- |
| M0 | Skeleton: CMake, CI, GPL-3.0 license, file headers, `starbased` stub |
| M1 | Schema + `sb_db` + `starbasectl db-init` / `add-root` |
| M2 | FITS extractor + header mapping + single-threaded walk |
| M3 | Threaded sweep + change detection + move survival + inotify |
| M4 | HTTP API + auth + minimal web UI (browse, frame detail) |
| M5 | Filter AST query builder + saved queries |
| M6 | Calibration matcher with explanations |
| M7 | Action engine: staging tree, exports, fsops with dry-run |
| M8 | PixInsight bridge: CLI automation, `loadOnly`, REST pull, PJSR helper |
| M9 | XISF + sidecars |
| M10 | Packaging (`.deb`, debconf, systemd), docs, public repo |

M1–M4 is the smallest thing that is already useful: a searchable index with a
web UI. M7–M8 is where it starts replacing manual work.
