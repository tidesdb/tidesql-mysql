---
title: Transactions and Isolation
description: The optimistic MVCC model, how SQL isolation levels map to the library, consistent snapshots, savepoints, and how write conflicts surface.
---

# Transactions and Isolation

TidesDB uses the library's multi-version concurrency control. Each statement runs inside a library
transaction, and the engine keeps a per-connection transaction context through the server's
handlerton interface, the same shape InnoDB uses. The transaction object is allocated lazily on the
first data access and registered with the transaction coordinator. After commit or rollback it is
kept and reused with `tidesdb_txn_reset()` on the next statement, which takes a fresh MVCC snapshot
while preserving the internal buffers, so an autocommit statement does not pay a malloc and free
each time.

Every DML statement runs at the resolved session isolation level, whether or not it is the only
statement in its transaction. An autocommit statement is simply the shortest transaction there is,
and it races other connections exactly as a longer one does.

## Isolation levels

The engine honors the session isolation level from `SET TRANSACTION ISOLATION LEVEL`. The mapping
to the library is:

| SQL isolation level | TidesDB |
|---------------------|---------|
| `READ UNCOMMITTED` | `TDB_ISOLATION_READ_UNCOMMITTED` |
| `READ COMMITTED` | `TDB_ISOLATION_READ_COMMITTED` |
| `REPEATABLE READ` | `TDB_ISOLATION_SNAPSHOT` |
| `SERIALIZABLE` | `TDB_ISOLATION_SERIALIZABLE` |

`REPEATABLE READ` resolves through the per-table `isolation_level` option. A table that leaves the
option at its default `REPEATABLE_READ` maps to the library's `SNAPSHOT`, which is the semantic
match for InnoDB's repeatable-read, a consistent-read snapshot with write-write conflict detection
and no read-set tracking. The library's own `REPEATABLE_READ` level is stricter, it tracks the read
set and detects read-write conflicts at commit, which produces excessive conflicts under normal
OLTP, so it is not the default. A table that sets `isolation_level` to `SNAPSHOT`, `SERIALIZABLE`,
`READ_COMMITTED`, or `READ_UNCOMMITTED` is honored as written, so a workload can pin a tighter or
looser level per table without moving the session default. A session that explicitly requests
`READ UNCOMMITTED`, `READ COMMITTED`, or `SERIALIZABLE` bypasses the table option and uses the
session level.

The available per-table levels are `READ_UNCOMMITTED`, `READ_COMMITTED`, `REPEATABLE_READ`,
`SNAPSHOT`, and `SERIALIZABLE`, set at `CREATE TABLE`:

```sql
CREATE TABLE ledger (
  id INT PRIMARY KEY, amt DECIMAL(10,2)
) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"isolation_level": "SERIALIZABLE"}';
```

DDL such as `ALTER TABLE`, `CREATE INDEX`, `DROP INDEX`, `TRUNCATE`, and `OPTIMIZE` runs at
`READ_COMMITTED` regardless of the session, which keeps a large scan from accumulating an unbounded
read set and costs nothing: the server's metadata locks already keep two schema changes off the same
table. DML never does — see the warning below.

## Optimistic concurrency, not locks

This is the architectural difference from InnoDB, and it is worth stating plainly. TideSQL does not
take pessimistic row locks. Concurrency is optimistic MVCC in the library. Readers never block
writers and writers never block readers. When two transactions modify the same row, both proceed,
and the second one to commit fails with a conflict rather than waiting.

Write-write conflict detection is a property of the higher isolation levels. At `SNAPSHOT` and
`SERIALIZABLE` the library validates each write against the version the transaction actually read,
recorded in its conflict footprint, and runs a first-committer-wins check before the write log, so
two transactions that modify the same row cannot both commit.

At `READ_COMMITTED` and below the library does no write-write checking at all, and a write is a
blind overwrite: if another transaction wrote the same key first, that write is simply lost. That
is the right trade for appending independent records, and the wrong one for anything that reads a
row and writes it back. It is also not a level the engine will pick for a DML statement on your
behalf — a statement that loses its row write while the secondary-index entries it wrote survive
leaves an index naming a row that is not there, and nothing later reports it. A session that asks
for `READ COMMITTED` explicitly gets those semantics, and owns them.

The check runs inside `tidesdb_txn_commit()`, so a detected conflict fails the losing transaction's
`COMMIT`. The engine maps `TDB_ERR_CONFLICT` to `HA_ERR_LOCK_DEADLOCK`, but the server reports any
error raised from the commit callback as `ER_ERROR_DURING_COMMIT` (1180) rather than
`ER_LOCK_DEADLOCK` (1213), so a commit-time conflict reaches the application like this:

```
ERROR 1180 (HY000): Got error 149 - 'Lock deadlock; Retry transaction' during COMMIT
```

The transaction is rolled back cleanly and should be retried. The server does not retry it for you,
so **any** application writing concurrently to the same rows needs retry logic for 1180 — including
one that only uses autocommit statements, since those validate their writes too.

Plain reads never take a lock at any isolation level, matching InnoDB's non-locking reads. Whether a
read is recorded into the conflict footprint depends on the isolation level, not on `FOR UPDATE` or
`LOCK IN SHARE MODE`, which the engine treats as ordinary reads. At the default level, which maps to
the library's snapshot isolation, reads are not tracked and only write-write conflicts are caught. At
`SERIALIZABLE` the engine also tracks the read set, so a concurrent write to a row this transaction
only read makes it lose the first-committer-wins check at commit. There is no lock wait and no
wait-for-graph deadlock, only the commit-time conflict.

