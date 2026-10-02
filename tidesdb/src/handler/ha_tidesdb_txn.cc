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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "ha_tidesdb.h"

#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/plugin.h>
#include <mysql/service_plugin_registry.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "key.h"
#include "sql_class.h"
#include "src/engine/ha_tidesdb_config.h"
#include "src/handler/ha_tidesdb_fts.h"
#include "src/handler/ha_tidesdb_internal.h"
#include "src/handler/ha_tidesdb_txn.h"

/**
 * tdb_binlog_enabled
 * whether the server has the binary log on
 * @return true when @@GLOBAL.log_bin reads ON
 *
 * Read through the sysvar service rather than the mysqld global, which the server does not export
 * to plugins.  Called once, from handlerton init, which the server runs before it picks the
 * transaction coordinator.
 */
static bool tdb_binlog_enabled()
{
    SERVICE_TYPE(registry) *reg = mysql_plugin_registry_acquire();
    if (!reg) return false;

    bool on = false;
    my_h_service svc = nullptr;
    if (!reg->acquire("mysql_system_variable_reader", &svc) && svc)
    {
        auto *reader = reinterpret_cast<SERVICE_TYPE(mysql_system_variable_reader) *>(svc);
        char buf[8];
        char *value = buf;
        size_t value_len = sizeof(buf) - 1;
        if (!reader->get(nullptr, "GLOBAL", "mysql_server", "log_bin",
                         reinterpret_cast<void **>(&value), &value_len) &&
            value != nullptr && value_len >= 2)
            on = (value[0] == 'O' || value[0] == 'o') && (value[1] == 'N' || value[1] == 'n');
        reg->release(svc);
    }
    mysql_plugin_registry_release(reg);
    return on;
}

/* Prepared transactions awaiting their phase-two decision, keyed by the serialized XID.  An
   external XA PREPARE hands its library transaction here and detaches it from the connection, and
   crash recovery repopulates it from tidesdb_recover_prepared at startup, so commit_by_xid /
   rollback_by_xid can resolve a transaction from any connection or after a restart.  The map owns
   each transaction until it is resolved (then freed) -- guarded by tdb_prepared_mtx. */
static std::mutex tdb_prepared_mtx;
static std::map<std::string, tidesdb_txn_t *> tdb_prepared_txns;

/* Recovery enumeration state, loaded once from the library on the first handlerton recover() call.
   The server drains the in-doubt XIDs in batches, so we keep a cursor over the loaded set. */
static std::vector<std::string> tdb_recovery_xids;
static size_t tdb_recovery_pos = 0;
static bool tdb_recovery_loaded = false;

/* The tables each recovered in-doubt transaction wrote to, keyed by the serialized XID, read back
   from the prepared-in-coordinator records.  The server locks these while the transaction is in
   doubt, so a DDL statement cannot drop a table a prepared transaction is still entitled to
   commit into -- guarded by tdb_prepared_mtx. */
static std::map<std::string, std::vector<std::pair<std::string, std::string>>>
    tdb_recovery_mod_tables;

/* the serialised form of an XID: [format id (8 BE)][gtrid length (4 BE)][bqual length (4 BE)][the
   two identifiers, back to back].  every field is written explicitly rather than the struct being
   copied wholesale, because recovery has to hand the server back an XID equal to the one the user
   named -- a form that drops a field, or one that depends on the struct's layout, produces a key
   the coordinator will not match and a prepared transaction the user can no longer resolve. */
static constexpr size_t TDB_XID_HDR_LEN = 16;

static void tdb_put_u64_be(uint64_t v, char *out)
{
    for (int i = 0; i < 8; i++) out[i] = (char)((v >> ((7 - i) * 8)) & 0xFFu);
}

static void tdb_put_u32_be(uint32_t v, char *out)
{
    for (int i = 0; i < 4; i++) out[i] = (char)((v >> ((3 - i) * 8)) & 0xFFu);
}

static uint64_t tdb_get_u64_be(const uint8_t *in)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)in[i];
    return v;
}

static uint32_t tdb_get_u32_be(const uint8_t *in)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)in[i];
    return v;
}

static std::string tdb_xid_key(const XID *xid)
{
    const long gl = TDB_XID_GTRID_LEN(xid);
    const long bl = TDB_XID_BQUAL_LEN(xid);
    const size_t n = (gl > 0 ? (size_t)gl : 0) + (bl > 0 ? (size_t)bl : 0);

    std::string key(TDB_XID_HDR_LEN + n, '\0');
    tdb_put_u64_be((uint64_t)(int64_t)TDB_XID_FORMAT_ID(xid), &key[0]);
    tdb_put_u32_be((uint32_t)gl, &key[8]);
    tdb_put_u32_be((uint32_t)bl, &key[12]);
    if (n > 0) memcpy(&key[TDB_XID_HDR_LEN], TDB_XID_DATA(xid), n);
    return key;
}

/**
 * tdb_xid_from_key
 * rebuild the XID a stored key was written from
 * @param key the serialised bytes
 * @param key_len their length, checked
 * @param out out -- the XID, untouched unless the key is well formed
 * @return true when the key parses; a key that does not is left alone rather than turned into a
 *         plausible-looking XID naming somebody else's transaction
 */
static bool tdb_xid_from_key(const uint8_t *key, size_t key_len, XID *out)
{
    if (!key || !out || key_len < TDB_XID_HDR_LEN) return false;

    const long fmt = (long)(int64_t)tdb_get_u64_be(key);
    const uint32_t gl = tdb_get_u32_be(key + 8);
    const uint32_t bl = tdb_get_u32_be(key + 12);

    if (gl > XIDDATASIZE || bl > XIDDATASIZE || (size_t)gl + (size_t)bl > XIDDATASIZE) return false;
    if (key_len != TDB_XID_HDR_LEN + (size_t)gl + (size_t)bl) return false;

    const char *data = (const char *)key + TDB_XID_HDR_LEN;
    *out = XID();
    out->set(fmt, data, (long)gl, data + gl, (long)bl);
    return true;
}

/* Drop the prepared-in-coordinator marker for a resolved XID.  Defined with the rest of that state
   further down; declared here because the by-XID resolvers above it are what clear it. */
static void tdb_xa_tc_clear(const std::string &key);

/* Commit a transaction honoring its 2PC state: a prepared transaction takes the phase-two
   commit-prepared path, an active one the normal blocking commit. */
static int tdb_txn_commit_stateful(THD *thd, tidesdb_txn_t *txn)
{
    tidesdb_txn_state_t st = TDB_TXN_STATE_ACTIVE;
    tidesdb_txn_state(txn, &st);
    if (st == TDB_TXN_STATE_PREPARED) return tidesdb_txn_commit_prepared(txn);
    return tdb_txn_commit_blocking(thd, txn);
}

