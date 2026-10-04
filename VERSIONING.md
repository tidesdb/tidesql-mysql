# Versioning

TideSQL is a storage engine that embeds the TidesDB library. It follows
[SemVer](https://semver.org/) (MAJOR.MINOR.PATCH). Two contracts version here
and they move independently. One is the SQL surface the plugin presents. The
other is the on-disk format, which TideSQL does not define itself but inherits
whole from the TidesDB release it links.

**Public surface** is what a SQL user or an operator can depend on - the
`TidesDB` engine name, the per-table storage options, the system and status
variables, and the observable SQL behavior of the engine. Anything reachable
only by editing plugin source carries no compatibility guarantee.

TideSQL is a MySQL storage engine. Where a MySQL release changes something the
engine depends on, the difference is resolved in `ha_tidesdb_compat.h` rather
than in engine code. The capabilities MySQL does and does not offer an engine
are listed in **What MySQL offers the engine** below, and a change to that list
versions like any other change to the public surface.

## TideSQL 2.0.0 pairs with TidesDB v10.1.1

The pairing is enforced at build time, not left to the operator: the plugin's
CMake reads `TIDESDB_VERSION` from the installed `tidesdb/tidesdb_version.h` and
refuses to configure against a library whose major is not the one this TideSQL
major pairs with. The linker would not catch it -- the soname carries the major,
so a mismatch surfaces later as a missing library at load, or as a format the
engine misreads.

Each TideSQL release links exactly one TidesDB release and stores data in that
library's on-disk format. TideSQL 2.0.0 links TidesDB v10.1.1 and writes the
v10 format line. The plugin version and the library version keep their own
cadence, so the pairing is recorded here and surfaced at runtime through the
`tidesdb_version` status variable for the plugin and `tidesdb_library_version`
for the linked library.

TideSQL 2.0.0 ships at **beta** maturity.

## Major
- The linked TidesDB major changes, which opens a new on-disk format line.
- A storage option, system variable, or status variable is removed or changes
  meaning.
- Observable SQL behavior changes in a way that can affect results or query
  plans.
- A database created by the prior major needs a dump and reload rather than an
  in-place upgrade.

## Minor
- Backward-compatible additions to the SQL surface, such as a new storage
  option, a new system or status variable, or a new engine capability.
- Reads every database written by prior minors of the same major.
- Any new on-disk behavior from the linked library is opt-in and default-off, so
  a downgrade stays possible.

## Patch
- Bug and security fixes in the plugin.
- A patch bump of the linked TidesDB library that does not change the on-disk
  format or observable behavior.
- Must not change the SQL surface or the on-disk format, so it is safe to apply
  without reading release notes.

## What MySQL offers the engine

<!-- What an operator has to know about the boundary between engine and server. -->

Some of the engine's behaviour is its own and some is whatever MySQL hands it.
These are the places where the server decides, and each is a property of MySQL
rather than a choice the engine made.

| Capability | On MySQL |
|------------|----------|
| Per-table storage options | `ENGINE_ATTRIBUTE='{"compression": "ZSTD", ...}'` |
| Per-row expiry column | `col INT ENGINE_ATTRIBUTE='{"ttl": true}'` |
| Encryption key custody | keyring component, two-tier: master key wraps table keys |
| Encryption key rotation | `SET GLOBAL tidesdb_rotate_table_key` / `tidesdb_rotate_master_key` |
| Partitioned tables | not available - the server has no general partitioning layer, and this engine does not implement its own |
| System-versioned tables | not available - the server has no system versioning |
| Multi-primary clustering | not available - Group Replication's certification layer is InnoDB-only |
| Bulk load | not available - `LOAD DATA ... ALGORITHM = BULK` needs a driver component community builds do not ship |
| Parallel scan | not available - the server declares the interface but never calls it |
| Killed query noticed during index-condition evaluation | at the next row boundary instead - the server's pushed-condition result has no killed state |

An option a table does not name is resolved from the session `tidesdb_default_*`
variable **when the table is created** and stays with the table from then on.
The engine records the resolved set in the table's own storage to make that
true, so changing a session default never changes a table that already exists.

## Compatibility matrix

<!-- Records the durability guarantees operators check before touching production. -->

The on-disk format belongs to the linked TidesDB library. TidesDB stamps one
format number into every file it writes and checks it for **exact equality** on
read, rejecting any other version outright. The rollback boundary for a TideSQL
release is therefore the set of releases that link a library writing the same
format line.

| TideSQL | TidesDB library | On-disk format | Rollback boundary            |
|---------|-----------------|----------------|------------------------------|
| 2.0.0   | 10.1.1          | 10             | any TideSQL linking format 10 |

- **TideSQL 2.0.0 writes the v10 format line** by linking TidesDB v10.1.1. It
  reads only the v10 format, so a database created by an earlier release line
  does not open in place and no in-place migration ships for that step. A
  database from an earlier release moves across by dumping with `mysqldump` and
  reloading through SQL.
- **The on-disk format is the library's, not the server's** - the column
  families, the row format, the key encoding and the engine's own metadata come
  from the linked TidesDB release, so any TideSQL build at the same version
  reads the same store.
- **Rollback across TideSQL 2.x is unrestricted** while every 2.x minor links a
  TidesDB release that writes format 10. A minor that moves to a new format must
  make it opt-in and default-off, which is what keeps that column true, and the
  release that changes it updates this table in the same commit.

Add a row per release. A release that moves none of the columns still gets a
row, because "unchanged" is the answer an operator is looking for.
