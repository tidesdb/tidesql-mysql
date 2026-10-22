---
title: Time-To-Live
description: Table-level, per-row, and per-session row expiration, and the order they resolve in.
---

# Time-To-Live

TidesDB can expire rows automatically, at the table level, the row level, or the session level.
An expired row is filtered out of reads and reclaimed at compaction. Every TTL is expressed in
seconds of lifetime from the time of the write.

## Table-level TTL

Every row inserted into the table expires after the given number of seconds:

```sql
CREATE TABLE sessions (
  id INT PRIMARY KEY, token VARCHAR(100)
) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"ttl": 3600}';   -- one hour

INSERT INTO sessions VALUES (1, 'abc123');
-- after 3600 seconds this row is no longer returned
```

## Per-row TTL

A column can be marked as the TTL source with its own `ENGINE_ATTRIBUTE`. The value in that column
is the row's lifetime in seconds from insertion:

```sql
CREATE TABLE cache (
  id INT PRIMARY KEY, val VARCHAR(100),
  ttl_sec INT ENGINE_ATTRIBUTE='{"ttl": true}'
) ENGINE=TIDESDB;

INSERT INTO cache VALUES (1, 'short-lived', 5);      -- expires in 5 seconds
INSERT INTO cache VALUES (2, 'long-lived', 86400);   -- expires in a day
INSERT INTO cache VALUES (3, 'permanent', 0);        -- 0 defers to session and table TTL, unset here, so no expiry
```

At most one column may be marked as the TTL source; a table naming two is refused at `CREATE`,
because which one won would otherwise depend on the order the columns happened to be read in.

A non-zero per-row value takes precedence. If it is zero, resolution falls through to the session
TTL and then the table TTL. Updating a row recomputes its TTL from the new column value, which
refreshes the expiration.

## Session-level TTL

`tidesdb_ttl` applies a TTL to every INSERT and UPDATE on any TidesDB table for the session, even
tables not created with a TTL option:

```sql
SET SESSION tidesdb_ttl = 300;                          -- five minutes
INSERT INTO events (id, data) VALUES (1, 'temporary');  -- expires in 300s
SET SESSION tidesdb_ttl = 0;                            -- back to the table default
```

There is no syntax here for scoping a variable to a single statement, so a one-statement TTL is
written by setting the variable and putting it back:

```sql
SET SESSION tidesdb_ttl = 60;
INSERT INTO events (id, data) VALUES (2, 'one-minute');
SET SESSION tidesdb_ttl = 0;
```

## Resolution order

For each written row the lifetime is resolved in this order, taking the first that is set:

1. The per-row TTL column value, when non-zero.
2. The session `tidesdb_ttl`, when non-zero.
3. The table-level `ttl` option, when non-zero.
4. Otherwise the row never expires.

## TTL across an UPDATE

An `UPDATE` rewrites the row, so it also resolves the lifetime again, by the same order. Where
a source applies the countdown restarts from the update: a table with `ttl=3600` keeps each row
for an hour after its last write rather than after its insert, and a TTL column is read from
the new row the same way it was read from the old one. A TTL column set to zero still means
this row does not expire, so that is how an expiry is cleared.

One case has no source to resolve at all: the table carries no `ttl` option and no TTL column,
so the only thing that ever set this row's expiry was a session holding `tidesdb_ttl` at insert
time — and the session running the `UPDATE` need not have it set. Rather than write the row back
as permanent, the engine reads the stored deadline and carries the remaining lifetime forward,
so the row still expires when it was going to:

```sql
CREATE TABLE sessions (id INT PRIMARY KEY, token VARCHAR(64)) ENGINE=TIDESDB;

SET SESSION tidesdb_ttl = 3600;
INSERT INTO sessions VALUES (1, 'abc');   -- expires an hour from now
SET SESSION tidesdb_ttl = 0;

UPDATE sessions SET token = 'def' WHERE id = 1;  -- still expires at the original deadline
```

On a table shaped like this, an `UPDATE` cannot remove an expiry by omission. To change one,
give the updating session a `tidesdb_ttl`, which puts the write back on step 2.

Carrying the deadline forward costs a table that never uses TTL nothing. A row an `UPDATE`
reaches through a scan has its expiry read off the iterator as the scan passes it, with no
extra lookup, and only in a statement that writes the table, so a `SELECT` never reads it at
all. A row reached by an exact primary-key match, as in the example above, is fetched by point
lookup instead, and its expiry is looked up once more when it is written back.
