# StarBase

StarBase is a standalone astronomy project. It is **related to** the NightWatcher
collection of tools but is deliberately **not part of the toolset**: its own
repository, its own lifecycle, and no dependency on the shared `nwdb` database or
the NightWatcher extension registry.

> **Purpose / architecture: TBD.** Fill this in once the project is defined
> (what it does, language, build system, deliverables), then delete this note.

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
