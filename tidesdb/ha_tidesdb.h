/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/* the server-portability layer.  it also supplies the server prelude -- the base headers that must
   precede handler.h and my_base.h where the server publishes them at all -- so it has to stay
   ahead of those two includes.  keep it first in this block; a formatter pass that sorts it after
   handler.h reintroduces the missing-typedef build break it exists to prevent. */
#include "ha_tidesdb_compat.h"

#include "handler.h"
#include "my_base.h"
#include "thr_lock.h"

/* TABLE, TABLE_SHARE, KEY and the Field hierarchy.  handler.h only forward-declares them on some
   servers, so the row, key and DDL paths that reach into a TABLE need the definitions named here
   rather than relying on a transitive include that is not guaranteed. */
#include "field.h"
#include "item.h"
#include "log.h"
#include "table.h"

extern "C"
{
#include <tidesdb/db.h>
}

/* The TideSQL plugin's own version, reported through tidesdb_version / _version_hex and the
   plugin declaration block.  Distinct from the vendored library version (TIDESDB_VERSION). */
/* The engine name as SHOW ENGINES, ENGINE=, and information_schema report it.  One
   definition so the plugin descriptor and handler::table_type() cannot disagree. */
#define TIDESQL_ENGINE_NAME "TidesDB"

#define TIDESQL_VERSION_STR "2.0.0"

/* the whole version as one number, major-minor-patch a byte each, for the tidesdb_version_hex
   status variable a caller can compare numerically. */
#define TIDESQL_VERSION_HEX 0x020000

/* the version the plugin descriptor carries.  the server renders this field as "%d.%d" of its
   high and low bytes, so it holds the major and minor only; giving it the three-byte form above
   would have it reported as 512.0 rather than 2.0. */
#define TIDESQL_PLUGIN_VERSION 0x0200

/* shared compile-time constants (key/row format, spatial, fts, cost model, library defaults) */
#include "ha_tidesdb_constants.h"
#include "src/core/table_options.h"

/*
  One foreign-key constraint as this engine persists and enforces it.  The same
  definition is loaded on both sides of the relationship, so a table keeps a list
  of the constraints where it is the child (referencing) and a separate list of
  the constraints where it is the parent (referenced).  The field-index vectors
  are in the constraint's declared order, and the resolved column-family names let
  the enforcement path probe the parent and scan the children without reopening
  the server table definitions.
*/
struct tdb_fk_def
{
    std::string name;                          /* constraint name                        */
    std::string child_cf;                      /* referencing table's data cf            */
    std::string child_db;                      /* referencing table's database           */
    std::string child_table;                   /* referencing table's name               */
    std::vector<uint16> child_fields;          /* referencing column field indexes       */
    std::vector<uint8> child_nullable;         /* whether each child column is nullable   */
    std::string ref_db;                        /* referenced database, as written        */
    std::string ref_table;                     /* referenced table, as written           */
    std::string parent_cf;                     /* referenced table's data cf             */
    std::vector<uint16> parent_fields;         /* referenced column field indexes        */
    std::vector<std::string> ref_column_names; /* referenced column names, for display */
    std::string child_index_name;              /* index on the child fk columns          */
    std::string parent_index_name;             /* pk or unique index probed on the parent */
    std::vector<uint8> parent_nullable;        /* whether each parent key column is nullable */
    uint8 on_delete;                           /* enum_fk_option                         */
    uint8 on_update;                           /* enum_fk_option                         */
    /* Key numbers resolved at load time from the table this side owns, so the
       enforcement path encodes comparable keys without a per-check name lookup.
       child_key_no is set on the child side, parent_key_no on the parent side,
       either stays -1 when this side does not own that end. */
    int child_key_no{-1};
    int parent_key_no{-1};
    bool parent_is_pk{false}; /* referenced columns are the parent pk    */
};

/*
  TidesDB_share -- shared state for one table, visible to all handler objects.
*/
class TidesDB_share : public Handler_share
{
   public:
    /* Main data CF */
    tidesdb_column_family_t *cf;
    std::string cf_name;

    /* Primary key info */
    bool has_user_pk;
    uint pk_index; /* server key number of the PK (usually 0)   */
    uint pk_key_len;

    /* Hidden PK row-id generator (used when has_user_pk == false) */
    std::atomic<uint64_t> next_row_id;

    /* In-memory AUTO_INCREMENT counter (avoids index_last() per INSERT).
       Seeded once from index_last() at open time; incremented atomically. */
    std::atomic<ulonglong> auto_inc_val{0};

    /* Per-table isolation level (from CREATE TABLE options) */
    tidesdb_isolation_level_t isolation_level;

    /* TTL support */
    ulonglong default_ttl; /* table-level default TTL in seconds (0 = none) */
    int ttl_field_idx;     /* field index of TTL_COL column (-1 = none)     */

    /* Data-at-rest encryption */
    bool encrypted;
    uint encryption_key_id;      /* ENCRYPTION_KEY_ID table option (default 1) */
    uint encryption_key_version; /* cached latest key version */

    /* The table's storage options as they were resolved at open, so a re-open of the same table
       (a TRUNCATE rebuilding its column families, an ALTER updating them) works from the settings
       the table actually has rather than resolving the defaults a second time. */
    ha_table_option_struct options;

    /* Cached table shape flags (set once at open time) */
    bool has_blobs;
    bool has_ttl;
    uint num_secondary_indexes; /* count of non-NULL secondary index CFs */
    size_t cached_row_est{0};   /* cached serialize_row size estimate for non-BLOB tables */

    /* Foreign keys loaded from the engine's own catalog at open time.  fk_child
       holds the constraints where this table references another, driving the
       parent-existence checks on insert and update.  fk_parent holds the
       constraints where another table references this one, driving the
       restrict, cascade, and set-null actions on delete and update.  Both stay
       empty for a table with no foreign keys, so the enforcement path costs
       nothing there. */
    std::vector<tdb_fk_def> fk_child;
    std::vector<tdb_fk_def> fk_parent;
    bool fk_loaded{false};

    /* Field indices of BLOB/TEXT columns -- populated at open() when
       has_blobs is true.  serialize_row iterates only these instead of
       scanning all fields for the BLOB_FLAG. */
    std::vector<uint16> blob_field_indices;

    /* Per-field plan for the serialize/deserialize hot path.
       Built once at open() so the row loops avoid per-row recomputation
       of `f->ptr - table->record[0]` and skip the Field::pack/unpack
       vtable dispatch for fields whose pack() is the default memcpy.

       memcpy_ok is true when the field's pack format is exactly
       `pack_length()` bytes of memcpy (the Field::pack default, used by
       all integer, FLOAT/DOUBLE, fixed DATETIME/DATE/TIME/TIMESTAMP,
       YEAR, ENUM, SET, BIT and NEWDECIMAL types).  CHAR/VARCHAR/BLOB/
       VARBINARY/GEOMETRY/JSON keep the slow path because their pack()
       trims trailing spaces or emits a length prefix.

       maybe_null is cached so the loop branches off a single bool
       instead of calling Field::real_maybe_null() per row.

       src_off is the field's offset within table->record[0] -- the loops
       still rebase by ptrdiff at runtime so the same plan serves reads
       and writes that target record[1] too. */
    struct field_plan_t
    {
        uint32 src_off;  /* offset within table->record[0]                */
        uint16 pack_len; /* f->pack_length(), used when memcpy_ok         */
        bool memcpy_ok;  /* true -> inline memcpy; false -> Field::pack   */
        bool is_blob;    /* blob-family, so the read path may need to point
                            the field at the row buffer rather than unpack  */
        bool maybe_null; /* cached f->maybe_null() (NOT real_maybe_null)  */
    };
    std::vector<field_plan_t> field_plan;
    bool has_no_nullable{false};
    uint8 null_bytes_cached{0}; /* cached table->s->null_bytes            */
    uint16 fields_cached{0};    /* cached table->s->fields                */

