<!--
  Author:        David Gilinsky
  File:          docs/INTEGRATIONS.md
  Purpose:       StarBase's public, read-only integration contracts for sibling
                 tools. What other tools may read, and the stability guarantees.
  Created:       2026-07-26
  Last Modified: 2026-07-26
  Version:       0.1.0
  License:       GPL-3.0-or-later
-->

# StarBase Integrations

StarBase exposes a small number of **read-only** contracts that sibling tools may
consume. The rule is one-directional: a dependency points **consumer -> StarBase**,
never the reverse. StarBase does not read from, know about, or depend on any
consumer's database or code. Each contract below is a database object plus a
grant; nothing more couples the tools.

## `v_rig_resolve` — rig resolution (consumed by nightwatcher-ingest)

`nightwatcher-ingest` files FITS frames into the archive and names equipment
folders from StarBase's rigs. To do that it needs the same
(canonical camera model + focal length) -> rig-name match that StarBase's C++
`EquipmentResolver` performs when it stamps `rig_id` on a frame. Rather than
duplicate that rule, StarBase exposes it as a view.

### The contract

```sql
CREATE OR REPLACE VIEW v_rig_resolve AS
SELECT c.model        AS camera_model,
       r.focal_min_mm AS focal_min_mm,
       r.focal_max_mm AS focal_max_mm,
       r.name         AS rig_name
FROM rigs r
JOIN cameras c ON c.id = r.camera_id
WHERE r.status = 'active';
```

The consumer's query is:

```sql
SELECT rig_name FROM v_rig_resolve
WHERE camera_model = ? AND ? BETWEEN focal_min_mm AND focal_max_mm
LIMIT 1;
```

**These four columns (`camera_model`, `focal_min_mm`, `focal_max_mm`,
`rig_name`) are a frozen public interface.** They are consumed outside this
repository and must stay stable in name, type, and meaning even if the internal
`rigs` / `cameras` tables are refactored. Change the view's backing query freely;
do not change what these four columns present. The view is defined in
`sql/schema.sql`, which the daemon re-applies (idempotently) on every start, so
it exists on both fresh installs and upgraded databases with no separate
migration.

### Granting the consumer read access

The one and only cross-tool coupling is a single `SELECT` grant. It is **not**
applied by StarBase's own setup, because it needs the consumer's `nightwatcher`
user to already exist. Apply it as root once both tools are installed
(idempotent; re-run after either is reinstalled):

```sh
sudo mariadb < /usr/local/starbase/sql/grant-rig-resolve.sql
```

which runs:

```sql
GRANT SELECT ON starbase.v_rig_resolve TO 'nightwatcher'@'localhost';
GRANT SELECT ON starbase.v_rig_resolve TO 'nightwatcher'@'127.0.0.1';
FLUSH PRIVILEGES;
```

**Stricter option.** Instead of granting to the shared `nightwatcher` user,
create a dedicated read-only user scoped to just this view and point the consumer
at it (`nwingest_ro` in the file above). Nothing else in StarBase changes.

### Requirements for a match

The lookup returns a row only when the equipment is defined, so on the StarBase
side:

- **Rigs must exist and be `active`.** With no active rigs, `v_rig_resolve` is
  empty and the consumer resolves nothing. Define rigs from the Equipment tab (or
  `sql/equipment.example.sql`).
- **`cameras.model` must hold the canonical body string** (`ASI2600`, `ASI6200`,
  ...). ZWO cameras align automatically with the consumer's `ASI\d{3,4}`
  normalization. For any non-ZWO body, add a `camera_aliases` row mapping the
  header's `INSTRUME` spelling to the canonical model, so both tools agree on
  `camera_model`.
- **Keep per-camera focal ranges non-overlapping among active rigs.** The
  consumer uses `LIMIT 1`, so two active rigs on the same camera with overlapping
  `[focal_min_mm, focal_max_mm]` make the winner non-deterministic, the same
  first-match-wins caveat `EquipmentResolver` carries. The Equipment builder
  warns when a rig's range overlaps another active rig on the same camera.

### What is explicitly out of scope

StarBase does not name folders, does not depend on the nightwatcher database, and
carries no code aware of the consumer beyond the read grant. Folder naming, and
any use of the resolved rig name, belong entirely to the consumer.
