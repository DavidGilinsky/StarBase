# StarBase

StarBase is a standalone astronomy project. It is **related to** the NightWatcher
collection of tools but is deliberately **not part of the toolset**: its own
repository, its own lifecycle, and no dependency on the shared `nwdb` database or
the NightWatcher extension registry.

## What it is

A queryable interface to astronomical image files. `starbased` runs as a Linux
service, monitors directory trees of FITS (and XISF) data, maintains a MariaDB
index of every frame and its characteristics, and serves a web UI whose central
act is: build a query, get a set of frames, do something with that set. The
principal consumer is PixInsight WBPP; the rest is batch filesystem operations
and running external tools against query results.

- **Language:** C++17 for the functional core (threaded walker, extractors,
  query and action engines, HTTP API). Vanilla-JS static SPA for the web UI, no
  build step.
- **Build:** CMake >= 3.16, self-contained prefix `/usr/local/starbase`,
  CPack `.deb` with debconf, systemd unit. Mirrors NightWatcher2.
- **Database:** its own `starbase` database on the same MariaDB instance
  NightWatcher2 uses, via MariaDB Connector/C. No shared tables.
- **FITS:** CFITSIO, headers only. **XISF:** the open spec, parsed directly.
  No PixInsight code is vendored or linked (PCL License 2.0 is not GPL).

Full design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Read it before
touching the scanner, the schema, or the PixInsight bridge; several decisions
there (rescan-is-authoritative, keep-the-whole-header, header-dialects-are-config,
never-mutate-without-a-dry-run) are load-bearing and non-obvious.

## Conventions

Follow the global `~/.claude/CLAUDE.md`: a file-header block on every source file
(author, file, purpose, created/last-modified, version, and a GPL-3.0-or-later
license line for open-source work); MariaDB Connector/C (`libmariadb`), not
Oracle's client, for any GPL project that talks to MySQL/MariaDB; lean,
no-em-dash writing voice; autonomous execution (run ordinary commands, report
back; confirm only irreversible or outward-facing actions).

## Sibling NightWatcher repos — reference only, not dependencies

Under `/home/gilinsky/devel/astronomy/`. Read them for reusable patterns; do not
couple StarBase to them.

- **NightWatcher-v2** — C++ SQM toolset: cpp-httplib web server, PBKDF2 auth,
  self-signed TLS, CIDR device discovery, CPack `.deb` + debconf packaging.
- **NightWatcher-AirWatcher** — C++ daemon: libsmbclient copy engine with a
  copy/delete policy scheduler, the NightWatcher extension-registry pattern, and
  its own embedded web UI.
- **nightwatcher-ingest** — Python FITS watcher/classifier/filer (config-driven).