    /* Cached scan_time range cost (refreshed every TIDESDB_STATS_REFRESH_US) */
    std::atomic<double> cached_scan_cost{0.0};
    std::atomic<long long> scan_cost_time{0};

    /* Table timestamps for information_schema.TABLES */
    time_t create_time{0};              /* from .frm stat at first open */
    std::atomic<time_t> update_time{0}; /* bumped on DML (write/update/delete) */

    /* Cached stats -- avoid expensive tidesdb_get_stats per statement.
       Refreshed at most every 2 seconds; read with relaxed atomics. */
    std::atomic<ha_rows> cached_records{0};
    std::atomic<uint64_t> cached_data_size{0};     /* total_data_size from CF */
    std::atomic<uint64_t> cached_idx_data_size{0}; /* sum of secondary CF sizes */
    std::atomic<uint32_t> cached_mean_rec_len{0};  /* avg_key_size + avg_value_size */
    std::atomic<long long> stats_refresh_us{0};
    std::atomic<double> cached_read_amp{1.0}; /* read amplification factor */

    /* Precomputed comparable key length per index (avoids per-row recomputation) */
    uint idx_comp_key_len[MAX_KEY];

    /* Precomputed index-type flags (avoid ki->algorithm dereference per row
       in DML secondary-index loops).  Populated at open() and refreshed
       during online DDL. */
    bool idx_is_fts[MAX_KEY];
    bool idx_is_spatial[MAX_KEY];

    /* Cached rec_per_key for secondary indexes (populated by ANALYZE TABLE
       or the open-time auto-sample below).
       0 = not yet computed, use heuristic; >0 = sampled value. */
    std::atomic<ulong> cached_rec_per_key[MAX_KEY];

    /* Auto-sample bookkeeping.  sampled flips true once a populated table has
       had its secondary-index cardinality measured for real, so we never fall
       back to records/10 for a high-cardinality index; sampling guards the
       one scan against a thundering herd of concurrent first queries. */
    std::atomic<bool> idx_stats_sampled{false};
    std::atomic<bool> idx_stats_sampling{false};

    /* Secondary index CFs (one per secondary key) */
    std::vector<tidesdb_column_family_t *> idx_cfs;
    std::vector<std::string> idx_cf_names;

    /* Per-index covered-field map used by try_keyread_from_index.  For each
       index i, idx_cover[i][field_c] == true when field `c` can be
       reconstructed from the index key bytes (i.e. field is in the index's
       key parts or -- for secondary indexes -- in the PK parts appended
       to the key).  Replaces the O(read_set_bits * (pk_parts + idx_parts))
       nested scan the old code did on every covered read. */
    std::vector<std::vector<bool>> idx_cover;

    TidesDB_share();
    ~TidesDB_share();
};

/*
  Context passed between Online DDL phases (prepare -> inplace -> commit).
  Holds the new/dropped CF pointers so commit can finalize atomically.
*/
class ha_tidesdb_inplace_ctx : public inplace_alter_handler_ctx
{
   public:
    /* CFs created for newly added indexes (populated during inplace phase) */
    std::vector<tidesdb_column_family_t *> add_cfs;
    std::vector<std::string> add_cf_names;
    std::vector<uint> add_key_nums; /* position in new key_info */

    /* CF names to drop for removed indexes (dropped during commit phase) */
    std::vector<std::string> drop_cf_names;

    virtual ~ha_tidesdb_inplace_ctx()
    {
    }
};

/* Per-txn accumulator entry for one FTS index's metadata key.  The
   plugin folds the per-row delta_docs and delta_words contributions
   from every write_row / update_row / delete_row in a transaction here
   and writes one combined update at commit time, so the FTS meta key
   does not become a write-write serialisation point under concurrent
   writers and a long statement does not produce N read-modify-writes
   on the same key. */
struct fts_meta_delta_t
{
    tidesdb_column_family_t *data_cf;
    uint keynr;
    int64_t doc_delta;
    int64_t word_delta;
};

/*
  Per-connection TidesDB transaction context.
  Stored via thd_set_ha_data(); shared by all handler objects on the
  same connection.  The TidesDB txn spans the entire BEGIN...COMMIT
  block (or a single auto-commit statement).
*/
struct tidesdb_trx_t
{
    tidesdb_txn_t *txn{nullptr};
    bool dirty{false};                 /* true once any DML uses txn */
    bool stmt_savepoint_active{false}; /* true while a "stmt" savepoint exists */
    bool needs_reset{false};           /* true after commit/rollback; cleared after txn_reset */
    tidesdb_isolation_level_t isolation_level{TDB_ISOLATION_REPEATABLE_READ};
    uint64_t txn_generation{0};

    /* Per-statement FTS meta deltas, applied before tidesdb_commit hands
       the txn to the library so the meta update lands in the same commit
       as the row writes that produced it. */
    std::vector<fts_meta_delta_t> fts_meta_pending;
    bool fts_meta_dirty{false};

    /* What a DDL statement wrote to the engine's schema-change log, as (record key, encoded
       record), read back by the post-DDL hook to see which of them survived the statement's
       commit.
       It lives here, on state the engine allocates and frees itself, rather than in a
       thread_local.  A thread_local with a destructor is implemented on Windows as a callback
       the DLL registers with fiber-local storage, and the server unloads the plugin before the
       process exits -- so the runtime walks that list at ExitProcess and calls into an address
       that is no longer mapped.  mysqld then dies on the way out, after every test has already
       passed, which reads as the engine crashing rather than as a teardown ordering problem. */
    std::vector<std::pair<std::string, std::string>> ddl_pending;

    /* Statement-atomicity bookkeeping.  A "stmt" library savepoint is armed at
       statement start (external_lock) inside a multi-statement transaction so a
       statement error rolls back only its own effects, not the whole txn.  The
       library savepoint reverts the txn op array; this member reverts the
       plugin-side fts meta accumulator to the same statement boundary.
       stmt_fts_snapshot is a copy of the small fts_meta_pending vector taken at
       statement start and restored wholesale on rollback. */
    std::vector<fts_meta_delta_t> stmt_fts_snapshot;
    bool stmt_fts_dirty_snapshot{false};

    /* When this connection's transaction has been XA PREPAREd, the serialized XID it was registered
       in the prepared-transaction map under, so a same-connection XA COMMIT/ROLLBACK (resolved
       through the normal commit/rollback callbacks) can drop that registry entry.  Empty otherwise.
     */
    std::string prepared_xid;

