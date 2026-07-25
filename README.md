# StarBase

A queryable interface to astronomical image files.

StarBase runs as a Linux service. It watches directory trees of FITS (and XISF)
data, keeps a MariaDB index of every frame and its characteristics, and serves a
web UI where the central act is: build a query, get a set of frames, then do
something with that set.

What "something" means:

- hand the matched lights, and the calibration frames that actually belong to
  them, to **PixInsight WBPP** (a staging tree plus a rendered command, or a
  live pull from inside PixInsight via the bundled PJSR helper)
- tag frames and gather them into named collections, then query by membership
- stage, export (CSV / JSON / path list), or run filesystem operations
  (copy / symlink / move / trash) over a result set, always with a dry-run
- normalize target names to the catalog form (`M101` -> `M 101`), optionally
  resolving cross-designations against SIMBAD

It is related to the NightWatcher collection of tools but is not part of that
toolset: its own repository, its own lifecycle, its own database. It shares the
MariaDB *instance* NightWatcher2 uses, and nothing else.

## Install

### From the Debian package (recommended)

```sh
sudo apt install ./starbase_<version>_amd64.deb
```

The installer (debconf) asks for the database password and host, whether to
create the `starbase` database and user for you, and the web UI bind address /
port / TLS. It creates a `starbase` service user, installs a systemd unit, and
enables the service. On first start the daemon builds its own tables and seeds
an `admin` / `admin` login **you must change immediately** from the web UI.

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

Or build the `.deb` yourself: `( cd build && cpack -G DEB )`.

## Using it

The web UI has nine tabs: **Dashboard** (totals and a target rollup),
**Browse** (a filter bar with discovered-value dropdowns), **Query** (a visual
filter-AST builder with saved queries and an actions bar), **Jobs** (the action
ledger), **Roots**, **Tags** (tags & collections), **Database**, **Users**, and
an admin **Server** tab that changes the listening interface without editing the
config. Writes require a login (admin / user / readonly roles); reads are open.

`starbasectl` is the CLI: `add-root`, `list-roots`, `set-root`, `remove-root`,
`scan`, `probe <file>`, and the `db-*` helpers. The database password is read
from `SB_DB_PASSWORD`; `SB_DB_HOST/PORT/USER/NAME` override the config file.

## Design

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for components, the data model,
scanning strategy, the calibration matcher, target-name canonicalization, and
the PixInsight bridge.

Shape of it: C++17 core (threaded walker, CFITSIO/XISF header extraction, query
and action engines, embedded HTTPS server with PBKDF2 auth), MariaDB via
Connector/C, a vanilla-JS static SPA served by the daemon, CMake with a CPack
`.deb` (debconf + systemd). No PixInsight code is linked or vendored.

## Status

M0-M10 complete: schema, extractors (FITS + XISF), threaded scanner, the full
web UI and REST API, the query and calibration engines, the action engine and
WBPP bridge, sidecars, tags/collections, users/auth, the server-settings tab,
target-name normalization, and Debian packaging. The daemon self-bootstraps its
schema, so a fresh database needs no manual setup.

## Requirements

MariaDB 10.x, `libmariadb`, `cfitsio`, OpenSSL, CMake >= 3.16, a C++17 compiler.
PixInsight is optional and only needed for the WBPP handoff; the CDS Sesame name
resolver is optional and off by default (the only feature that reaches the
network).

## License

GPL-3.0-or-later.
