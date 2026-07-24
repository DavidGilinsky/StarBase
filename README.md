# StarBase

A queryable interface to astronomical image files.

StarBase runs as a Linux service. It watches directory trees of FITS (and XISF)
data, keeps a MariaDB index of every frame and its characteristics, and serves a
web UI where the central act is: build a query, get a set of frames, then do
something with that set.

What "something" means:

- hand the matched lights, and the calibration frames that actually belong to
  them, to **PixInsight WBPP**
- batch filesystem operations: tag, rename, move, copy, symlink
- run an external tool against the query results

It is related to the NightWatcher collection of tools but is not part of that
toolset: its own repository, its own lifecycle, its own database. It shares the
MariaDB *instance* NightWatcher2 uses, and nothing else.

## Design

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for components, data model,
scanning strategy, the calibration matcher, and the PixInsight bridge.

Shape of it: C++17 core (threaded walker, CFITSIO header extraction, query and
action engines, embedded HTTP server), MariaDB via Connector/C, a vanilla-JS
static SPA served by the daemon, CMake with a CPack `.deb`.

## Status

Bootstrapped 2026-07-23; architecture defined 2026-07-23. Implementation has not
started. Milestones are listed at the end of the architecture document; M1-M4
(schema, extractor, scanner, browse UI) is the smallest useful thing.

## Requirements

MariaDB 10.x, `libmariadb`, `cfitsio`, OpenSSL, CMake >= 3.16, a C++17 compiler.
PixInsight is optional and only needed for the WBPP handoff.

## License

GPL-3.0-or-later.