    /* The tables this transaction has written to, as [database, table] pairs.  A transaction that
       is left prepared across a restart is in doubt until the user resolves it, and the server has
       to hold a metadata lock on everything it touched for that whole time, or a DDL statement
       could drop a table out from under a transaction that is still entitled to commit.  The
       server cannot know what those tables were, so the engine records them here and hands them
       back at recovery.  Recorded once per table per transaction, not once per row. */
    std::vector<std::pair<std::string, std::string>> mod_tables;

    /* The transaction generation mod_tables was last recorded for, so the list is emptied when a
       new transaction starts rather than at each of the many points one can end. */
    uint64_t mod_tables_generation{0};

    /* mod_tables as it stood at XA PREPARE.  A prepare may detach the transaction from the
       connection, so the list is moved aside here for the prepared-in-coordinator record to be
       written from. */
    std::vector<std::pair<std::string, std::string>> prepared_mod_tables;
};

/* TDB_TABLE_OPTIONS, the per-table CREATE TABLE option accessor, is defined by the compat layer:
   the field it reads was renamed on one server, and MySQL has no per-engine option syntax at all --
   there the same settings arrive as one JSON object in the table's ENGINE_ATTRIBUTE and are read
   back through src/core/table_options.

   ha_table_option_struct itself, the struct both routes fill, is declared in that server-free core
   so the column-family config builders and the reader can share it. */

/* per-column CREATE TABLE options.  as with the table options, the option list binding this to a
   field flag lives in the root translation unit; the struct is shared so open can find the column
   marked as the per-row TTL source. */
struct ha_field_option_struct
{
    bool ttl; /* marks this column as the per-row TTL source (seconds) */
};

/*
  Scope guard for a buffer handed back by tidesdb_iter_key, tidesdb_iter_value,
  or tidesdb_iter_key_value.  Each returns a newly allocated buffer the caller
  owns and must release with tidesdb_free.  The guard binds to the caller's
  pointer by reference and frees whatever it holds at scope exit, so a scan that
  reads a key or value per row does not have to thread a free onto every return
  path.  A NULL pointer, as left by a failed read, is left alone.
*/
struct tdb_owned_buf
{
    uint8_t *&p;
    explicit tdb_owned_buf(uint8_t *&ptr) : p(ptr)
    {
    }
    ~tdb_owned_buf()
    {
        if (p) tidesdb_free(p);
    }
    tdb_owned_buf(const tdb_owned_buf &) = delete;
    tdb_owned_buf &operator=(const tdb_owned_buf &) = delete;
};

/*
  ha_tidesdb -- per-connection handler object.
*/
class ha_tidesdb : public handler
{
    TidesDB_share *share;

    /* Points into the per-connection tidesdb_trx_t::txn.
       Set in external_lock(), cleared in external_lock(F_UNLCK). */
    tidesdb_txn_t *stmt_txn;
    bool stmt_txn_dirty; /* true once any DML uses stmt_txn */

    /* Scan / index-scan state (iterator lives on stmt_txn when available) */
    tidesdb_txn_t *scan_txn;
    tidesdb_iter_t *scan_iter;
    tidesdb_column_family_t *scan_cf_;      /* CF for lazy iterator creation */
    tidesdb_column_family_t *scan_iter_cf_; /* CF the cached scan_iter was created for */
    tidesdb_txn_t *scan_iter_txn_;          /* txn the cached scan_iter was created on */
    uint64_t scan_iter_txn_gen_;            /* txn_generation when scan_iter was created */
    /* When a range scan is set up via read_range_first these hold the encoded
       lower/upper key bounds, so ensure_scan_iter builds a range iterator that
       opens only the sstables overlapping the scan rather than every sstable in
       the CF.  scan_range_valid_ gates it; a full scan or point lookup leaves it
       false and gets an unbounded iterator. */
    bool scan_range_valid_;
    uint scan_range_lo_len_;
    uint scan_range_hi_len_;
    uchar scan_range_lo_[DATA_KEY_BUF_LEN];
    uchar scan_range_hi_[DATA_KEY_BUF_LEN];
    bool idx_pk_exact_done_; /* deferred seek after PK exact */
    enum scan_dir_t
    {
        DIR_NONE,
        DIR_FORWARD,
        DIR_BACKWARD
    } scan_dir_;
    std::string last_row;  /* keeps BLOB data alive for record[0] */
    std::string last_row2; /* keeps BLOB data alive for record[1] */

    /* Spatial index scan state */
    bool spatial_scan_active_{false};
    enum ha_rkey_function spatial_mode_
    {
        HA_READ_KEY_EXACT
    };
    double spatial_qmbr_[SPATIAL_MBR_DIMS]{}; /* query MBR (xmin, ymin, xmax, ymax) */

    /* Hilbert range decomposition are sorted non-overlapping [lo, hi] ranges
       covering the query box.  spatial_range_idx_ tracks which range we're
       currently scanning. */
    std::vector<std::pair<uint64_t, uint64_t>> spatial_ranges_; /* {lo, hi} */
    size_t spatial_range_idx_{0};

    /* Spatial scan continuation -- scans Hilbert range with MBR post-filter */
    int spatial_scan_next(uchar *buf);

    /* Current row's PK key bytes (without namespace prefix).
       Fixed buffer eliminates std::string heap allocation per row. */
    uchar current_pk_buf_[MAX_KEY_LENGTH];
    uint current_pk_len_;

    /* Reusable buffer for serialize_row (retains heap capacity) */
    std::string row_buf_;

    /* Cached comparable search key from index_read_map for index_next_same */
    uchar idx_search_comp_[MAX_KEY_LENGTH];
    uint idx_search_comp_len_;

    /* True when index_read_map landed on a partial-PK exact prefix scan and
       defers iteration to index_next.  index_next's PK branch must then
       re-validate the prefix after each step, the same way the secondary
       branch and index_next_same already do, or it would walk off the
       prefix and return unrelated rows. */
    bool pk_partial_exact_active_{false};

    /* Reusable buffers for secondary index key construction in update_row.
       Avoids heap allocation per row and keeps the stack frame small. */
    uchar upd_old_ik_[SEC_IDX_KEY_BUF_LEN];
    uchar upd_new_ik_[SEC_IDX_KEY_BUF_LEN];

    /* Reusable buffer for tidesdb_txn_get values -- avoids malloc/free per
       point-lookup.  Retains heap capacity across calls. */
    std::string get_val_buf_;

    /* Separate encryption output buffer so row_buf_ retains its heap
       capacity across rows.  serialize_row writes plaintext into row_buf_
       and the encrypted blob into enc_buf_. */
    std::string enc_buf_;

    /* Per-statement cached encryption key version -- avoids calling
       tdb_crypto_latest_key_version() on every single row write. */
    uint cached_enc_key_ver_;
    bool enc_key_ver_valid_;

    /* Per-statement cached time(NULL) -- avoids the vDSO/syscall on every
       row for TTL computation.  1-second granularity is sufficient for TTL. */
    time_t cached_time_;
    bool cached_time_valid_;

    /* Per-statement cached THDVAR lookups -- avoids the indirect
       thd_get_ha_data + offset computation on every row. */
    ulonglong cached_sess_ttl_;
    bool cached_skip_unique_;
    bool cached_single_delete_primary_;
    bool cached_thdvars_valid_;