/* Roll back a transaction honoring its 2PC state: a prepared transaction takes the phase-two
   rollback-prepared path, an active one the normal rollback. */
static void tdb_txn_rollback_stateful(tidesdb_txn_t *txn)
{
    tidesdb_txn_state_t st = TDB_TXN_STATE_ACTIVE;
    tidesdb_txn_state(txn, &st);
    if (st == TDB_TXN_STATE_PREPARED)
        tidesdb_txn_rollback_prepared(txn);
    else
        tidesdb_txn_rollback(txn);
}

/**
 * ha_tidesdb::note_modified_table
 * record this table among the ones the transaction has written to
 * @param trx the connection's transaction, or NULL outside one
 *
 * Called from the write paths, so it runs once per row and must cost almost nothing when there is
 * nothing to do.  The transaction generation is what makes that possible: it changes whenever the
 * connection starts a new transaction, so a table already recorded under the current generation is
 * recognised by one integer comparison.
 */
void ha_tidesdb::note_modified_table(tidesdb_trx_t *trx)
{
    if (!trx || mod_table_noted_generation_ == trx->txn_generation) return;
    mod_table_noted_generation_ = trx->txn_generation;

    /* The list belongs to whichever transaction is current; a new one starts it empty.  Doing that
       here rather than everywhere a transaction ends keeps it to one place. */
    if (trx->mod_tables_generation != trx->txn_generation)
    {
        trx->mod_tables.clear();
        trx->mod_tables_generation = trx->txn_generation;
    }

    if (!table || !table->s) return;
    std::pair<std::string, std::string> name(table->s->db.str ? table->s->db.str : "",
                                             table->s->table_name.str ? table->s->table_name.str
                                                                      : "");
    if (name.first.empty() || name.second.empty()) return;

    /* A transaction touches few tables, so a linear scan beats the bookkeeping a set would need. */
    for (const auto &have : trx->mod_tables)
        if (have == name) return;
    trx->mod_tables.push_back(std::move(name));
}

/* Drop a connection's prepared-transaction registry entry when its own commit/rollback resolves the
   XA transaction, so a later commit_by_xid cannot touch the same (reused or freed) transaction. */
static void tdb_drop_prepared_entry(tidesdb_trx_t *trx)
{
    if (trx->prepared_xid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        tdb_prepared_txns.erase(trx->prepared_xid);
    }
    trx->prepared_xid.clear();
}
/* Reserved name of the per-statement savepoint.  The SQL SAVEPOINT callbacks
   synthesize "sv_%p" names, so this never collides with a user savepoint. */
static constexpr const char TIDESDB_STMT_SAVEPOINT[] = "stmt";

/* Arm a statement savepoint at statement start.  Caller guarantees we are in a
   multi-statement transaction (not autocommit/DDL), trx->txn exists, and no
   statement savepoint is currently armed.  On any failure we leave the savepoint
   unarmed so a later statement rollback safely falls back to full rollback. */
static void stmt_savepoint_arm(tidesdb_trx_t *trx)
{
    /* Defensive: a prior statement's savepoint should already be gone (released
       on success, removed on rollback), but a user ROLLBACK TO SAVEPOINT can
       collaterally drop it; releasing again is a cheap no-op if absent. */
    (void)tidesdb_txn_release_savepoint(trx->txn, TIDESDB_STMT_SAVEPOINT);
    if (tidesdb_txn_savepoint(trx->txn, TIDESDB_STMT_SAVEPOINT) != TDB_SUCCESS) return;

    trx->stmt_savepoint_active = true;
    trx->stmt_fts_snapshot = trx->fts_meta_pending;
    trx->stmt_fts_dirty_snapshot = trx->fts_meta_dirty;
}

/* Statement completed successfully -- the statement's writes are now permanent
   within the (still uncommitted) txn.  Drop the savepoint and per-statement undo
   state so the next statement re-arms cleanly. */
static void stmt_savepoint_disarm(tidesdb_trx_t *trx)
{
    if (!trx->stmt_savepoint_active) return;
    (void)tidesdb_txn_release_savepoint(trx->txn, TIDESDB_STMT_SAVEPOINT);
    trx->stmt_savepoint_active = false;
    trx->stmt_fts_snapshot.clear();
}

/* Roll back just the current statement's effects to the armed savepoint.
   Returns true if the partial rollback was performed, false if the caller must
   fall back to a full transaction rollback (no savepoint armed, or the library
   savepoint is gone because a bulk mid-commit reset the txn or a user ROLLBACK
   TO SAVEPOINT removed it). */
static bool stmt_savepoint_rollback(tidesdb_trx_t *trx)
{
    if (!trx->stmt_savepoint_active) return false;

    int rc = tidesdb_txn_rollback_to_savepoint(trx->txn, TIDESDB_STMT_SAVEPOINT);
    if (rc != TDB_SUCCESS)
    {
        /* Savepoint vanished under us -- cannot do a partial rollback. */
        trx->stmt_savepoint_active = false;
        trx->stmt_fts_snapshot.clear();
        return false;
    }

    trx->fts_meta_pending = trx->stmt_fts_snapshot;
    trx->fts_meta_dirty = trx->stmt_fts_dirty_snapshot;
    trx->stmt_fts_snapshot.clear();

    /* The library freed the txn ops appended after the savepoint, so any cached
       scan iterator over this txn on every handler on this connection now has a
       stale view.  Bumping the generation makes each handler rebuild lazily,
       exactly as a bulk mid-commit does. */
    trx->txn_generation++;
    trx->stmt_savepoint_active = false;
    return true;
}

/**
 * tdb_stmt_read_txn
 * the transaction a scan should read under while this statement commits in pieces
 * @param trx the connection's transaction
 * @return the snapshot-backed read transaction, or NULL when the statement is not one that
 *         commits in pieces or the snapshot could not be taken
 *
 * Opened on the first scan of such a statement and held until it ends.  Taking the snapshot here,
 * rather than at the first mid-statement commit, is what keeps it clear of the statement's own
 * writes: by the time a commit happens the statement has already written rows, and a snapshot
 * taken then would show them to the scan still reading.
 */
tidesdb_txn_t *tdb_stmt_read_txn(tidesdb_trx_t *trx)
{
    if (!trx || !trx->stmt_piecewise) return NULL;
    if (trx->stmt_read_txn) return trx->stmt_read_txn;

    if (tidesdb_snapshot_create(tdb_global, &trx->stmt_read_snapshot) != TDB_SUCCESS)
    {
        trx->stmt_read_snapshot = NULL;
        return NULL;
    }
    if (tidesdb_txn_begin_at_snapshot(tdb_global, trx->stmt_read_snapshot, &trx->stmt_read_txn) !=
        TDB_SUCCESS)
    {
        tidesdb_snapshot_release(trx->stmt_read_snapshot);
        trx->stmt_read_snapshot = NULL;
        trx->stmt_read_txn = NULL;
        return NULL;
    }
    return trx->stmt_read_txn;
}

