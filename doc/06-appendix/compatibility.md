---
title: Server Compatibility
description: Which MySQL versions TideSQL is tested against, the current release, and which capabilities the server offers an engine.
---

# Server Compatibility

TideSQL is built against a server source tree and tracks that server's storage-engine interface, so
a given TideSQL release targets specific server versions. The table records which have been tested
and confirmed working. Full support means the engine is tested against all known functionality on
that server version.

| Server version | Minimum TideSQL version | Full support |
|----------------|-------------------------|:------------:|
| MySQL 9.7.0 | 2.0.0 | Yes |
| MySQL 26.7.0 | 2.0.0 | Yes |

This is the TideSQL 2.x manual and it pairs with TidesDB v10. The current pinned release is 2.0.0,
linking TidesDB v10.1.0, at **beta** maturity. A 2.x minor or patch links a TidesDB v10 release and
extends this same manual, so only a new major opens a new manual. See
[Versioning](https://github.com/tidesdb/tidesql/blob/master/VERSIONING.md) for how the plugin and
the library versions relate.

As versions are tested and confirmed working this table is updated.

## What the server offers an engine

Some of what a storage engine can do is the engine's own; the rest is whatever the server hands it.
This is the second kind — the capabilities MySQL does or does not offer, and how TideSQL reaches
each one. The manual says so again at the point each matters:

| Capability | On MySQL |
|------------|-------------|
| Per-table storage options | Through `ENGINE_ATTRIBUTE`; see [Table Options](/reference/table-options) |
| Per-row expiry column | Through a column's `ENGINE_ATTRIBUTE`; see [Time-To-Live](/reference/ttl) |
| Encryption key custody | A keyring component, two-tier; see [Data-at-Rest Encryption](/reference/encryption) |
| Partitioned tables | Not available; see [Partitioning](/administration/partitioning) |
| System-versioned tables | Not available — the server has no system versioning |
| Multi-primary clustering | Not available; see [Replication and High Availability](/administration/replication-ha) |
| Column histograms | `ANALYZE TABLE ... UPDATE HISTOGRAM`, sampled by the engine; see [Table Maintenance](/administration/maintenance) |
| Bulk load | Not available — the server's loader needs a driver component community builds do not ship; see [Limitations](/appendix/limitations) |
| Foreign keys | Enforced by the engine, with the shape restrictions in [Limitations](/appendix/limitations) |

A database written by one TideSQL build is readable by any other at the same version: the column
families, the row format, the key encoding and the engine's own metadata are fixed by the linked
TidesDB release rather than by the server. See
[Versioning](https://github.com/tidesdb/tidesql/blob/master/VERSIONING.md) for the rollback
boundary that follows from it.