    /* Cached "is this scan on the primary key" flag.  Set once in index_init
       so the navigation methods (index_next/prev/first/last/next_same) skip
       the per-row `share->has_user_pk && active_index == share->pk_index`
       recomputation. */
    bool is_pk_;

    /* Cached last tidesdb_iter_new failure for the current scan CF/txn.
       When non-zero and the scan_cf_/scan_txn haven't changed, ensure_scan_iter
       returns the prior error immediately instead of retrying + re-logging. */
    int scan_iter_last_err_;
    tidesdb_column_family_t *scan_iter_last_err_cf_;
    tidesdb_txn_t *scan_iter_last_err_txn_;

    /* Handler mirrors of share->has_blobs / share->encrypted.  Per-row
       fetches and scans branch on these; reading them from a handler member
       avoids the shared-memory dereference that dominates when the L1 line
       for `share` isn't already hot. */
    bool has_blobs_;
    bool encrypted_;

    /* Cached bounds of table->record[1] so the BLOB path of fetch_row_by_pk
       and iter_read_current can classify `buf` against record[0] vs record[1]
       without dereferencing `table->record[1]` and `table->s->reclength` on
       every row. */
    const uchar *record1_lo_;
    const uchar *record1_hi_;

    /* Cached per-statement THD query shape so ensure_stmt_txn() and
       external_lock() don't each re-evaluate thd_sql_command() and
       thd_test_options().  Populated by external_lock(F_WRLCK/F_RDLCK);
       invalidated by external_lock(F_UNLCK) along with the other per-stmt
       caches. */
    int cached_sql_cmd_;
    bool cached_is_autocommit_;
    bool cached_stmt_shape_valid_;

    /* Cached per-statement pointers to avoid repeated hash lookups.
       Set in external_lock(lock), cleared in external_lock(F_UNLCK).
       InnoDB caches these as m_user_thd / m_prebuilt->trx. */
    THD *cached_thd_;           /* avoids ha_thd() virtual dispatch */
    tidesdb_trx_t *cached_trx_; /* avoids thd_get_ha_data() hash lookup */

    /* Bulk DML state.  The ops counter is shared across insert/update/delete
       bulk modes since only one can be active at a time and they all use the
       same TIDESDB_BULK_INSERT_BATCH_OPS threshold. */
    bool in_bulk_insert_;
    bool in_bulk_update_;
    bool in_bulk_delete_;
    ha_rows bulk_insert_ops_; /* ops buffered since last mid-txn commit */

    /* Auto-compact-after-range-delete tracking.  When the session var
       tidesdb_compact_after_range_delete_min_rows is non-zero, delete_row
       updates these fields with the comparable PK bytes of each deleted
       row, and end_bulk_delete fires tidesdb_compact_range over the
       observed [min_pk, max_pk] range if the deleted-row count meets the
       threshold.  Cleared on start_bulk_delete and on each
       cached-THDVAR refresh. */
    ulonglong cached_compact_after_range_delete_min_rows_;
    ha_rows bulk_delete_rows_;
    std::string bulk_delete_min_pk_;
    std::string bulk_delete_max_pk_;

    /* Range-tombstone deferral for a bulk DELETE.  When a bulk delete is eligible (a user primary
       key and no delete triggers) delete_row buffers each primary-row data key here instead
       of writing a per-row tombstone, and end_bulk_delete coalesces them into one range tombstone
       once it has confirmed every live row in the touched key span was one this statement deleted.
       Cleared on start_bulk_delete; emptied when flushed as per-row tombstones or turned into a
       range tombstone. */
    bool bulk_delete_defer_{false};
    std::vector<std::string> bulk_delete_keys_;

    /* Write every buffered data key as a per-row tombstone, the fallback when a delete is not a
       clean range or outgrows the deferral cap.  Empties bulk_delete_keys_. */
    int bulk_delete_flush_buffered(tidesdb_txn_t *txn);

    /* Multi-Range Read state.  We accept MRR when every range the optimizer
       hands us is UNIQUE_RANGE|EQ_RANGE (i.e. the WHERE col IN (...) case on
       a full key) and fall back to the default MRR->read_range_first path for
       everything else.  Accepted ranges are buffered + sorted by comparable
       key bytes so the LSM sees a monotone stream of seeks. */
    struct tdb_mrr_entry
    {
        std::string comp_key; /* comparable PK / index bytes */
        TDB_MRR_RANGE_ID ptr; /* value returned to caller as *range_info */
    };
    bool mrr_custom_active_;
    bool mrr_no_assoc_;
    uint mrr_keyno_;
    std::vector<tdb_mrr_entry> mrr_entries_;
    size_t mrr_next_idx_;

    /* Covering-index mode (HA_EXTRA_KEYREAD) */
    bool keyread_only_;
    bool write_can_replace_; /* true during REPLACE INTO (HA_EXTRA_WRITE_CAN_REPLACE) */

    /* private helpers
     */
    int ensure_stmt_txn();     /* lazy txn creation on first data access */
    void cache_stmt_thdvars(); /* populate the per-statement session-var cache once */
    TidesDB_share *get_share();
    const std::string &serialize_row(const uchar *buf);
    /* upper-bound packed size of the row at buf, ptrdiff its offset from record[0], adding real
       blob lengths for a blob table on top of the cached constant estimate. */
    size_t serialize_estimate_size(const uchar *buf, my_ptrdiff_t ptrdiff);
    /* encrypt the plaintext already packed into row_buf_ into enc_buf_ and return it, refreshing
       the per-statement cached key version on first use. */
    const std::string &serialize_encrypt_row();
    void deserialize_row(uchar *buf, const uchar *data, size_t len);
    /* unpack unpack_count packed fields from [from, from_end) into buf using the field plan. */
    void deserialize_unpack_fields(uchar *buf, const uchar *from, const uchar *from_end,
                                   uint unpack_count);
    void deserialize_row(uchar *buf, const std::string &row);

    /* Build memcmp-comparable key bytes into out[]; returns byte count.
       fk_ref_nullable, when given, builds a key that probes a parent key rather than this table's
       own: an indicator byte is written for the parts it marks nullable and for no others, which is
       how the parent encoded them.  It is always SORT_KEY_NOT_NULL, since the caller checks the
       referencing columns are non-null before probing.  Null means encode for this table, taking
       each part's nullability from its own field. */
    uint make_comparable_key(KEY *key_info, const uchar *record, uint num_parts, uchar *out,
                             const std::vector<uint8> *fk_ref_nullable = nullptr);

    /* Convert key_copy-format search key directly to comparable bytes */
    uint key_copy_to_comparable(KEY *key_info, const uchar *key_buf, uint key_len, uchar *out);

    /* Build PK bytes from a record buffer into out[]; returns byte count */
    uint pk_from_record(const uchar *record, uchar *out);

    /* Build KEY_NS_DATA + pk into out[]; returns byte count */
    static uint build_data_key(const uchar *pk, uint pk_len, uchar *out)
    {
        out[0] = KEY_NS_DATA;
        memcpy(out + KEY_NAMESPACE_LEN, pk, pk_len);
        return pk_len + KEY_NAMESPACE_LEN;
    }

    /* Build a secondary-index entry key into out[]; returns byte count */
    uint sec_idx_key(uint idx, const uchar *record, uchar *out);