/**
 * tdb_stmt_read_txn_release
 * drop the statement's snapshot read transaction once the statement is over
 * @param trx the connection's transaction
 *
 * Safe to call with iterators still open under it: freeing a transaction detaches them, and a
 * detached iterator answers nothing but its own free, which touches none of the transaction.  The
 * snapshot is released after the transaction that reads at it, never before.
 */
void tdb_stmt_read_txn_release(tidesdb_trx_t *trx)
{
    if (!trx) return;
    if (trx->stmt_read_txn)
    {
        tidesdb_txn_rollback(trx->stmt_read_txn);
        tidesdb_txn_free(trx->stmt_read_txn);
        trx->stmt_read_txn = NULL;
    }
    if (trx->stmt_read_snapshot)
    {
        tidesdb_snapshot_release(trx->stmt_read_snapshot);
        trx->stmt_read_snapshot = NULL;
    }
    trx->stmt_piecewise = false;
}

/* ******************** Per-connection transaction helpers ******************** */

/*
  Get or create the per-connection TidesDB transaction context.
  The txn lives for the entire BEGIN...COMMIT block (or single auto-commit
  statement).  All handler objects on the same connection share it.
*/
static tidesdb_trx_t *get_or_create_trx(THD *thd, handlerton *hton, tidesdb_isolation_level_t iso)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, hton);
    if (trx)
    {
        if (!trx->txn)
        {
            int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
            if (rc != TDB_SUCCESS)
            {
                (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reuse)");
                return NULL;
            }
            trx->dirty = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        else if (trx->needs_reset)
        {
            /* Txn object kept alive from previous commit/rollback (see
               tidesdb_commit).  We reset it to get a fresh MVCC snapshot at
               current-transaction-start.  This avoids the expensive
               free+begin cycle while ensuring we see the latest data.
               The bulk-insert path already uses commit+reset successfully.
               Only reset when needs_reset is true (set after real commit/
               rollback) to preserve snapshot within multi-statement txns. */
            int rrc = tidesdb_txn_reset(trx->txn, iso);
            if (rrc != TDB_SUCCESS)
            {
                /* Reset failed -- we fall back to free + begin.  Surface the
                   failure so we can spot regressions in txn recycling instead
                   of silently degrading to per-statement free+begin. */
                sql_print_warning("[TIDESDB] tidesdb_txn_reset failed (rc=%d), falling back to "
                                  "free+begin -- expect higher per-statement overhead until "
                                  "this is investigated",
                                  rrc);
                tidesdb_txn_free(trx->txn);
                trx->txn = NULL;
                int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
                if (rc != TDB_SUCCESS)
                {
                    (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reset_fallback)");
                    return NULL;
                }
            }
            trx->needs_reset = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        return trx;
    }

    /* The trx struct owns a std::vector (fts_meta_pending), so it must be
       constructed and destroyed properly.  Switching from MY_ZEROFILL/my_free
       to new/delete runs the std::vector's ctor/dtor and gives every field
       its default value via the header's member initialisers. */
    trx = new tidesdb_trx_t{};
    if (!trx) return NULL;

    int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
    if (rc != TDB_SUCCESS)
    {
        delete trx;
        (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(new)");
        return NULL;
    }
    trx->isolation_level = iso;
    trx->txn_generation = 1;
    thd_set_ha_data(thd, hton, trx);
    return trx;
}

/* ******************** Handlerton transaction callbacks ******************** */

/* Maximum length of a TidesDB savepoint name, including the trailing NUL.
   Names are synthesized via TIDESDB_SAVEPOINT_NAME_FMT below; 32 bytes
   fits the decoded pointer plus prefix on all supported platforms. */
static constexpr uint TIDESDB_SAVEPOINT_NAME_MAX = 32;
/* Format used to synthesize a unique savepoint name for the TidesDB
   transaction layer.  The pointer to the SQL-layer savepoint slot is
   the only handle we have that survives across the set/rollback/release
   callbacks, so we encode it as the engine-level savepoint name. */
static constexpr const char TIDESDB_SAVEPOINT_NAME_FMT[] = "sv_%p";

struct tidesdb_savepoint_t
{
    char name[TIDESDB_SAVEPOINT_NAME_MAX];
};

static int tidesdb_savepoint_set(TDB_HTON_CB_ARG THD *thd, void *sv)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    return tdb_rc_to_ha(rc, "savepoint_set");
}

static int tidesdb_savepoint_rollback(TDB_HTON_CB_ARG THD *thd, void *sv)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_rollback_to_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS)
    {
        /* The TidesDB library may drop the savepoint as part of the rollback.
           SQL semantics require the savepoint to still exist after rollback,
           so we re-create it here to allow RELEASE SAVEPOINT to succeed. */
        (void)tidesdb_txn_savepoint(trx->txn, sp->name);
        return 0;
    }
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_rollback");
}

static bool tidesdb_savepoint_rollback_can_release_mdl(TDB_HTON_CB_ARG THD *)
{
    return true;
}

static int tidesdb_savepoint_release(TDB_HTON_CB_ARG THD *thd, void *sv)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_release_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_release");
}

/* Perform the durable final commit of a real (non statement-level) transaction, leaving the txn
   object alive and reset-pending for reuse. */
