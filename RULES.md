# Code Rules

1. **Simple control flow.** No `goto` or `setjmp`/`longjmp`. Recursion is allowed only with a statically bounded depth, such as the WKB geometry parser bounded by geometry nesting; prefer an explicit stack where practical.
2. **Bounded loops.** Every loop terminates on a well-defined condition. Loops over a fixed domain carry a statically verifiable upper bound; a loop that consumes a server-driven cursor or a TidesDB iterator terminates on its documented end-of-stream. Wait and retry loops carry an explicit maximum and a defined outcome when they reach it.
3. **Allocate deliberately.** Every allocation's failure is handled. Hot-path buffers are pre-sized once and reused across rows, so steady-state operation performs no per-row heap growth, and nothing allocates without bound. Standard-library containers and server-owned objects such as `String`, `std::string`, and `std::vector` are permitted where their lifetime is bounded and their capacity is reused.
4. **Smallest possible scope.** Declare data objects at the tightest scope that works.
5. **Check every return value.** Validate all function parameters; never ignore a non-void return. A result deliberately discarded is cast to `(void)` with the reason beside it.
6. **Minimal preprocessor use.** Macros limited to file inclusion and simple constants — no token pasting. Conditional compilation is confined to `ha_tidesdb_compat.h`, where a MySQL release difference is resolved at the macro that depends on it; ordinary engine code never hides logic behind `#ifdef`. Where a whole capability is absent, the translation unit is dropped from the build in CMakeLists.txt rather than wrapped.
7. **Restricted pointer use.** At most one level of dereference in ordinary logic. Function pointers appear only where the server API mandates them, namely the handler vtable, the full-text `_ft_vft`, system-variable update callbacks, and handlerton hooks; the engine defines no gratuitous function pointers of its own.
8. **Zero-warning compilation.** All warnings enabled, all warnings fixed, and the code passes static analysis clean before release.
9. **No magic numbers or strings.** Every literal with meaning gets a named constant or macro instead of a bare number or string appearing inline.
10. **Functions should be unit and integration testable.** A system module is unit and integration tested in the style of what is under `tidesdb/test`, and its SQL-visible behaviour is covered by the `tidesdb` or `tidesdb_rpl` mtr suite.
11. Comments are primarily **lowercase**.
12. Before committing code be sure to test it thoroughly locally and prove it, if on linux with ASAN, UBSAN and TSAN, all possible flags on your running platform.
13. Attempt to keep source and header files under **1000** lines of code. Test files are not held to this. A small number of files are graced from this and are listed below; a graced file still has a hard ceiling, and nothing else may exceed 1000 lines without being added to the list.

    | File | Ceiling | Why |
    | --- | --- | --- |
    | `src/handler/ha_tidesdb_txn.cc` | 1600 | One connection's transaction lifecycle, from start through savepoints, two-phase commit and recovery. The prepared-transaction registry and the recovery cursor are file-private state the ordinary commit path and the XA resolvers both reach; splitting the file would make them `extern` and turn private state into a cross-unit interface. |
    | `ha_tidesdb.h` | 1400 | One include gives a translation unit the handler, the share and the foreign-key definition. Splitting it would trade that for a header set every consumer has to assemble. |
    | `src/handler/ha_tidesdb_fk.cc` | 1200 | Foreign-key enforcement is one concern: the catalog it persists, the parent probes and the child actions share the same encoding and key-shape helpers. |
    | `src/handler/ha_tidesdb_dml.cc` | 1200 | The row write path, including the bulk-commit threshold its write and delete halves share. |

14. Functions should be attempted to be no greater than **100** lines.

## Documentation Style

Comments should explain *why* and *what for*, not restate the code. Skip comments that
just repeat a variable or type name. Every public struct and function gets a doc comment
in this format:

### Structs

```c
/**
 * flush_ctx_t
 * the shared, read-only context a flush runs against; the engine builds one and the flush pool reuses
 * it across immutables
 * @param l0 the L0 subsystem, for reclaiming an immutable once its data is durable in L1
 * @param cfs the column family registry indexed by cf-index; a NULL slot is a dropped family whose
 *            entries are discarded
 * @param n_cfs the length of cfs
 * @param manifest the db-level manifest every output sstable is recorded in
 * @param manifest_path the path the manifest commits to
 * @param next_sstable_id the db-global sstable id allocator, fetch-added per output
 * @param fdm the db-global descriptor budget, for releasing a flushed immutable's WAL descriptor, or
 *            NULL when the immutables carry no WAL
 * @param sync_mode the block-manager sync mode driving the klog and manifest durability barriers
 */
```

### Functions

```c
/**
 * flush_immutable
 * flush one dequeued immutable to L1 -- demux its skip_list into per-column-family sstables, record
 * them in one atomic manifest commit, install them into the level sets, then mark the immutable flushed
 * and reclaim it. on any failure before the commit the built sstables are closed and the immutable is
 * left for a retry, its data still durable in its WAL
 * @param fx the flush context
 * @param immutable the dequeued immutable memtable, owned by this call on success
 * @return TDB_SUCCESS, TDB_ERR_INVALID_ARGS, TDB_ERR_IO on a klog or manifest failure, TDB_ERR_MEMORY,
 *         or TDB_ERR_CORRUPTION on a malformed skip_list key
 */
```

**Conventions:**

- First line: the identifier name.
- Second line: one-sentence purpose, lowercase, no trailing period.
- `@param` / `@name`-style fields: one line per parameter or struct field, stating type constraints and nullability where relevant.
- `@return`: what each outcome means, not just "returns int."
- No inline comments duplicating the doc comment's information inside the function body.
