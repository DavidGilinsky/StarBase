# StarBase

**A queryable interface to your astronomical image archive.**

StarBase runs as a Linux service. It watches directory trees of FITS (and XISF)
data, keeps a MariaDB index of every frame and its characteristics, and serves a
web UI whose central act is simple: **build a query, get a set of frames, then do
something with that set.**

That "something" is usually one of:

- Hand the matched lights, plus the calibration frames that actually belong to
  them, to **PixInsight WBPP**: a staging tree and a rendered command line, or a
  live pull from inside PixInsight via the bundled PJSR helper.
- Tag frames, gather them into named collections, and query by membership.
- Stage, export (CSV / JSON / path list), or run filesystem operations
  (copy / symlink / move / trash) across a result set, always behind a dry run.
- Normalize messy target names to their catalog form and search across
  designations, so `M31`, `M 31`, `NGC224`, and `NGC 224` all find the same object.

StarBase reads headers, not pixels. It does not process, stack, or analyze image
data; it tells you precisely which files to hand to the tools that do.

---

## Why it exists

A working observatory accumulates tens or hundreds of thousands of sub-frames
across many nights, targets, filters, and equipment configurations. The hard part
of preprocessing is rarely the stacking; it is answering questions like *"give me
every Ha light of NGC 7000 shot on the WO73 rig at f/5.6, and the flats and darks
that match them,"* and doing it without hand-walking directory trees. StarBase
turns that archive into a database you can query, and turns a query result into an
action.

It is built for a real home observatory: mixed local / NFS / SMB storage, several
capture programs writing different header vocabularies, and PixInsight as the
downstream processor.

---

## Features

### Indexing and scanning
- Threaded directory sweep with a bounded work queue, so memory stays flat on a
  tree of a million frames.
- `inotify` is used only as an accelerator; the scheduled rescan is authoritative,
  because inotify is unreliable over NFS and SMB.
- A settle gate ignores files still being written.
- Asynchronous scans with **live progress** reported to the UI.
- FITS headers via CFITSIO; XISF parsed directly from the open specification.
- The **whole header is kept** verbatim, so anything not promoted to a column is
  still queryable and can be back-filled later without a rescan.
- **Header dialects are configuration**: NINA, TheSkyX, and the ASIAIR write three
  different vocabularies, and the keyword mapping lives in a data file you can
  extend without rebuilding.

### Query and organize
- A visual filter-AST builder with saved queries, plus a fast filter bar with
  discovered-value dropdowns for browsing.
- **Catalog-aware target search**: Messier <-> NGC/IC equivalence and
  space/case-insensitive matching, so either catalog number finds an object that
  has both.
- Offline target-name canonicalization (`M101` -> `M 101`), with optional CDS
  **Sesame** resolution of cross-designations against SIMBAD (off by default; the
  only feature that reaches the network).
- Cone search on RA/Dec, tags, and named collections queried by membership.
- Sky-brightness (**SQM**, mag/arcsec^2) is extracted and queryable when the header
  carries it.

### Equipment model
- Cameras and filters are auto-detected on scan.
- **Rigs** (a camera on an optical train) are matched to frames by camera and focal
  length; **sites** locate them.
- An **equipment builder** suggests rigs from the camera/focal-length combinations
  in your headers, and clusters frames into sites by observatory location within a
  distance you set.
- **Flat sets** bind a specific group of flats, or a master flat, to specific
  lights per rig. A mobile rig (no fixed site) matches flats by night; a fixed rig
  keeps an active set until you shoot a new one; explicit pins override both.

### Calibration matching
- Declarative, editable rules match darks, flats, bias, and dark-flats to a light.
- Every candidate carries a score **and a human-readable reason**, so a match you
  cannot explain is a match you do not have to trust.
- Masters are preferred over raw stacks when present, and the matcher is flat-set
  aware.

### Actions and the PixInsight bridge
- A staging tree (symlink, hardlink, or copy) is the substrate for handoff.
- **Every destructive action previews first**, is recorded as an audited job, and
  deletes to a trash directory rather than unlinking.
- Exports to CSV, JSON, or a newline-delimited path list.
- **PixInsight / WBPP**: render WBPP's command-line automation from a stored
  profile for fully unattended runs, a `loadOnly` handoff that opens WBPP
  pre-populated for interactive tweaking, or a live REST pull from a small PJSR
  helper inside PixInsight. No PixInsight code is vendored or linked.

### Operations and security
- Embedded HTTPS server with an auto-generated self-signed certificate.
- PBKDF2 password hashing and session cookies, with admin / user / readonly roles;
  writes require a login, reads are open on localhost.
- A systemd service, a self-contained `/usr/local/starbase` prefix, and a Debian
  package with debconf-driven setup.
- The daemon **self-bootstraps its schema**, so a fresh database needs no manual
  setup.

---

## Architecture

StarBase is a C++17 daemon (`starbased`) plus a companion CLI (`starbasectl`), a
MariaDB database it owns, and a dependency-free JavaScript single-page app it
serves itself. The core is a pipeline:

```
   image trees      ┌──────────────────────────────────────────┐
  (local/NFS/SMB) ─►│  Scanner: sweep threads + inotify + gate  │
                    └───────────────┬──────────────────────────┘
                                    │ file records
                    ┌───────────────▼──────────────────────────┐
                    │  Extractors: FITS (CFITSIO), XISF, sidecar│
                    │  + configurable header-dialect mapping    │
                    └───────────────┬──────────────────────────┘
  ┌────────────┐    ┌───────────────▼──────────────────────────┐
  │ starbasectl├───►│  Repository (MariaDB Connector/C) ◄─► DB  │
  └────────────┘    └───────────────┬──────────────────────────┘
                    ┌───────────────▼──────────────────────────┐
                    │  Query engine  │  Calibration matcher     │
                    └───────────────┬──────────────────────────┘
                    ┌───────────────▼──────────────────────────┐
                    │  Action engine: stage / fsop / export/wbpp│
                    └───────────────┬──────────────────────────┘
                    ┌───────────────▼──────────────────────────┐
                    │  HTTPS API + static SPA (cpp-httplib, TLS)│
                    └──────────────────────────────────────────┘
```

The functional core is split into small static libraries: configuration and
logging, name canonicalization, FITS/XISF extraction, the header-to-frame
resolver, the MariaDB repository, the indexer, the threaded scanner, the
filter-AST-to-SQL query compiler, the calibration matcher, the action engine, the
PixInsight bridge, PBKDF2 authentication, and the HTTP API. The daemon links them
together; the CLI links the subset it needs.

The **data model** is a MariaDB database named `starbase`, on the same instance
another tool may already use, sharing no tables. Frames are the center: one row per
image HDU, with the queryable characteristics promoted to columns and every raw
keyword also stored. Around them sit files and roots, the equipment tables
(cameras, telescopes, rigs, sites, filters, flat sets), calibration rules, tags and
collections, saved queries, an audited job ledger, and users and sessions. Views
present the joined, human-facing shape the UI reads.

### Design principles

These decisions are load-bearing and shape the rest of the system:

1. **The rescan is authoritative; inotify is only an accelerator.** A scan that
   only ever runs on schedule must still produce a correct index.
2. **The header is the truth, and the whole header is kept.** Promoted columns are
   a fast path, not the record of record.
3. **Header dialects are configuration, not code.** The keyword mapping is data you
   can extend without a rebuild.
4. **Never mutate the filesystem without a dry run.** Every destructive action
   previews, is audited as a job, and deletes to trash.
5. **No PixInsight code is linked or vendored.** Generating a script and running the
   `PixInsight` binary is arm's length and license-clean.

Full detail (components, the data model, the scanner tiers, the calibration
matcher, target-name canonicalization, and the PixInsight bridge) is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Installation

### From the Debian package (recommended)

```sh
sudo apt install ./starbase_<version>_amd64.deb
```

The debconf setup asks for the database password and host, whether to create the
`starbase` database and user for you, and the web UI bind address, port, and TLS
setting. It creates a `starbase` service user, installs and enables a systemd
unit, and starts the service. On first start the daemon builds its own tables and
seeds an `admin` / `admin` login **you must change immediately** from the web UI.

Two things to do after installing:

```sh
# 1. Let the service read your archive (use the group that owns it):
sudo usermod -aG astro starbase && sudo systemctl restart starbased

# 2. Register a directory tree to index (or do it from the Roots tab):
sudo -u starbase SB_DB_PASSWORD=... starbasectl add-root lights /path/to/lights
```

Then open `https://<host>:8642/`, log in, and scan the root from the Roots tab.

### From source

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
sudo cmake --install build           # installs under /usr/local/starbase
```

Or build the package yourself: `( cd build && cpack -G DEB )`.

---

## Using it

The web UI is organized into tabs: a **Dashboard** rollup, **Browse** (a filter
bar with discovered-value dropdowns), **Query** (the visual filter builder with
saved queries and an actions bar), **Jobs** (the action ledger), **Roots**,
**Tags** (tags and collections), **Database**, **Equipment** (rigs, sites, and
flat sets), **Users**, and an admin **Server** tab that changes the listening
interface without editing the config. Writes require a login; reads are open on
localhost.

`starbasectl` is the command-line companion: `add-root`, `list-roots`, `set-root`,
`remove-root`, `scan`, `probe <file>` (dump how a single file would be indexed),
and the `db-*` helpers. The database password is read from `SB_DB_PASSWORD`, and
`SB_DB_HOST` / `SB_DB_PORT` / `SB_DB_USER` / `SB_DB_NAME` override the config file.

Configuration lives in `/etc/starbase/starbase.conf` (INI style); secrets are read
from the environment, never the config file. Monitored roots are managed at runtime
through the CLI or the Roots tab, not the config, so they can be added, disabled,
or rescanned without restarting.

---

## Requirements

MariaDB 10.x with `libmariadb`, `cfitsio`, OpenSSL, CMake >= 3.16, and a C++17
compiler. PixInsight is optional and needed only for the WBPP handoff. The CDS
Sesame name resolver is optional and off by default.

The web UI has no build step and no runtime dependencies; it is vanilla JavaScript
served by the daemon.

---

## Relationship to NightWatcher

StarBase is related to the NightWatcher collection of observatory tools but is not
part of that toolset. It has its own repository, its own lifecycle, its own
database (`starbase`), and its own users and configuration. It reuses NightWatcher
*patterns* and can share the MariaDB *instance*; it shares no tables and depends on
nothing in those repositories.

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