static int tdb_finalize_commit(THD *thd, tidesdb_trx_t *trx)
{
    /* No statement is in flight across a transaction boundary, so nothing may still be reading at
       the statement snapshot.  Released here too because the transaction can end without the
       statement-end path running, and the snapshot holds the reclamation floor until it does. */
    tdb_stmt_read_txn_release(trx);

    /* We must release any active statement savepoint before final commit/rollback.
       Savepoints must be explicitly released before txn_commit.  Disarm also
       clears the per-statement undo journal and fts snapshot; it leaves
       fts_meta_pending intact for the flush below. */
    stmt_savepoint_disarm(trx);

    /* This connection is resolving its own transaction, so no by-XID resolver should reach it. */
    tdb_drop_prepared_entry(trx);

    /* Real commit -- flush to storage.
       After a successful commit, we keep the txn object alive and let
       get_or_create_trx() call tidesdb_txn_reset() to get a fresh
       snapshot.  This avoids the expensive free+begin cycle on every
       autocommit statement (saves malloc/free + internal buffer
       reallocation).  The bulk-insert path already uses commit+reset
       successfully, so the pattern is proven safe.
       If commit fails, fall back to rollback+free. */
    if (trx->dirty)
    {
        /* Fold the per-txn FTS meta deltas into this same txn before it
           commits so the meta update is atomic with the row writes that
           produced it. */
        int frc = flush_trx_fts_meta_pending(thd, trx);
        if (frc != TDB_SUCCESS)
        {
            sql_print_error(
                "[TIDESDB] hton_commit: flush_trx_fts_meta_pending returned %d (gen=%lu)", frc,
                (unsigned long)trx->txn_generation);
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            trx->stmt_savepoint_active = false;
            return tdb_rc_to_ha(frc, "hton_commit fts_meta_flush");
        }

        int rc = tdb_txn_commit_stateful(thd, trx->txn);
        if (rc != TDB_SUCCESS)
        {

            /* Only log truly unexpected errors (not transient conflicts). */
            if (rc != TDB_ERR_CONFLICT && rc != TDB_ERR_LOCKED && rc != TDB_ERR_MEMORY_LIMIT)
                sql_print_error("[TIDESDB] hton_commit: tidesdb_txn_commit returned %d "
                                "(dirty=%d gen=%lu)",
                                rc, trx->dirty, (unsigned long)trx->txn_generation);
            tdb_txn_rollback_stateful(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            trx->stmt_savepoint_active = false;
            return tdb_rc_to_ha(rc, "hton_commit");
        }
        /* We keep txn alive for reuse via txn_reset on next use. */
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    else
    {
        /* Read-only transaction -- we rollback, keep alive for reuse. */
        trx->fts_meta_pending.clear();
        trx->fts_meta_dirty = false;
        tidesdb_txn_rollback(trx->txn);
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    return 0;
}

static int tidesdb_commit(TDB_HTON_CB_ARG THD *thd, bool all)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return 0;

    if (!trx->txn) return 0;

    /* We determine whether this is the final commit for the transaction.
       all=true         -> explicit COMMIT or transaction-level end
       all=false        -> statement-level; only a real commit when autocommit */
    bool is_real_commit = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_commit)
    {
        /* Statement-level commit inside a multi-statement transaction.
           Defer the actual commit -- writes stay buffered in the txn,
           avoiding an expensive txn_begin + commit per statement.  The
           statement succeeded, so its writes become permanent within the
           still-open txn; drop the statement savepoint armed in external_lock
           so the next statement re-arms at the new boundary.  (tidesdb_txn
           savepoints are O(1) -- they record op/cf counts, they do not copy
           the write-set -- so this per-statement arm/disarm is cheap.) */
        stmt_savepoint_disarm(trx);
        return 0;
    }

    return tdb_finalize_commit(thd, trx);
}

static int tidesdb_rollback(TDB_HTON_CB_ARG THD *thd, bool all)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return 0;

    if (!trx->txn) return 0;

    bool is_real_rollback = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_rollback)
    {
        /* Statement-level rollback inside a multi-statement transaction.
           Roll back only this statement's effects to the savepoint armed in
           external_lock, preserving the rest of the transaction and its
           snapshot.  stmt_savepoint_rollback reverts the library op array and
           the plugin-side fts meta snapshot together.  It returns false when a partial
           rollback is impossible -- no savepoint armed, or a bulk mid-commit or
           user ROLLBACK TO SAVEPOINT removed it -- and we fall through to a full
           transaction rollback in that case. */
        if (stmt_savepoint_rollback(trx))
        {
            /* Leave trx->dirty as-is: earlier statements' writes remain in the
               still-open txn, and its snapshot is deliberately preserved. */
            return 0;
        }
    }

    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, TIDESDB_STMT_SAVEPOINT);
        trx->stmt_savepoint_active = false;
    }

    /* This connection is resolving its own transaction, so no by-XID resolver should reach it. */
    tdb_drop_prepared_entry(trx);

    /* The accumulated FTS meta deltas track the rows being rolled back,
       so discard them along with the txn's other write state. */
    trx->fts_meta_pending.clear();
    trx->fts_meta_dirty = false;
    trx->stmt_fts_snapshot.clear();

    /* Full rollback -- we keep txn alive for reuse via reset on next use.  A transaction that was
       XA PREPAREd on this same connection takes the phase-two rollback path. */
    tdb_txn_rollback_stateful(trx->txn);
    trx->txn_generation++;
    trx->needs_reset = true;
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    return 0;
}

static int tidesdb_close_connection(TDB_HTON_CB_ARG THD *thd)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (trx)
    {
        if (!trx->prepared_xid.empty())
        {
            /* A transaction XA PREPAREd on this connection stays in-doubt when the connection goes
               away: the registry keeps owning it so a later XA COMMIT/ROLLBACK -- or recovery after
               a restart -- can still resolve it.  Do not roll it back or free it here. */
        }
        else if (trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
        }
        delete trx;
        thd_set_ha_data(thd, tidesdb_hton, NULL);
    }
    return 0;
}

/*
  START TRANSACTION WITH CONSISTENT SNAPSHOT callback.
  Eagerly creates a TidesDB transaction so the snapshot sequence number
  is captured now, not lazily at first data access.  Without this, rows
  committed by other connections between START TRANSACTION and the first
  SELECT would be visible.

  Uses the session's isolation level (SET TRANSACTION ISOLATION LEVEL)
  rather than hard-coding REPEATABLE_READ.  Falls back to RR if the
  session is at the default.
*/
static int tidesdb_start_consistent_snapshot(TDB_HTON_CB_ARG THD *thd)
{
    /* START TRANSACTION WITH CONSISTENT SNAPSHOT explicitly requests a
       point-in-time snapshot.  Always use at least SNAPSHOT isolation
       so the snapshot persists for the entire transaction, regardless of
       the session's default isolation level (e.g. READ_COMMITTED would
       refresh the snapshot on each read, violating CONSISTENT_SNAPSHOT
       semantics). */
    tidesdb_isolation_level_t iso = resolve_effective_isolation(thd, TDB_ISOLATION_REPEATABLE_READ);
    if (iso < TDB_ISOLATION_SNAPSHOT) iso = TDB_ISOLATION_SNAPSHOT;
    tidesdb_trx_t *trx = get_or_create_trx(thd, tidesdb_hton, iso);
    if (!trx) return 1;

    /* We register at both statement and transaction level so the server
       knows TidesDB is participating in this BEGIN block. */
    trans_register_ha(thd, false, tidesdb_hton, 0);
    trans_register_ha(thd, true, tidesdb_hton, 0);
    return 0;
}

tidesdb_txn_t *tdb_stmt_txn_for_ddl(THD *thd)
{
    if (!thd || !tdb_global) return nullptr;

    /* A DDL statement touches no rows, so nothing has opened a transaction for it yet.  Opening one
       here and registering it is what puts the engine in the statement's commit, which is the whole
       point: the DDL log record written through it lives or dies with the statement. */
    tidesdb_trx_t *trx = get_or_create_trx(
        thd, tidesdb_hton, resolve_effective_isolation(thd, TDB_ISOLATION_READ_COMMITTED));
    if (!trx || !trx->txn) return nullptr;

    trans_register_ha(thd, false, tidesdb_hton, 0);
    trans_register_ha(thd, true, tidesdb_hton, 0);

    /* The record is a write, so the transaction has to be treated as having one: a transaction left
       marked clean can be skipped at commit, and the record would never reach storage. */
    trx->dirty = true;
    return trx->txn;
}

