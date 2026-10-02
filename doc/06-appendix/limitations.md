---
title: Limitations
description: What TideSQL does not do on this server, and the behaviors to plan around.
---

# Limitations

## Not available on this server

These are capabilities the server does not offer an engine, rather than choices TideSQL made. Each
is refused clearly rather than accepted and quietly ignored.

**Partitioned tables.** This server has no general partitioning layer, and TideSQL does not
implement native partitioning of its own, so every partitioned `CREATE TABLE` is refused. See
[Partitioning](/administration/partitioning) for what to use instead.

**System-versioned tables.** The server has no system versioning, so `WITH SYSTEM VERSIONING` does
not parse.

**Vector search.** A `VECTOR` column stores and reads back, because the server models the type as a
blob and the engine stores blobs — there is no vector-specific code in TideSQL or in the TidesDB
library. What does not exist is any similarity search: the server has neither a vector index nor a
distance function, and indexing the column is refused outright with `ER_NON_SCALAR_USED_AS_KEY`. See
[Vector Columns](/reference/vector-columns).

**Multi-primary clustering.** The server carries no write-set replication integration, so there is
nothing for the engine to participate in. Source-and-replica topologies work normally. See
[Replication and High Availability](/administration/replication-ha).

**Bulk load.** `LOAD DATA ... ALGORITHM = BULK` is refused before any engine is reached:

```
ERROR 1235 (42000): This version of MySQL doesn't yet support 'Bulk Load'
```

The server's bulk loader is driven by a `bulk_load_driver` component. The service is declared in the
server's headers but no component in a community build implements it, so the statement fails while
checking for the driver and the engine's side of the interface is never called. Ordinary `LOAD DATA`
works and is the way to load a TideSQL table.

**Parallel scan.** The server declares a parallel-scan interface an engine may offer, but nothing in
the server calls it — it is used by InnoDB on itself, and by a loader that is not part of a
community build. There is no statement that would reach the engine through it, so TideSQL does not
implement it. Scans within one statement are single-threaded; concurrent statements scan
concurrently as usual.

## Behaviours to plan around

**Write conflicts surface at commit and are the application's to retry.** Concurrency is optimistic
MVCC, so a transaction can fail at commit with a first-committer-wins conflict, which reaches the
client as `ER_ERROR_DURING_COMMIT` (1180):

```
ERROR 1180 (HY000): Got error 149 - 'Lock deadlock; Retry transaction' during COMMIT
```

This applies to autocommit statements as much as to explicit `BEGIN ... COMMIT` blocks — an
autocommit statement is a transaction too, and it validates its writes for the same reason. The
server does not retry it for you, so any application writing concurrently to the same rows needs
retry logic for 1180. There are no pessimistic row locks, so there are no lock waits and no
lock-wait deadlocks to tune. See [Transactions and Isolation](/concepts/transactions).

**XA needs the binary log.** With `--log-bin` off the engine does not offer two-phase commit:
`SHOW ENGINES` reports `XA: NO` and `XA START` is refused. With it on, XA works in full. The reason
is a server limitation rather than an engine one -- without the binary log, a second
two-phase-capable engine makes the server coordinate through a fixed-size transaction log whose
commit path does not release a slot when an engine's commit fails, and enough unreleased slots stop
every later commit in the server. Ordinary transactions, savepoints and crash recovery are
unaffected. See [Transactions and Isolation](/concepts/transactions).

**`READ COMMITTED` does not detect write conflicts.** A session that asks for it explicitly gets a
level at which a write is a blind overwrite: if another transaction wrote the same row first, that
write is lost. That is the documented behaviour of the level and the right trade for appending
independent records, but it is not a level to run read-modify-write traffic at.

**A foreign key column cannot be declared descending.** TideSQL enforces foreign keys inside the
engine, including `ON DELETE` and `ON UPDATE` with `CASCADE`, `SET NULL`, and `RESTRICT`, references
to a primary key or to a unique key whether or not its columns are nullable, and self-references.
One constraint shape is rejected at `CREATE TABLE` and `ALTER TABLE`: a foreign key column declared
with descending order, because the engine matches child rows against a forward sort key. Everything
else behaves as in InnoDB. See [Foreign Keys](/reference/foreign-keys).

**Changing the primary key or a column type needs a full copy.** The engine does not support an
inplace primary-key change, and changing a column type such as `INT` to `BIGINT` also rebuilds the
table by copy. See [Online DDL](/administration/online-ddl).

**A very large INSERT or DELETE commits in pieces.** A statement touching more rows than the
engine's batch threshold commits its work to storage part-way through rather than holding the whole
statement in one transaction, which is what keeps a multi-million-row load from growing the
transaction without bound. Two consequences follow, and both are about the boundary rather than the
result:

- The statement is not atomic against a crash. A server that dies in the middle of one leaves the
  batches that had already committed, not an all-or-nothing outcome.
- With binary logging on, the row events for the statement are written to the binlog cache and
  flushed when the statement commits, while the engine has already made earlier batches durable. A
  crash in that window leaves the engine holding rows the binlog never recorded, so a replica built
  from that binlog, or a point-in-time recovery through it, is missing them.

`UPDATE` is not affected: it is applied one row at a time through the path the server logs from, so
it commits once at the end. Where a large write has to be atomic and replicated exactly, split it
into transactions you control rather than relying on one statement.

**Statistics are cached for up to two seconds.** Right after a bulk load the optimizer may briefly
see stale row counts. `ANALYZE TABLE` forces an immediate refresh.

**A killed query is noticed at the next row boundary.** The server's pushed-condition interface here
has no state for reporting a kill, so an index scan evaluating a pushed condition observes
`KILL` at the row boundary the scan already checks rather than inside condition evaluation. The
difference is one row.

**Index condition pushdown is taken on secondary indexes only.** A condition on the columns of a
secondary index being scanned is pushed into the engine and evaluated against the index entry, which
skips the row fetch for an entry that cannot match — that is the pushdown worth having, and TideSQL
implements it. A primary-key scan declines it, because there is nothing to win there: what the
pushdown saves is the base-row fetch an index entry would otherwise need, and a primary-key scan is
already holding the row. `EXPLAIN` shows the difference — `Using index condition` on a
secondary-index scan, `Using where` on a primary-key range — and the rows returned are the same
either way.

**Whole-condition pushdown is not taken at all.** The separate interface for pushing an entire
`WHERE` clause is offered only to single-table `UPDATE` and `DELETE`, and the server evaluates the
clause again for every row the engine returns regardless of what the engine reports back. Taking it
would duplicate work the server repeats anyway and would evaluate user expressions twice, so TideSQL
leaves it alone, as InnoDB does. `EXPLAIN` never shows `Using pushed condition`.

**The plugin may need the allocator preloaded to load at all.** When the linked TidesDB library was
built against `jemalloc`, `mimalloc` or `tcmalloc`, the plugin cannot be `dlopen`ed without the
allocator already in the process image. See [Installing TideSQL](/getting-started/install), which
has the exact error and the fix.