Neither model is strictly better. Optimistic MVCC removes all lock waits and lock-manager overhead,
and a low-contention workload where most rows are touched by at most one writer at a time sees
almost no conflicts. A workload with many transactions contending on a few hot rows, such as a
single counter row, will see conflicts and depends on efficient retry.

## Consistent snapshots

`START TRANSACTION WITH CONSISTENT SNAPSHOT` is supported. It eagerly creates the transaction and
captures the snapshot sequence immediately rather than at the first data access, and it forces the
isolation to at least `SNAPSHOT`, since a lower level would refresh the snapshot on each read and
break the consistent-snapshot contract. Rows committed by other connections after the snapshot are
invisible. This is useful for cross-engine consistency when TidesDB and InnoDB tables share a
transaction:

```sql
SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
START TRANSACTION WITH CONSISTENT SNAPSHOT;
SELECT * FROM tidesdb_table;   -- sees data as of snapshot time
SELECT * FROM innodb_table;    -- InnoDB snapshotted at the same point
COMMIT;
```

## Savepoints

SQL savepoints (`SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, `RELEASE SAVEPOINT`) are supported inside
explicit multi-statement transactions. They are only meaningful within a `BEGIN ... COMMIT` block.

## Bulk DML batching

Statements that write many rows — `LOAD DATA INFILE`, multi-row `INSERT`, `INSERT ... SELECT`, and a
range `DELETE` — keep the transaction from growing without bound by committing mid-statement in
fixed-size batches. The engine hooks `start_bulk_insert` and `start_bulk_delete`, counts row
operations (the data write plus secondary-index maintenance) against a batch size of 500, and at each
threshold commits the current transaction and resets it at the same isolation level for the next
batch, so the rest of a long statement is validated exactly as its first rows were. Statement memory
stays bounded regardless of statement size, and the statement reports the first error it hit. Both
paths share one mid-statement commit helper, so the threshold and the iterator and dup-cache
invalidation are identical on each.

A statement that commits in pieces reads under a snapshot of its own, taken before it wrote
anything, rather than under the transaction those commits reset. This is what lets a statement both
scan and write: `INSERT ... SELECT` and a copy-based `ALTER` read the source while the destination
commits batch after batch, and resetting the writing transaction would otherwise end the reading
scan where it stood. It also fixes the rows the statement sees: a self-referencing
`INSERT INTO t SELECT ... FROM t` copies the rows that were in `t` when it started and cannot read
its own output.

`UPDATE` is deliberately excluded. The server offers a batched-update path, but a multi-row `UPDATE`
routed through it is never written to the binary log: `handler::ha_update_row` logs the row it
changed, and `handler::ha_bulk_update_row` does not. An engine that accepts batching there loses
every multi-row `UPDATE` from replication and from point-in-time recovery, silently. So the engine
declines it, exactly as InnoDB does, and a large `UPDATE` holds its whole statement in one
transaction instead.

What that costs is described in [Limitations](/appendix/limitations): a batched statement is not
atomic against a crash, and with binary logging on the engine can make batches durable before the
binlog cache is flushed.

## Group commit

Commits are ordered by the server's binlog group-commit machinery, and the transactions in one
commit round share the cost of a single durability barrier, which is what keeps `FULL` sync
affordable under load — see [Durability and Sync Modes](/concepts/durability).

The engine takes no part in the ordering. MySQL orders commits inside the binlog coordinator and
publishes no handlerton hook an engine could register for, so the engine's durable commit simply runs
on the ordinary commit path and inherits the ordering the coordinator imposes around it.

## Crash recovery and two-phase commit

With the binary log enabled, TideSQL is a full two-phase commit participant, so it recovers to a
consistent point after a crash and coordinates with the binlog and with external XA. In the prepare
phase the engine durably logs the transaction's write batch under its XID, so a crash after prepare
but before commit leaves an in-doubt transaction that recovery resolves rather than loses. A
prepared transaction is held in a process-wide registry, so its commit or rollback decision can
arrive from another connection or after a restart.

With the binary log off the engine does not offer two-phase commit, `SHOW ENGINES` reports `XA: NO`,
and `XA START` is refused. That is not a gap in the engine but a deliberate refusal to reach a
server path that cannot carry it: the binary log is the transaction coordinator when it is on, and
when it is off a second two-phase-capable engine makes the server coordinate through a fixed-size
log whose commit path does not release a transaction's slot when an engine's commit fails. A slot
never released pins its page, and enough of them stop every later commit in the server. An engine
whose commit cannot fail never reaches that path; TideSQL validates writes at commit, so under
concurrency it would. Enable `--log-bin` to use XA. Single-engine transactions, savepoints, and
crash recovery of the engine's own data are unaffected and work either way.

When the server restarts it asks the engine to recover, and the engine replays every in-doubt
prepared transaction so the coordinator can commit or roll each one back to match the binlog. An
external `XA PREPARE` detaches the transaction to the same registry, so a distributed transaction
survives the client disconnecting between `XA PREPARE` and `XA COMMIT`:

```sql
XA START 'txn1';
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
XA END 'txn1';
XA PREPARE 'txn1';
-- the prepared transaction is durable and can be committed from any connection
XA COMMIT 'txn1';
```