/* ******************** Locking ******************** */

/*
  Lazy txn creation.  Gets the per-connection TidesDB txn (shared by
  all handler objects on this connection).  The txn spans the entire
  BEGIN...COMMIT block, not just one statement.
*/
int ha_tidesdb::ensure_stmt_txn()
{
    if (stmt_txn) return 0;

    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();

    /* Isolation resolution mirrors the external_lock path:
         DDL       -> READ_COMMITTED, which avoids unbounded read-set growth across a long scan
                      and costs nothing, because the server's metadata locks already keep two
                      schema changes off the same table.
         every DML -> the session's level, so the library validates what the statement wrote.
       Prefer the per-statement cache populated by external_lock; fall back to the live THD call
       only when external_lock hasn't run yet (e.g. some DDL callbacks). */
    int sql_cmd;
    if (cached_stmt_shape_valid_)
    {
        sql_cmd = cached_sql_cmd_;
    }
    else
    {
        sql_cmd = thd_sql_command(thd);
    }
    bool is_ddl =
        (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
         sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE || sql_cmd == SQLCOM_OPTIMIZE ||
         sql_cmd == SQLCOM_CREATE_TABLE || sql_cmd == SQLCOM_DROP_TABLE);
    /* DDL coordinates through the server's metadata locks rather than the library's conflict
       detection, and reads catalogue keys no statement competes for, so it takes the cheapest
       level.  Every DML statement validates its writes, whether or not it is the only statement in
       its transaction: a single statement is still a read-modify-write against whatever else is
       running, and the level below validation makes a losing write silently disappear -- the row
       reverts while the secondary-index entries the same statement wrote do not, leaving an index
       naming a row that is not there.  An autocommit statement is the shortest transaction there
       is, so the conflicts this admits are the ones that were always real. */
    tidesdb_isolation_level_t effective_iso;
    if (is_ddl)
        effective_iso = TDB_ISOLATION_READ_COMMITTED;
    else
        effective_iso = resolve_effective_isolation(thd, share ? share->isolation_level
                                                               : TDB_ISOLATION_SNAPSHOT);
    tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
    if (!trx) return HA_ERR_OUT_OF_MEM;

    stmt_txn = trx->txn;
    return 0;
}

int ha_tidesdb::external_lock_acquire(THD *thd)
{
    /* We resolve per-statement THD shape once and cache to ensure_stmt_txn
       reads the cache instead of re-calling thd_sql_command() and
       thd_test_options(). */
    int sql_cmd = thd_sql_command(thd);
    bool is_ddl =
        (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
         sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE || sql_cmd == SQLCOM_OPTIMIZE ||
         sql_cmd == SQLCOM_CREATE_TABLE || sql_cmd == SQLCOM_DROP_TABLE);
    bool is_autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    cached_sql_cmd_ = sql_cmd;
    cached_is_autocommit_ = is_autocommit;
    cached_stmt_shape_valid_ = true;

    /* Same rule as ensure_stmt_txn: only DDL skips write validation. */
    tidesdb_isolation_level_t effective_iso;
    if (is_ddl)
        effective_iso = TDB_ISOLATION_READ_COMMITTED;
    else
        effective_iso = resolve_effective_isolation(thd, share ? share->isolation_level
                                                               : TDB_ISOLATION_SNAPSHOT);
    tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
    if (!trx) return HA_ERR_OUT_OF_MEM;

    stmt_txn = trx->txn;
    stmt_txn_dirty = false;

    /* We cache THD and trx pointers for fast access in hot paths
       (index_read_map, update_row, delete_row, ensure_stmt_txn).
       Eliminates ha_thd() virtual dispatch and thd_get_ha_data()
       hash lookup on every row operation. */
    cached_thd_ = thd;
    cached_trx_ = trx;

    trans_register_ha(thd, false, ht, 0);

    if (!is_autocommit) trans_register_ha(thd, true, ht, 0);

    /* Arm a statement savepoint so a statement error inside BEGIN...COMMIT
       rolls back only this statement, not the whole transaction.  Only for
       real multi-statement transactions -- autocommit and DDL statements
       are their own transaction, where statement rollback is already a full
       rollback.  external_lock fires once per table per statement, so the
       !stmt_savepoint_active guard arms it exactly once, at the first table,
       before any of the statement's row writes. */
    if (!is_autocommit && !is_ddl && trx->txn && !trx->stmt_savepoint_active)
        stmt_savepoint_arm(trx);
    return 0;
}

/*
  Statement start for a table the server has already locked.

  LOCK TABLES takes the table lock once and holds it across many statements, so external_lock runs
  at LOCK TABLES and again at UNLOCK TABLES and not in between.  Everything a statement needs --
  its transaction, the cached thread and transaction pointers, the statement savepoint -- is set up
  per statement, so without this every statement under LOCK TABLES would run with no transaction
  and hand a null handle to the first row operation it attempted.
*/
int ha_tidesdb::start_stmt(THD *thd, thr_lock_type lock_type [[maybe_unused]])
{
    return external_lock_acquire(thd);
}

void ha_tidesdb::external_lock_release()
{
    /* For multi-statement transactions (BEGIN...COMMIT), the txn stays the
       same across statements.  Preserve the cached scan iterator across
       read-only statements so the next statement reuses it (avoids the
       O(sstables) merge-heap rebuild).  After a write statement it must be
       freed: an iterator snapshots the txn's writeset when created, so one
       built before this statement's puts/deletes would not see them.  For
       autocommit, always free. */
    /* The cached iterator does not outlive the statement that built it.  It used to be kept across
       a read-only statement of a multi-statement transaction, to save the next one the
       O(sstables) merge-heap rebuild, and that is no longer worth what it costs:

       - It is only sound where the transaction's snapshot is frozen at its start.  Read committed
         and below re-read the sequence watermark for each operation, so the next statement there
         is entitled to see what other connections committed since this one, and an iterator
         carries the merge heap it was built with, which hides exactly that.
       - The next statement's first act is to reset the connection's transaction, and a reset
         detaches every iterator still open under it.  Keeping one alive across that boundary
         leaves the engine walking a list of iterators this handler is no longer stepping, for a
         statement that is going to rebuild its scan anyway.

       Caching within a statement is untouched, which is where it pays: index_init/index_end cycle
       once per outer row in a nested-loop join and reuse the same iterator throughout. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
        scan_iter_read_txn_ = false;
    }

    /* The statement is over, so its snapshot read transaction and the snapshot under it go.  Done
       here rather than in end_bulk_insert/end_bulk_delete so a statement that fails before those
       run does not leave the snapshot holding the reclamation floor for the connection's life. */
    tdb_stmt_read_txn_release(cached_trx_);

    /* We bump update_time once per write-statement for information_schema.
       We use cached_time_ if available to avoid another time() syscall. */
    if (stmt_txn_dirty && share)
        share->update_time.store(cached_time_valid_ ? cached_time_ : time(0),
                                 std::memory_order_relaxed);

    /* We invalidate all per-statement caches so the next statement
       picks up any changes (key rotation, session variable changes,
       clock advance). */
    enc_key_ver_valid_ = false;
    cached_time_valid_ = false;
    cached_thdvars_valid_ = false;

    stmt_txn = NULL;
    stmt_txn_dirty = false;
    cached_thd_ = NULL;
    cached_trx_ = NULL;

    /* We invalidate statement shape cache last so the above checks still see it. */
    cached_stmt_shape_valid_ = false;
}