    /* Fetch a row by its PK bytes into buf; sets current_pk + last_row */
    int fetch_row_by_pk(tidesdb_txn_t *txn, const uchar *pk, uint pk_len, uchar *buf);

    /* Probe whether a PK already exists, WITHOUT entering the write txn's
       read-set.  tkey = cf_name + data-key bytes.  Returns 1 = exists,
       0 = absent, <0 = -(HA error). */
    int probe_pk_exists(tidesdb_trx_t *trx, const uchar *dk, uint dk_len);

    /* Compute the row's TTL as a duration in seconds from now, the way the
       library expects it.  Reads the per-row TTL_COL value if present, else the
       session or table default.  Returns -1 (no expiration) or a positive
       number of seconds the row should live. */
    time_t compute_row_ttl(const uchar *buf);

    /* Read current iterator entry (data-CF), decode row into buf.
       Returns 0 or HA_ERR_END_OF_FILE / HA_ERR_KEY_NOT_FOUND. */
    int iter_read_current(uchar *buf);

    /* Lazily create scan_iter from scan_cf_ when first needed */
    int ensure_scan_iter();

    /* Position and read for a primary-key index_read_map, comp_key holds the
       comparable-format search key of comp_len bytes.  Handles the full-PK
       point lookup and every iterator-based PK seek mode. */
    int index_read_pk(uchar *buf, const uchar *comp_key, uint comp_len,
                      enum ha_rkey_function find_flag);

    /* Position and read for a spatial MBR index_read_map, key is the raw
       server search key.  Decomposes the query box into hilbert ranges and
       hands off to spatial_scan_next. */
    int index_read_spatial(uchar *buf, const uchar *key, enum ha_rkey_function find_flag);

    /* Seek scan_iter to the starting entry for a secondary-index read, comp_key
       holds the comparable-format index prefix of comp_len bytes. */
    void index_seek_secondary(const uchar *comp_key, uint comp_len,
                              enum ha_rkey_function find_flag);

    /* Read the first matching secondary-index entry from the positioned cursor,
       applying index-condition pushdown before the PK point-lookup. */
    int index_read_secondary(uchar *buf, const uchar *comp_key, uint comp_len,
                             enum ha_rkey_function find_flag);

    /* Encode a records_in_range key range into comparable lo_buf/hi_buf bytes,
       substituting the natural key-space boundary for a missing bound.  inx is
       the index, is_pk true when it is the clustered primary key. */
    void rir_encode_bounds(uint inx, bool is_pk, const key_range *min_key, const key_range *max_key,
                           uchar *lo_buf, uint &lo_len, uchar *hi_buf, uint &hi_len);

    /* Try to decode record from secondary index key (keyread-only) */
    bool try_keyread_from_index(const uint8_t *ik, size_t iks, uint idx, uchar *buf);
    /* Decode the read-set columns of one key's parts from a comparable key at pos into buf, setting
       null bits at ptrdiff and un-inverting descending parts.  Advances pos; returns false if a
       part runs past end or is undecodable.  Shared by the index-part and pk-suffix passes above.
     */
    bool keyread_decode_key_parts(KEY *key, const uint8_t *&pos, const uint8_t *end, uchar *buf,
                                  my_ptrdiff_t ptrdiff);

    /* Evaluate pushed index condition on a secondary-index entry before
       the expensive PK point-lookup.  Decodes the index key columns into
       buf and calls handler_index_cond_check().
       Returns CHECK_POS                -- condition satisfied, proceed with PK lookup
               CHECK_NEG                -- condition not satisfied, skip this entry
               CHECK_OUT_OF_RANGE       -- past end of scan range
               a killed query is reported where the server models that state */
    tdb_icp_result_t icp_check_secondary(const uint8_t *ik, size_t iks, uint idx, uchar *buf);
    /* decode one key's parts from a comparable-format sort key into buf, advancing pos; returns
       false as soon as a part runs past end or holds a type decode_sort_key_part cannot reverse. */
    bool icp_decode_key_parts(KEY *key, const uint8_t *&pos, const uint8_t *end, uchar *buf,
                              my_ptrdiff_t ptrdiff);

    /* Extended sort-key decoder -- handles integers, DATE, DATETIME,
       TIMESTAMP, YEAR, and fixed-length CHAR/BINARY.  Returns true on
       success, false for unsupported types.  Used by covering index
       reads and ICP evaluation to avoid PK point-lookups. */
    static bool decode_sort_key_part(const uint8_t *src, uint sort_len, Field *f, uchar *buf);

    /* Commit the current txn mid-statement when a bulk op crosses the batch
       threshold, then reset it to READ_COMMITTED for the next batch.  Shared
       between bulk INSERT/UPDATE/DELETE.  Invalidates cached iterators.
       Returns 0 on success, handler error code on fatal failure. */
    int maybe_bulk_commit(tidesdb_trx_t *trx);

    /* Recover hidden-PK counter by scanning the CF */
    void recover_counters();

    /* When the auto-increment column is not the leftmost primary-key part, seed its counter from
       the last entry of the index whose first part is that column (that entry holds the maximum),
       so a restart does not restart the counter low and hand out colliding ids.  No-op otherwise.
     */
    void recover_auto_inc_secondary();

    /* Persist the CREATE/ALTER ... AUTO_INCREMENT=N start value (next_value) as a meta key in the
       given column family so an empty table's counter still begins at N after a restart, when there
       are no rows to recover it from. */
    void write_auto_inc_meta(tidesdb_column_family_t *cf, ulonglong next_value);

    /* Read the persisted AUTO_INCREMENT=N start value under the txn and raise the counter to it
       when it exceeds the value already recovered from the rows.  No-op when the table has no
       auto-inc column or no meta key. */
    void apply_auto_inc_start_meta(tidesdb_txn_t *txn);

    /* First-open share initialization, split from open() so each phase stays small.  These run once
       per share under lock_shared_ha_data(), populating the shared per-table metadata the hot paths
       read.  name is the server table path. */

    /* resolve the data column family and cache primary-key geometry, isolation, TTL, encryption,
       and the blob/ttl field indices; returns 0 or a handler error when the CF or encryption key
       is unavailable (the caller unlocks and returns the code). */
    /* dd_table_def is the server's own definition of the table, passed opaquely because only
       the servers that have one name the type; it is where a per-column option lives there. */
    int open_init_share_columns(const char *name);

    /* build the per-field serialize/deserialize plan (offset, pack length, memcpy-fast eligibility)
       cached on the share for the row hot loops. */
    void open_build_field_plan();

    /* build the per-index metadata: comparable key lengths, index-type flags, coverage bitmaps,
       resolved secondary column families, and the full-cost cache, then recover counters. */
    void open_build_index_meta(const char *name);

    /* info() helpers, split so each stays small.  refresh recomputes the cached row-count, data
       size, and mean record length from the column-family stats behind a time-gated CAS; fill
       populates every index's rec_per_key selectivity estimate for the current row count. */
    void info_refresh_cf_stats();
    void info_fill_rec_per_key();

