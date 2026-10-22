---
title: Backup and Checkpoint
description: Online backup, checkpoints that make the live database durable, and why there is no per-table physical copy.
---

# Backup and Checkpoint

## Online backup

`tidesdb_backup()` writes a consistent, directly-openable copy of the whole TidesDB data directory:

```sql
SELECT tidesdb_backup('/path/to/backup');
+-----------------------------------+
| tidesdb_backup('/path/to/backup') |
+-----------------------------------+
| OK                                |
+-----------------------------------+
```

The backup runs without blocking reads and writes. It begins by flushing the memtable, then copies
the manifest, the shared value log, and every SSTable the manifest references, all at one manifest
snapshot with compaction held off, so the copy can never reference a file a merge deleted part-way
through. The engine frees the calling connection's own transaction before starting, because the
backup waits for open transactions to drain and would otherwise wait on the caller.

Three things to know before scheduling one:

- **The copy is as of the flush it starts with.** Writes committed while the backup runs are not in
  it. The database stays writable throughout.
- **It is a copy, not a link.** Both the time it takes and the free space it needs are proportional
  to the live on-disk size.
- **The destination must be absent or empty.** It is created if absent, and it must not be the live
  data directory. A path that already holds files is refused before anything is written, so a second
  backup over an earlier one fails the statement instead of mixing the two in one directory; the
  earlier backup is left intact. Give each backup a fresh path, or clear the directory first.

It is a function rather than a setting because a backup is something the server does once, not a
state it is in — the path is where a backup went, not how the engine is configured. A backup that
fails fails the statement, with the reason in the error, so a scheduled job sees it without reading
the error log.

## Checkpoint

`tidesdb_checkpoint()` does two things in order: it takes a full durability barrier on the live
database, then writes a backup to the given path.

```sql
SELECT tidesdb_checkpoint('/path/to/checkpoint');
```

The barrier flushes the memtable and forces the value log, the write-ahead log, and the manifest to
disk regardless of the configured sync mode, so when it returns everything committed beforehand is
on the device. The copy that follows is the same copy `tidesdb_backup()` writes, with the same
properties as above.

The difference between the two is what happens to the **live** database, not to the copy. A backup
leaves the live database's durability exactly as the sync mode had it; a checkpoint makes the live
database fully durable as part of the operation. Reach for a checkpoint before something risky — a
host reboot, a storage migration, a filesystem-level snapshot — and for a backup when all you need
is the second copy.

## There is no per-table physical copy

`FLUSH TABLES ... FOR EXPORT` is refused:

```sql
FLUSH TABLES orders FOR EXPORT;
```

```
ERROR 1031 (HY000): Table storage engine for 'orders' doesn't have this option
```

The statement promises that holding a table still leaves files on disk you can copy out and read
back as that table. TideSQL has no such files. The data directory is flat rather than one directory
per table: a single manifest, one shared value log, and SSTables that carry whichever column
families were flushed together, with the most recent rows still in a memtable shared by every table
in the server. No subset of that is one table. Rather than accept the statement and hand back an
incomplete copy without saying so, the engine does not claim the capability and the server refuses
it.

To move one table, use `mysqldump` or `SELECT ... INTO OUTFILE` and load it back. To copy the store,
use `tidesdb_backup()` above, which is the operation that produces a directory a server can
actually open.