int ha_tidesdb::external_lock(THD *thd, int lock_type)
{
    DBUG_ENTER("ha_tidesdb::external_lock");

    if (lock_type != F_UNLCK) DBUG_RETURN(external_lock_acquire(thd));

    external_lock_release();
    DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_tidesdb::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
    /* With lock_count()=0 the server skips THR_LOCK entirely.  store_lock is still
       called but we do not push into the 'to' array (same pattern as InnoDB);
       TidesDB owns concurrency control through its own MVCC and commit-time
       conflict detection, so there is nothing to record here. */
    (void)thd;
    (void)lock_type;
    return to;
}

/* ******************** two-phase commit (XA) ******************** */

/* two-phase-commit phase one.  fold the pending fts meta into the transaction, then durably prepare
   its write batch under the connection's XID.  an external XA PREPARE additionally hands the
   prepared transaction to the global registry and detaches it from the connection, so a later
   XA COMMIT/ROLLBACK on any connection -- or after a restart -- can resolve it. */
static int tidesdb_prepare(TDB_HTON_CB_ARG THD *thd, bool all)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn) return 0;
    if (!all) return 0; /* statement-level prepare defers, like statement-level commit */

    stmt_savepoint_disarm(trx);

    bool is_external = (thd_sql_command(thd) == SQLCOM_XA_PREPARE);

    /* A read-only transaction driven by the internal 2PC coordinator has nothing durable to
       prepare; leave it for commit() to roll back.  An external XA PREPARE still records it so the
       later XA COMMIT/ROLLBACK can resolve the XID. */
    if (!trx->dirty && !is_external) return 0;

    if (trx->dirty)
    {
        int frc = flush_trx_fts_meta_pending(thd, trx);
        if (frc != TDB_SUCCESS)
        {
            tdb_txn_rollback_stateful(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            return tdb_rc_to_ha(frc, "hton_prepare fts_meta_flush");
        }
    }

    XID xid;
    thd_get_xid(thd, (MYSQL_XID *)&xid);
    std::string key = tdb_xid_key(&xid);

    int rc = tidesdb_txn_prepare(trx->txn, (const uint8_t *)key.data(), key.size());
    if (rc != TDB_SUCCESS)
    {
        tdb_txn_rollback_stateful(trx->txn);
        tidesdb_txn_free(trx->txn);
        trx->txn = NULL;
        trx->txn_generation++;
        trx->dirty = false;
        return tdb_rc_to_ha(rc, "hton_prepare");
    }

    if (is_external)
    {
        /* Publish the prepared transaction so it can also be resolved by XID -- from another
           connection, or after a restart via recover().  The transaction stays attached to this
           connection: a same-connection XA COMMIT/ROLLBACK runs through commit()/rollback(), which
           drop this registry entry, while a disconnect leaves it in-doubt for the registry to own.
         */
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        tdb_prepared_txns[key] = trx->txn;
        trx->prepared_xid = key;

        /* The prepared-in-coordinator record is written after this returns, by which point the
           transaction may no longer be attached to the connection, so the tables it modified are
           moved aside now. */
        trx->prepared_mod_tables = std::move(trx->mod_tables);
        trx->mod_tables.clear();

        if (TDB_XA_PREPARE_DETACHES)
        {
            /* The server will resolve this transaction by XID even from this connection, and the
               by-XID resolver frees it.  Let go of it here so the next statement on this connection
               commits a fresh transaction rather than one the resolver has already freed. */
            trx->txn = NULL;
            trx->dirty = false;
            trx->txn_generation++;
            trx->prepared_xid.clear();
            trx->fts_meta_pending.clear();
            trx->fts_meta_dirty = false;
        }
        else
        {
            trx->prepared_xid = std::move(key);
        }
    }
    return 0;
}

static TDB_XA_RESULT tidesdb_commit_by_xid(TDB_HTON_CB_ARG XID *xid)
{
    std::string key = tdb_xid_key(xid);
    tidesdb_txn_t *txn = NULL;
    {
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        auto it = tdb_prepared_txns.find(key);
        if (it == tdb_prepared_txns.end()) return TDB_XA_OK; /* not one of ours */
        txn = it->second;
        tdb_prepared_txns.erase(it);
    }
    int rc = tidesdb_txn_commit_prepared(txn);
    if (rc != TDB_SUCCESS)
    {
        /* A transient failure leaves the transaction prepared; re-register so the coordinator can
           retry the decision. */
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        tdb_prepared_txns[key] = txn;
        return TDB_XA_FAILED(tdb_rc_to_ha(rc, "commit_by_xid"));
    }
    tidesdb_txn_free(txn);
    tdb_xa_tc_clear(key);
    return TDB_XA_OK;
}

static TDB_XA_RESULT tidesdb_rollback_by_xid(TDB_HTON_CB_ARG XID *xid)
{
    std::string key = tdb_xid_key(xid);
    tidesdb_txn_t *txn = NULL;
    {
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        auto it = tdb_prepared_txns.find(key);
        if (it == tdb_prepared_txns.end()) return TDB_XA_OK; /* not one of ours */
        txn = it->second;
        tdb_prepared_txns.erase(it);
    }
    int rc = tidesdb_txn_rollback_prepared(txn);
    if (rc != TDB_SUCCESS)
    {
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        tdb_prepared_txns[key] = txn;
        return TDB_XA_FAILED(tdb_rc_to_ha(rc, "rollback_by_xid"));
    }
    tidesdb_txn_free(txn);
    tdb_xa_tc_clear(key);
    return TDB_XA_OK;
}

/* load the transactions the library recovered as in-doubt after the last shutdown into the prepared
   registry and the recovery cursor, once.  caller holds tdb_prepared_mtx. */