    /* analyze() helpers.  report pushes the column-family, per-level, b-tree, and write-amp figures
       as ANALYZE notes; sample walks each secondary index to estimate its rec_per_key cardinality
       and caches it. */
    void analyze_report_cf_stats(THD *thd, const tidesdb_cf_stats_t &st);
    void analyze_sample_indexes(THD *thd, tidesdb_txn_t *txn, bool verbose);

    /* Measure secondary-index cardinality on its own short-lived read txn the
       first time a populated table is asked for const stats, so a workload that
       never runs ANALYZE still plans against real selectivity instead of the
       records/10 fallback.  Runs at most once per share per server lifetime. */
    void auto_sample_index_stats();

   public:
    ha_tidesdb(handlerton *hton, TABLE_SHARE *table_arg);
    ~ha_tidesdb() override = default;

    /* The engine name the server reports for a table.  Some servers derive it from the
       handlerton and leave this optional; MySQL requires every handler to answer. */
    const char *table_type() const override
    {
        return TIDESQL_ENGINE_NAME;
    }

    ulonglong table_flags() const override
    {
        /* the capability bits every target server defines, plus TDB_TABLE_FLAGS_SERVER_EXTRA for
           the group that only some do.  the compat layer lists what falls in that group and why. */
        return HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE | HA_NULL_IN_KEY |
               HA_PRIMARY_KEY_IN_READ_INDEX | HA_TABLE_SCAN_ON_INDEX | HA_FAST_KEY_READ |
               HA_CAN_SQL_HANDLER | HA_REQUIRES_KEY_COLUMNS_FOR_DELETE |
               HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT |
               HA_CAN_GEOMETRY | HA_CAN_RTREEKEYS | TDB_TABLE_FLAGS_SERVER_EXTRA;
        /* HA_CAN_EXPORT is deliberately absent.  It promises that holding the table still leaves a
           set of files on disk that can be copied out and read back as that table, and no such set
           exists here: the store is one flat directory with a database-wide manifest and value log,
           an sstable carries whichever families were flushed together, and the rows still in the
           shared memtable are in none of them.  Claiming it would let FLUSH TABLES ... FOR EXPORT
           succeed and hand back an incomplete copy without saying so.  Refused, the statement
           raises ER_ILLEGAL_HA and tidesdb_backup_dir is the operation that does work. */
    }

    ulong index_flags(uint idx, uint part, bool all_parts) const override;

    TDB_INDEX_TYPE_OVERRIDE

    uint max_supported_record_length() const override
    {
        return HA_MAX_REC_LENGTH;
    }
    uint max_supported_keys() const override
    {
        return MAX_TIDESDB_KEYS;
    }
    uint max_supported_key_parts() const override
    {
        return MAX_REF_PARTS;
    }
    uint max_supported_key_length() const override
    {
        return MAX_KEY_LENGTH;
    }

    /* The per-column index limit, declared through the compat layer because the two servers spell
       the accessor differently.  Both default it to 255 bytes, which this engine has no reason to
       accept. */
    TDB_MAX_KEY_PART_LENGTH_OVERRIDE

    /* Optimizer cost.  The server-facing methods differ in shape between servers, so the compat
       layer supplies the override set and one of two adapter translation units defines them; the
       arithmetic they share lives in src/core/cost_model and the LSM overlap probe in
       scan_overlap() below, so neither adapter carries engine logic of its own. */
    /* Row sampling and the emptiness question, offered by some servers only; the compat layer
       declares them where they exist and src/handler/ha_tidesdb_sampling.cc defines them. */
    TDB_SAMPLING_METHOD_OVERRIDES
    TDB_IS_TABLE_EMPTY_OVERRIDE

    TDB_COST_METHOD_OVERRIDES

    /* Read amplification currently in force, the LSM level count a point lookup expects to touch.
       Falls back to the single-level default before a table's stats are first sampled. */
    double read_amp();

    /* Number of sorted runs a full scan would merge, cached on the share and refreshed on the
       statistics interval.  This is the part of a scan's cost that volume alone cannot see. */
    double scan_overlap();

    /* Convert a server table path to a TidesDB column family name */
    static std::string path_to_cf_name(const char *path);

    /* DDL */
    int open(const char *name, int mode, uint test_if_locked TDB_DD_OPEN_ARG) override;
    int close(void) override;
    int create(const char *name, TABLE *form,
               HA_CREATE_INFO *create_info TDB_DD_CREATE_ARG) override;
    int delete_table(const char *name TDB_DD_DELETE_ARG) override;
    int rename_table(const char *from, const char *to TDB_DD_RENAME_ARG) override;

    /* Foreign keys.  The server never checks a constraint itself, it only asks
       the engine to describe its constraints for SHOW CREATE, information_schema,
       and prelocking, and relies on the engine to enforce them in its row ops.
       These describe what the loaded catalog holds, the enforcement lives in the
       write, update, and delete paths. */
    TDB_FOREIGN_KEY_METHOD_OVERRIDES

    /* Full table scan */
    int rnd_init(bool scan) override;
    int rnd_end() override;
    int rnd_next(uchar *buf) override;
    int rnd_pos(uchar *buf, uchar *pos) override;
    void position(const uchar *record) override;