static void tdb_load_recovered_prepared()
{
    if (tdb_recovery_loaded) return;
    tdb_recovery_loaded = true;
    if (!tdb_global) return;

    int count = 0;
    if (tidesdb_recover_prepared(tdb_global, NULL, 0, &count) != TDB_SUCCESS || count <= 0) return;

    std::vector<tidesdb_prepared_txn_t> buf((size_t)count);
    int got = 0;
    if (tidesdb_recover_prepared(tdb_global, buf.data(), count, &got) != TDB_SUCCESS) return;

    for (int i = 0; i < got; i++)
    {
        std::string key((const char *)buf[i].xid, buf[i].xid_size);
        tdb_prepared_txns[key] = buf[i].txn;
        tdb_recovery_xids.push_back(std::move(key));
    }
}

/* hand the server the in-doubt XIDs recovered from storage, in batches drained across calls, so it
   can rebuild its transaction cache and let the operator resolve each with XA COMMIT/ROLLBACK. */
static int tidesdb_recover(TDB_RECOVER_ARGS)
{
    if (!xid_list || len == 0) return 0;

    std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
    tdb_load_recovered_prepared();

    uint n = 0;
    while (tdb_recovery_pos < tdb_recovery_xids.size() && n < len)
    {
        const std::string &k = tdb_recovery_xids[tdb_recovery_pos++];
        XID *slot = TDB_RECOVER_SLOT(xid_list, n);
        /* A key that does not parse was not written by a build of this engine, and reporting a
           guess at it would have the server resolve a transaction it cannot name. */
        if (!tdb_xid_from_key((const uint8_t *)k.data(), k.size(), slot)) continue;

        static const std::vector<std::pair<std::string, std::string>> none;
        auto found = tdb_recovery_mod_tables.find(k);
        TDB_RECOVER_ATTACH_MOD_TABLES(
            xid_list, n, mem_root, found == tdb_recovery_mod_tables.end() ? none : found->second);
        n++;
    }
    return (int)n;
}

/* ******************** handlerton registration ******************** */

/* ******************** prepared-in-coordinator state ******************** */

/* The reserved column family recording which prepared transactions also reached the transaction
   coordinator.  A user's XA PREPARE completes there; a transaction that crashed between the
   engine's prepare and the coordinator's did not, and must still be rolled back at recovery.  The
   two are indistinguishable from the engine's own prepare, so the state is written here when it
   happens and read back after a restart.

   Keyed by the serialized XID.  The value carries the tables the transaction wrote to, which the
   server needs to lock while the transaction is in doubt after a restart. */
static constexpr const char TDB_XA_TC_CF_NAME[] = "__tidesql_xa_tc";

/* the record's leading byte, so a later layout change is detectable rather than misparsed. */
static constexpr uint8_t TDB_XA_TC_FORMAT_VERSION = 1;

/* no database or table name comes close to this; a stored length beyond it means the record is not
   one of ours and the parse stops rather than sizing a string from a bad number. */
static constexpr uint32_t TDB_XA_TC_MAX_NAME = 4096;

typedef std::vector<std::pair<std::string, std::string>> tdb_table_names_t;

/**
 * tdb_xa_tc_encode
 * serialise the tables a prepared transaction modified
 * @param tables the [database, table] pairs
 * @return the record bytes
 */
static std::string tdb_xa_tc_encode(const tdb_table_names_t &tables)
{
    std::string out;
    out.push_back((char)TDB_XA_TC_FORMAT_VERSION);

    char n[4];
    tdb_put_u32_be((uint32_t)tables.size(), n);
    out.append(n, sizeof(n));

    for (const auto &tbl : tables)
        for (const std::string *s : {&tbl.first, &tbl.second})
        {
            tdb_put_u32_be((uint32_t)s->size(), n);
            out.append(n, sizeof(n));
            out.append(*s);
        }
    return out;
}

/**
 * tdb_xa_tc_decode
 * parse a record written by tdb_xa_tc_encode
 * @param data the stored bytes
 * @param len their length
 * @param out out -- the table names, cleared first
 * @return true when the record is well formed; a record that is not yields no names rather than
 *         names guessed from bytes this build does not understand
 */
static bool tdb_xa_tc_decode(const uint8_t *data, size_t len, tdb_table_names_t &out)
{
    out.clear();
    if (!data || len < 5 || data[0] != TDB_XA_TC_FORMAT_VERSION) return false;

    const uint8_t *p = data + 1;
    const uint8_t *const end = data + len;
    const uint32_t count = tdb_get_u32_be(p);
    p += 4;

    for (uint32_t i = 0; i < count; i++)
    {
        std::string name[2];
        for (int half = 0; half < 2; half++)
        {
            if ((size_t)(end - p) < 4) return false;
            const uint32_t n = tdb_get_u32_be(p);
            p += 4;
            if (n > TDB_XA_TC_MAX_NAME || (size_t)(end - p) < n) return false;
            name[half].assign((const char *)p, n);
            p += n;
        }
        out.emplace_back(std::move(name[0]), std::move(name[1]));
    }
    return true;
}

/**
 * tdb_xa_tc_cf
 * the reserved family, created on first use
 * @return the handle, or NULL when it could not be opened
 */
static tidesdb_column_family_t *tdb_xa_tc_cf()
{
    if (!tdb_global) return NULL;

    tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, TDB_XA_TC_CF_NAME);
    if (!cf)
    {
        tidesdb_column_family_config_t config = tidesdb_default_column_family_config();
        if (tidesdb_create_column_family(tdb_global, TDB_XA_TC_CF_NAME, &config) == TDB_SUCCESS)
            cf = tidesdb_get_column_family(tdb_global, TDB_XA_TC_CF_NAME);
    }
    return cf;
}

/**
 * tdb_xa_tc_mark
 * record, durably, that the transaction under this XID reached the coordinator's prepared state
 * @param key the serialized XID
 * @param tables the tables the transaction wrote to, for recovery to lock
 * @return true when the record committed
 */
static bool tdb_xa_tc_mark(const std::string &key, const tdb_table_names_t &tables)
{
    tidesdb_column_family_t *cf = tdb_xa_tc_cf();
    if (!cf) return false;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    const std::string rec = tdb_xa_tc_encode(tables);
    bool ok =
        tidesdb_txn_put(txn, cf, (const uint8_t *)key.data(), key.size(),
                        (const uint8_t *)rec.data(), rec.size(), TIDESDB_TTL_NONE) == TDB_SUCCESS;
    if (ok)
        ok = tidesdb_txn_commit(txn) == TDB_SUCCESS;
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
    return ok;
}

/**
 * tdb_xa_tc_clear
 * drop the marker once the transaction has been committed or rolled back, so a later recovery does
 * not resurrect an XID that is already resolved
 * @param key the serialized XID
 */