    /* Index scan */
    int index_init(uint idx, bool sorted) override;
    int index_end() override;
    /* Overridden to capture the scan's key range and drive a range-bounded
       iterator that prunes sstables outside the scan. */
    int read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range,
                         bool sorted) override;
    int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                       enum ha_rkey_function find_flag) override;
    int index_next(uchar *buf) override;
    int index_prev(uchar *buf) override;
    int index_first(uchar *buf) override;
    int index_last(uchar *buf) override;
    int index_next_same(uchar *buf, const uchar *key, uint keylen) override;

    /* DML */
    int write_row(TDB_ROW_IN *buf) override;
    int update_row(const uchar *old_data, TDB_ROW_IN *new_data) override;
    int delete_row(const uchar *buf) override;

    /* write_row helpers, one cohesive step each (defined in ha_tidesdb_dml.cc).  Each returns 0 /
       TDB_SUCCESS on success or an error code to surface, and none touches the caller's saved
       column map -- write_row owns the tmp_use_all_columns/restore pairing. */

    /* run the server auto-increment step for an INSERT and report through pk_auto_generated whether
       the value was engine-generated (and thus known unique).  returns 0 or a handler error. */
    int write_row_auto_increment(const uchar *buf, bool &pk_auto_generated);
    /* build the primary key bytes for a row into pk, using the user PK columns or a freshly minted
       hidden row id, and return the key length. */
    uint write_build_pk(const uchar *buf, uchar *pk);
    /* account weight buffered operations against the bulk-DML batch and, when the batch threshold
       is reached, commit it mid-statement.  returns 0 or the commit error to surface. */
    int bulk_flush_if_threshold(tidesdb_trx_t *trx, ha_rows weight);
    /* reject an INSERT whose primary key already exists, unless skip_pk_unique lets it overwrite.
       returns 0 to proceed or the handler error to surface. */
    int write_check_pk_unique(tidesdb_trx_t *trx, const uchar *dk, uint dk_len, const uchar *pk,
                              uint pk_len, bool skip_pk_unique);
    /* reject an INSERT that duplicates any UNIQUE secondary index value.  returns 0 to proceed or
       the handler error to surface. */
    int write_check_secondary_unique(const uchar *buf, tidesdb_txn_t *txn);

    /* Foreign-key internals, implemented in ha_tidesdb_fk.cc.  fk_persist_defs
       parses the foreign keys off the create clause and records them in the
       engine catalog.  fk_load reads the catalog into share->fk_child and
       share->fk_parent at open.  fk_purge_catalog removes a table's rows when it
       is dropped.  The three enforce helpers run the referential checks in the
       row ops and return 0 to proceed or a handler error to surface, and each is
       a cheap early return when the relevant list is empty or foreign_key_checks
       is off. */
    int fk_persist_defs(const char *path, TABLE *table_arg, HA_CREATE_INFO *create_info);
    /* Re-key and rewrite every catalog record naming the table being renamed, on both the child
       and the parent side, so the constraints follow the table to its new name. */
    static int fk_rename_catalog(const char *from, const char *to);
    void fk_load();
    static int fk_purge_catalog(const char *child_cf_name);
    int fk_check_child(const uchar *new_row);
    int fk_enforce_parent_delete(const uchar *old_row);
    int fk_enforce_parent_update(const uchar *old_row, const uchar *new_row);
    /* Apply the cascade or set-null action for one constraint by driving the
       referencing child rows through the prelocked child table's own handler, so
       the children's secondary indexes and their own nested foreign keys stay
       correct.  new_row is the parent's new image for an update cascade and NULL
       for a delete.  Returns 0 or a handler error to surface. */
    int fk_collect_child_refs(TABLE *ct, int cidx, uint nparts, const uchar *keybuf, uint key_len,
                              std::vector<std::string> &refs);
    int fk_apply_cascade(const tdb_fk_def &d, TABLE *ct, KEY *ckey, uint nparts,
                         const uchar *new_row, bool is_update, bool set_null,
                         std::vector<std::string> &refs);
    int fk_cascade_children(const tdb_fk_def &d, const uchar *old_row, const uchar *new_row);
    /* Set on a child handler while a parent cascade drives its rows, so the
       child's own parent-existence check is skipped for the value the cascade is
       writing, which the cascade already knows to be valid. */
    bool fk_in_cascade_{false};
    /* Does one constraint have a referencing child row for the key in old_row?
       Returns 1 referenced, 0 none, or a negative handler error. */
    int fk_child_ref_exists(const tdb_fk_def &d, const uchar *old_row);
    /* write every secondary index entry (regular, fts, spatial) for a freshly inserted row. returns
       TDB_SUCCESS or the first library error. */
    int write_maintain_indexes(const uchar *buf, tidesdb_txn_t *txn, tidesdb_trx_t *trx,
                               const uchar *pk, uint pk_len, time_t row_ttl);
    /* delete every secondary index entry (regular, fts, spatial) for a row being removed.  returns
       TDB_SUCCESS or the first library error from a regular-index delete. */
    int delete_maintain_indexes(const uchar *buf, tidesdb_txn_t *txn, tidesdb_trx_t *trx);

    /* update_row helpers, one cohesive step each (defined in ha_tidesdb_dml_update.cc).  The index
       maintainers return TDB_SUCCESS on a no-op skip or success, or a library error to propagate;
       update_check_unique returns 0 or the HA_ERR_* to surface. */
    int update_check_unique(const uchar *old_data, const uchar *new_data, const uchar *old_pk,
                            uint old_pk_len, const uchar *new_pk, uint new_pk_len, bool pk_changed);
    int update_fts_index(uint i, const uchar *old_data, const uchar *new_data, const uchar *old_pk,
                         uint old_pk_len, const uchar *new_pk, uint new_pk_len, bool pk_changed,
                         time_t row_ttl);
    /* apply the term-level fts diff for an UPDATE that kept the same pk, deleting terms that
       vanished and rewriting terms whose frequency or the document length changed. */
    int update_fts_index_diff(uint i, const std::unordered_map<std::string, uint16> &old_tf,
                              const std::unordered_map<std::string, uint16> &new_tf,
                              const uchar *old_pk, uint old_pk_len, const uchar *new_pk,
                              uint new_pk_len, uint32 new_wc, bool doc_len_changed, time_t row_ttl);
    int update_spatial_index(uint i, const uchar *old_data, const uchar *new_data,
                             const uchar *old_pk, uint old_pk_len, const uchar *new_pk,
                             uint new_pk_len, time_t row_ttl);
    int update_regular_index(uint i, const uchar *old_data, const uchar *new_data,
                             const uchar *old_pk, uint old_pk_len, const uchar *new_pk,
                             uint new_pk_len, bool pk_changed, time_t row_ttl);
    /* rewrite the primary row for an UPDATE, deleting the old data key first when the pk changed
       and writing the new serialized row.  returns TDB_SUCCESS or the library error. */
    int update_rewrite_primary(const uchar *old_pk, uint old_pk_len, const uchar *new_pk,
                               uint new_pk_len, const uint8_t *row_ptr, size_t row_len,
                               time_t row_ttl, bool pk_changed);
    /* walk every secondary index for an UPDATE, dispatching each to its per-type maintainer.
       returns TDB_SUCCESS or the first library error. */
    int update_maintain_indexes(const uchar *old_data, const uchar *new_data, const uchar *old_pk,
                                uint old_pk_len, const uchar *new_pk, uint new_pk_len,
                                bool pk_changed, time_t row_ttl);
    int delete_all_rows(void) override;

    /* TRUNCATE TABLE.  Some servers supply a default that empties the table and restarts the
       auto-increment counter; MySQL reports the operation unsupported unless the engine implements
       it, so the engine states the same behaviour explicitly and both servers get it. */
    int truncate(TDB_TRUNCATE_ARG) override;

    /* Full-text search */
    int ft_init() override;
    TDB_FT_END_OVERRIDE
    FT_INFO *ft_init_ext(uint flags, uint inx, String *key) override;
    int ft_read(uchar *buf) override;

    /* Bulk insert hint (LOAD DATA, multi-row INSERT) */
    void start_bulk_insert(TDB_BULK_INSERT_ARGS) override;
    int end_bulk_insert() override;

    /* Bulk UPDATE / DELETE hints -- let multi-row UPDATE/DELETE share the
       same mid-txn commit batching as bulk INSERT so long statements don't
       balloon the memory the txn buffers before commit. */
    bool start_bulk_update() override;
    /* Apply whatever bulk_update_row buffered.  This engine buffers nothing -- the library's
       transaction is the buffer and every row is applied as it arrives -- so there is never
       anything outstanding and no duplicate to report.  It still has to be implemented: the base
       method reports the operation as unsupported, which fails the statement outright. */
    int exec_bulk_update(TDB_DUPKEY_COUNT *dup_key_found) override;
    TDB_END_BULK_UPDATE_RET end_bulk_update() override;
    int bulk_update_row(const uchar *old_data, TDB_ROW_IN *new_data,
                        TDB_DUPKEY_COUNT *dup_key_found) override;
    bool start_bulk_delete() override;
    int end_bulk_delete() override;

    /* Index Condition Pushdown (ICP) */
    Item *idx_cond_push(uint keyno, Item *idx_cond) override;

    /* Multi-Range Read (MRR).  We opt into a custom implementation for
       point-only range sequences and defer to the base handler for
       everything else by leaving HA_MRR_USE_DEFAULT_IMPL set. */
    ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param,
                                        uint n_ranges, uint *bufsz, uint *mrr_mode,
                                        TDB_MRR_INFO_CONST_TAIL, Cost_estimate *cost) override;
    int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint mrr_mode,
                              HANDLER_BUFFER *buf) override;
    int multi_range_read_next(TDB_MRR_RANGE_ID *range_info) override;

    /* AUTO_INCREMENT -- O(1) atomic counter */
    void get_auto_increment(ulonglong offset, ulonglong increment, ulonglong nb_desired_values,
                            ulonglong *first_value, ulonglong *nb_reserved_values) override;

    /* Reset the in-memory auto-increment counter so `TRUNCATE TABLE t` and
       `ALTER TABLE t AUTO_INCREMENT=N` take effect.  Base default is a no-op,
       which left TidesDB's cached counter running past TRUNCATE -- the next
       INSERT would return a stale value instead of restarting at 1 (or N). */
    TDB_RESET_AUTO_INCREMENT_OVERRIDE

    /* Stats / Maintenance */
    int info(uint flag) override;
    int analyze(THD *thd, HA_CHECK_OPT *check_opt) override;
    int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;
    int check(THD *thd, HA_CHECK_OPT *check_opt) override;
    int repair(THD *thd, HA_CHECK_OPT *check_opt) override;
    ha_rows records_in_range(uint inx, TDB_KEY_RANGE *min_key,
                             TDB_KEY_RANGE *max_key TDB_RECORDS_IN_RANGE_TAIL) override;
    int extra(enum ha_extra_function operation) override;

    /* Which index a duplicate-key error was raised on, or MAX_KEY when none has been.  The server
       members that carry this are read back differently on different servers -- one keeps what the
       write recorded, the other clears it and asks info(HA_STATUS_ERRKEY) instead -- so the engine
       keeps its own copy and answers both from it.  Without it the server cannot name the index and
       reports a duplicate as the generic "duplicate key in table" rather than naming the entry. */
    uint dup_key_no_{MAX_KEY};

    /* Record the index a duplicate was found on, for both the write path and a later info(). */
    void note_dup_key(uint key_no);

    /* The transaction generation this table was last recorded as modified under, so the write paths
       pay one integer comparison per row rather than a list search. */
    uint64_t mod_table_noted_generation_{UINT64_MAX};

    /* Record this table among the ones the transaction has written to, once per transaction. */
    void note_modified_table(tidesdb_trx_t *trx);

    /* Locking -- TidesDB handles concurrency via MVCC internally.
       lock_count()=0 bypasses the server's THR_LOCK. */
    uint lock_count(void) const override
    {
        return 0;
    }
    int external_lock(THD *thd, int lock_type) override;

    /* Under LOCK TABLES the server locks the table once and then calls start_stmt at the beginning
       of every statement, in place of external_lock.  The per-statement setup lives in one place
       and both entry points reach it. */
    int start_stmt(THD *thd, thr_lock_type lock_type) override;

    /* Report the auto-increment counter so SHOW CREATE TABLE and a dump carry it. */
    void update_create_info(HA_CREATE_INFO *create_info) override;
    /* statement-start half of external_lock: resolve and cache the per-statement shape, join or
       create the connection transaction, and arm the statement savepoint.  returns 0 or an error.
     */
    int external_lock_acquire(THD *thd);
    /* statement-end half of external_lock: free the scan iterator when the writeset moved, stamp
       the update time, and invalidate the per-statement caches. */
    void external_lock_release(THD *thd);
    THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override;

    /* Online DDL -- instant metadata, inplace indexes, copy for columns */
    enum_alter_inplace_result
    check_if_supported_inplace_alter(TABLE *altered_table,
                                     Alter_inplace_info *ha_alter_info) override;
    bool prepare_inplace_alter_table(TABLE *altered_table,
                                     Alter_inplace_info *ha_alter_info TDB_DD_ALTER_ARG) override;
    bool inplace_alter_table(TABLE *altered_table,
                             Alter_inplace_info *ha_alter_info TDB_DD_ALTER_ARG) override;
    /* build every newly-added secondary-index entry for one base row during an inplace ADD INDEX.
       returns 0 on success, 1 on a UNIQUE duplicate, or 2 on a library put failure; the offending
       index number is reported through fail_key_num for the caller's error message. */
    int inplace_add_row_entries(ha_tidesdb_inplace_ctx *ctx, TABLE *altered_table,
                                const uint8_t *key_data, size_t key_size, const uint8_t *val_data,
                                size_t val_size, tidesdb_txn_t *txn,
                                std::vector<std::unordered_set<std::string>> &idx_seen,
                                const std::vector<bool> &idx_is_unique, uint &fail_key_num);
    /* build the comparable sort key for one index of the row currently in table->record[0] during
       an inplace ADD INDEX; ptdiff rebases the altered-table field pointers, row_has_null reports a
       NULL part (which exempts a UNIQUE index).  returns the key length written to ik. */
    uint inplace_build_index_key(KEY *ki, my_ptrdiff_t ptdiff, uchar *ik, bool &row_has_null);
    /* commit the current index-build batch and reopen the scan cursor positioned just past the last
       processed data key.  txn and iter are updated in place.  returns 0 to continue the scan, 1 to
       abort the ALTER, or 2 to end the scan gracefully. */
    int inplace_batch_commit_reseek(tidesdb_txn_t *&txn, tidesdb_iter_t *&iter,
                                    const uchar *last_data_key, size_t last_data_key_len);
    /* scan the base table and populate every newly added secondary index, committing in batches.
       txn and iter are the already-opened build cursor.  returns true when the ALTER must abort
       (the helper has freed the cursor, restored old_map, and raised the error), false on success.
     */
    bool inplace_scan_and_build(ha_tidesdb_inplace_ctx *ctx, TABLE *altered_table,
                                tidesdb_txn_t *&txn, tidesdb_iter_t *&iter,
                                TDB_COLUMN_MAP_SAVED old_map);
    /* release an aborting index build's cursor and transaction and restore the column map; the
       caller raises the specific error first.  always returns true for `return
       inplace_abort_build`. */
    bool inplace_abort_build(tidesdb_iter_t *iter, tidesdb_txn_t *txn, TABLE *altered_table,
                             TDB_COLUMN_MAP_SAVED old_map);
    /* one flag per newly added index marking whether it is UNIQUE, so the build can reject a
       duplicate index-column prefix during population. */
    std::vector<bool> inplace_build_unique_flags(ha_tidesdb_inplace_ctx *ctx, TABLE *altered_table);
    /* rebuild the shared per-index metadata (resolved column families, key lengths, type flags, and
       coverage bitmaps) for the altered table's new key layout after an inplace ALTER commits. */
    void commit_rebuild_index_meta(TABLE *altered_table);
    /* push changed table options (compression, sync mode, bloom, isolation, ttl, encryption) onto
       the live data and index column families so an ALTER..OPTIONS takes effect without a reopen.
     */
    void commit_apply_runtime_config(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    bool commit_inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info,
                                    bool commit TDB_DD_ALTER_ARG) override;
    bool check_if_incompatible_data(HA_CREATE_INFO *create_info, uint table_changes) override;
};