static void tdb_xa_tc_clear(const std::string &key)
{
    tidesdb_column_family_t *cf = tdb_xa_tc_cf();
    if (!cf || key.empty()) return;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    if (tidesdb_txn_delete(txn, cf, (const uint8_t *)key.data(), key.size()) == TDB_SUCCESS)
        (void)tidesdb_txn_commit(txn);
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
}

/*
  Record that this connection's prepared transaction also reached the coordinator.

  Called immediately after a successful XA PREPARE.  The engine's own prepare is already durable by
  this point; what is written here is the second fact recovery needs, that the coordinator got far
  enough for the transaction to be the user's to decide rather than the server's to discard.
*/
static int tidesdb_set_prepared_in_tc(handlerton *hton, THD *thd)
{
    if (!thd) return 0;

    XID xid;
    thd_get_xid(thd, (MYSQL_XID *)&xid);
    const std::string key = tdb_xid_key(&xid);

    tdb_table_names_t tables;
    if (tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, hton))
    {
        tables = std::move(trx->prepared_mod_tables);
        trx->prepared_mod_tables.clear();
    }

    if (!tdb_xa_tc_mark(key, tables))
    {
        sql_print_error(
            "[TIDESDB] could not record the prepared-in-coordinator state for an XA transaction; "
            "it would be rolled back rather than left in doubt after a restart");
        return HA_ERR_INTERNAL_ERROR;
    }
    return 0;
}

/*
  Report the transactions that were prepared in the coordinator before the last shutdown.

  The coordinator rolls back any externally prepared transaction whose XID it does not find here, so
  this is what keeps a user's XA PREPARE decidable across a restart.
*/
static int tidesdb_recover_prepared_in_tc(handlerton *, Xa_state_list &xa_list)
{
    tidesdb_column_family_t *cf = tdb_xa_tc_cf();
    if (!cf || !tdb_global) return 0;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return 0;

    tidesdb_iter_t *iter = NULL;
    if (tidesdb_iter_new(txn, cf, &iter) != TDB_SUCCESS || !iter)
    {
        (void)tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return 0;
    }

    /* A fresh iterator is unpositioned; seek before the first read or the scan returns nothing. */
    (void)tidesdb_iter_seek_to_first(iter);

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *k = NULL;
        uint8_t *v = NULL;
        size_t klen = 0, vlen = 0;
        tdb_owned_buf kg(k);
        tdb_owned_buf vg(v);
        XID xid;
        if (tidesdb_iter_key(iter, &k, &klen) == TDB_SUCCESS && tdb_xid_from_key(k, klen, &xid))
        {
            xa_list.add(xid, enum_ha_recover_xa_state::PREPARED_IN_TC);

            /* recover() runs next and has to hand these back with the XID, so the names are held
               until then rather than the family being scanned a second time. */
            tdb_table_names_t tables;
            if (tidesdb_iter_value(iter, &v, &vlen) == TDB_SUCCESS)
                (void)tdb_xa_tc_decode(v, vlen, tables);

            std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
            tdb_recovery_mod_tables[std::string((const char *)k, klen)] = std::move(tables);
        }
        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return 0;
}

/*
  Re-assert the prepared-in-coordinator state for one recovered transaction.

  The server calls this once per in-doubt transaction it has taken over at recovery, and treats a
  missing callback as a failure to recover that transaction, so it is answered even though the
  record it writes is normally the one recovery just read.
*/
static TDB_XA_RESULT tidesdb_set_prepared_in_tc_by_xid(handlerton *, XID *xid)
{
    if (!xid) return TDB_XA_OK;

    const std::string key = tdb_xid_key(xid);

    tdb_table_names_t tables;
    {
        std::lock_guard<std::mutex> lk(tdb_prepared_mtx);
        auto found = tdb_recovery_mod_tables.find(key);
        if (found != tdb_recovery_mod_tables.end()) tables = found->second;
    }

    if (!tdb_xa_tc_mark(key, tables))
    {
        sql_print_error(
            "[TIDESDB] could not record the prepared-in-coordinator state for a recovered XA "
            "transaction; it would be rolled back rather than left in doubt after another restart");
        return TDB_XA_FAILED(HA_ERR_INTERNAL_ERROR);
    }
    return TDB_XA_OK;
}

void tidesdb_txn_register(handlerton *hton)
{
    /* per-transaction savepoint storage carved out of the server's transaction area */
    hton->savepoint_offset = sizeof(tidesdb_savepoint_t);

    /* one TidesDB txn per BEGIN..COMMIT, shared by every handler on the connection */
    hton->commit = tidesdb_commit;
    hton->rollback = tidesdb_rollback;
    hton->close_connection = tidesdb_close_connection;

    hton->savepoint_set = tidesdb_savepoint_set;
    hton->savepoint_rollback = tidesdb_savepoint_rollback;
    hton->savepoint_rollback_can_release_mdl = tidesdb_savepoint_rollback_can_release_mdl;
    hton->savepoint_release = tidesdb_savepoint_release;
    hton->start_consistent_snapshot = tidesdb_start_consistent_snapshot;

    /* two-phase commit (XA) -- prepare durably logs the write batch under the XID, the by-xid
       resolvers finish a prepared transaction from any connection, and recover replays the ones the
       library found in-doubt after a restart.
       Offered only with the binary log on, and not because the engine needs it.  Declaring prepare
       is what takes the server's count of two-phase-capable engines past one, and with the binary
       log off that makes it coordinate through TC_LOG_MMAP -- a fixed 24KB ring whose commit path
       reserves a slot per transaction and returns without releasing it when an engine's commit
       fails.  A slot never released pins its whole page, and six of them deadlock every later
       commit in the server.  InnoDB's commit cannot fail, so nothing else reaches it; this engine
       validates at commit, so a write conflict under concurrency reaches it within minutes.  With
       the binary log on the coordinator is the binary log, which has no such pool, so XA is
       offered there in full. */
    if (tdb_binlog_enabled())
    {
        hton->prepare = tidesdb_prepare;
        TDB_HTON_SET_PREPARED_IN_TC(hton, tidesdb_set_prepared_in_tc);
        TDB_HTON_RECOVER_PREPARED_IN_TC(hton, tidesdb_recover_prepared_in_tc);
        TDB_HTON_SET_PREPARED_IN_TC_BY_XID(hton, tidesdb_set_prepared_in_tc_by_xid);
        hton->recover = tidesdb_recover;
        hton->commit_by_xid = tidesdb_commit_by_xid;
        hton->rollback_by_xid = tidesdb_rollback_by_xid;
    }
    else
    {
        sql_print_warning(
            "[TIDESDB] the binary log is off, so XA is not offered: coordinating a second "
            "two-phase engine without it goes through a fixed-size log the server does not "
            "release a slot in when an engine's commit fails, which deadlocks every later "
            "commit.  Enable --log-bin to use XA with TidesDB.");
    }
}
