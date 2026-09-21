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

/* DDL timing for a server that resolves table definitions through a transactional dictionary.
 *
 * a DROP the statement later rolls back must leave the table intact, and a CREATE that rolls back
 * must leave nothing behind.  the engine therefore does not touch a column family while the
 * statement is in flight: it records what was asked for, and the post-DDL hook -- which the server
 * calls after the dictionary transaction has committed or rolled back -- carries it out or
 * abandons it.
 *
 * this follows InnoDB's arrangement, which solves the same problem against the same dictionary: a
 * durable log of pending schema changes, replayed by the post-DDL hook, plus a recovery pass that
 * picks up whatever a crash left behind.  the one deliberate difference is where the decision comes
 * from.  InnoDB keeps its log in an InnoDB table, so the log commits and rolls back with the
 * dictionary and a surviving entry is by itself proof the change committed.  this engine's log
 * lives in its own storage, which the dictionary transaction does not reach, so a surviving entry
 * proves only that the statement started.  the outcome is therefore read from the dictionary --
 * which is what decided it -- and the log supplies durability rather than the verdict.  that split
 * is what makes a crash between the dictionary's commit and this hook recoverable instead of
 * leaving storage orphaned forever.
 *
 * the immediate counterpart is ha_tidesdb_ddl_immediate.cc; CMakeLists.txt compiles exactly one. */

#include "src/handler/ha_tidesdb_ddl_atomicity.h"

#include <atomic>
#include <string>
#include <vector>

#include "my_systime.h"
#include "sql_class.h"
#include "src/core/cf_name.h"
#include "src/core/ddl_log_rec.h"
#include "src/handler/ha_tidesdb_internal.h"

namespace ddl_log = tidesdb::ddl_log;

/* the reserved family the pending records live in.  a user table's family is <db>__<table> and a
   database component never arrives empty, so a name leading with the separator cannot collide. */
static constexpr const char TDB_DDL_CF_NAME[] = "__tidesql_ddl";

/* identifies this server run.  a record carrying a different one was written by a run that is over,
   so whatever statement wrote it is over too and its outcome is settled -- which is what makes it
   safe for the recovery pass to resolve while other sessions are running DDL of their own. */
static uint64_t tdb_ddl_boot_id = 0;

/* distinguishes records within this run.  DDL is not a hot path, so one atomic counter is ample. */
static std::atomic<uint64_t> tdb_ddl_sequence{0};

/* the reserved family handle, opened once. */
static tidesdb_column_family_t *tdb_ddl_cf = nullptr;

/* whether the recovery pass has run.  it needs a session to reach the dictionary, so it happens at
   the first post-DDL hook rather than at plugin init where there is none. */
static std::atomic<bool> tdb_ddl_recovered{false};

/**
 * tdb_ddl_split_path
 * the database and table a server path names
 * @param path the server table path, such as "./db/table"
 * @param db out -- the database component
 * @param table out -- the table component
 * @return true when both components were present
 */
static bool tdb_ddl_split_path(const char *path, std::string &db, std::string &table)
{
    if (!path) return false;
    const tidesdb::cf_name::table_path parts = tidesdb::cf_name::split_table_path(path);
    if (!parts.has_dir || parts.db.empty() || parts.table.empty()) return false;
    db = parts.db;
    table = parts.table;
    return true;
}

/**
 * tdb_ddl_open_cf
 * the reserved family, created on first use
 * @return the handle, or null when it could not be opened
 */
static tidesdb_column_family_t *tdb_ddl_open_cf()
{
    if (tdb_ddl_cf || !tdb_global) return tdb_ddl_cf;

    tdb_ddl_cf = tidesdb_get_column_family(tdb_global, TDB_DDL_CF_NAME);
    if (!tdb_ddl_cf)
    {
        tidesdb_column_family_config_t config = tidesdb_default_column_family_config();
        if (tidesdb_create_column_family(tdb_global, TDB_DDL_CF_NAME, &config) == TDB_SUCCESS)
            tdb_ddl_cf = tidesdb_get_column_family(tdb_global, TDB_DDL_CF_NAME);
    }
    return tdb_ddl_cf;
}

/**
 * tdb_ddl_write
 * write one pending record through the statement's own transaction
 * @param thd the session
 * @param key the record key
 * @param key_len the key length
 * @param value the encoded record
 * @return true when the record was written into the statement's transaction
 *
 * Not committed here, and deliberately so.  The record goes in the transaction the server commits
 * or rolls back with the statement, so it reaches storage exactly when the schema change does.
 * That is what lets the hook afterwards read the verdict out of the engine's own storage: a record
 * that is there committed, one that is gone did not.
 */
static bool tdb_ddl_write(THD *thd, const uint8_t *key, size_t key_len, const std::string &value)
{
    tidesdb_column_family_t *cf = tdb_ddl_open_cf();
    if (!cf || !tdb_global) return false;

    tidesdb_txn_t *txn = tdb_stmt_txn_for_ddl(thd);
    if (!txn) return false;

    return tidesdb_txn_put(txn, cf, key, key_len, (const uint8_t *)value.data(), value.size(),
                           TIDESDB_TTL_NONE) == TDB_SUCCESS;
}

/* What this statement wrote, so the hook afterwards knows which records to look for.  A rolled-back
   CREATE takes its record down with the transaction, so the intent to undo it survives only here --
   which is why the list is kept rather than the hook simply scanning what is in storage. */
/**
 * tdb_ddl_pending_for
 * the list of records this connection's current statement wrote
 * @param thd the session
 * @return the list, or NULL when the connection has no engine transaction yet
 */
static std::vector<std::pair<std::string, std::string>> *tdb_ddl_pending_for(THD *thd)
{
    if (!thd) return nullptr;
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    return trx ? &trx->ddl_pending : nullptr;
}

/**
 * tdb_ddl_erase
 * remove one pending record once it has been carried out or abandoned
 * @param key the record key
 * @param key_len the key length
 */
static void tdb_ddl_erase(const uint8_t *key, size_t key_len)
{
    tidesdb_column_family_t *cf = tdb_ddl_open_cf();
    if (!cf || !tdb_global) return;

    tidesdb_txn_t *txn = nullptr;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    if (tidesdb_txn_delete(txn, cf, key, key_len) == TDB_SUCCESS)
        (void)tidesdb_txn_commit(txn);
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
}

/**
 * tdb_ddl_record
 * persist an intent before the change it describes is attempted
 * @param thd the session
 * @param path the server table path
 * @param what whether the families are to be dropped or were just created
 * @return true when the record is durable
 */
static bool tdb_ddl_record(THD *thd, const char *path, ddl_log::intent what)
{
    std::string db, table;
    if (!thd || !tdb_ddl_split_path(path, db, table)) return false;

    ddl_log::record rec;
    rec.what = what;
    rec.path = path;
    rec.db = db;
    rec.table = table;

    std::string value;
    if (!ddl_log::encode_record(rec, value)) return false;

    uint8_t key[ddl_log::KEY_LEN];
    const size_t key_len = ddl_log::encode_key(tdb_ddl_boot_id, tdb_ddl_sequence.fetch_add(1), key);

    if (!tdb_ddl_write(thd, key, key_len, value)) return false;

    /* tdb_ddl_write opened the transaction this hangs off, so the list is reachable now. */
    auto *pending = tdb_ddl_pending_for(thd);
    if (!pending) return false;
    pending->emplace_back(std::string((const char *)key, key_len), std::move(value));
    return true;
}

/**
 * tdb_ddl_record_survived
 * whether a record this statement wrote is in storage now
 * @param key the record key
 * @param key_len the key length
 * @return true when the record is there, which means the statement committed
 */
static bool tdb_ddl_record_survived(const uint8_t *key, size_t key_len)
{
    tidesdb_column_family_t *cf = tdb_ddl_open_cf();
    if (!cf || !tdb_global) return false;

    tidesdb_txn_t *txn = nullptr;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    uint8_t *val = nullptr;
    size_t vlen = 0;
    tdb_owned_buf vguard(val);
    const bool found = tidesdb_txn_get(txn, cf, key, key_len, &val, &vlen) == TDB_SUCCESS && val;

    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return found;
}

/**
 * tdb_ddl_resolve
 * carry out or abandon one pending record, then remove it
 * @param key the record key
 * @param key_len the key length
 * @param rec the parsed record
 * @param committed whether the statement that wrote the record committed
 */
static void tdb_ddl_resolve(const uint8_t *key, size_t key_len, const ddl_log::record &rec,
                            bool committed)
{
    /* Both intents turn on the same question, for different reasons, and want the same answer: the
       families go when the table the record describes is not the one the server ended up with.

         drop   + committed      the drop happened, so remove them
         drop   + rolled back    the table is still there, so keep them
         create + committed      the table is there, so keep them
         create + rolled back    nothing was created, so remove what it made

       Written as one condition rather than a branch per intent, because a branch that drifted apart
       would silently destroy a live table. */
    const bool remove_families = (rec.what == ddl_log::intent::drop) == committed;

    if (remove_families)
    {
        const int rc = tdb_ddl_drop_table_cfs_now(rec.path.c_str());
        if (rc != 0)
        {
            /* The statement has already finished, so this cannot fail it.  Keep the record so a
               later pass retries rather than orphaning the storage. */
            sql_print_error(
                "[TIDESDB] post-DDL: failed to remove the column families of `%s`.`%s` (err=%d); "
                "the pending record is kept and will be retried",
                rec.db.c_str(), rec.table.c_str(), rc);
            return;
        }
    }

    /* A record from a rolled-back statement went down with the transaction and there is nothing to
       erase; erasing anyway is a harmless no-op, and keeps the one caller simple. */
    tdb_ddl_erase(key, key_len);
}

/**
 * tdb_ddl_recover
 * resolve every record left in storage by an earlier server run
 *
 * A record only reaches storage when the statement that wrote it committed, so every record found
 * here describes a change that happened and had not been carried out when the server stopped.
 * Records this run wrote are not touched: their statements resolve through the hook.
 */
static void tdb_ddl_recover()
{
    tidesdb_column_family_t *cf = tdb_ddl_open_cf();
    if (!cf || !tdb_global) return;

    tidesdb_txn_t *txn = nullptr;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *iter = nullptr;
    if (tidesdb_iter_new(txn, cf, &iter) != TDB_SUCCESS || !iter)
    {
        (void)tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return;
    }

    /* Collect first, resolve after.  Resolving writes back to this same family, which must not
       happen while an iterator over it is open. */
    std::vector<std::pair<std::string, ddl_log::record>> pending;

    /* A fresh iterator is unpositioned, so it is seeked before the first read; stepping it without
       that reads nothing and the pass would silently find no pending work. */
    (void)tidesdb_iter_seek_to_first(iter);

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *k = nullptr, *v = nullptr;
        size_t klen = 0, vlen = 0;
        if (tidesdb_iter_key_value(iter, &k, &klen, &v, &vlen) != TDB_SUCCESS)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        tdb_owned_buf kguard(k), vguard(v);
        if (!k || !v)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        uint64_t boot = 0;
        if (!ddl_log::decode_key_boot_id(k, klen, &boot))
        {
            tidesdb_iter_next(iter);
            continue;
        }

        if (boot == tdb_ddl_boot_id)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        ddl_log::record rec;
        if (!ddl_log::decode_record(v, vlen, &rec))
        {
            /* A record this build cannot parse is left alone rather than acted on: acting on a
               misread path would remove the wrong table's storage. */
            sql_print_warning(
                "[TIDESDB] post-DDL: a pending schema-change record could not be parsed and is "
                "left in place");
            tidesdb_iter_next(iter);
            continue;
        }

        pending.emplace_back(std::string((const char *)k, klen), std::move(rec));
        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);

    for (const auto &entry : pending)
        tdb_ddl_resolve((const uint8_t *)entry.first.data(), entry.first.size(), entry.second,
                        true);
}

/**
 * tdb_ddl_post_ddl
 * the server's post-DDL hook.  runs once per statement after the dictionary transaction has
 * committed or rolled back, and cannot fail the statement
 * @param thd the session
 */
static void tdb_ddl_post_ddl(THD *thd)
{
    if (!thd) return;

    /* Records left by earlier runs are swept once, on the first hook after startup.  It no longer
       needs a session for anything -- it reads only the engine's own storage -- but this is still
       the first point at which the engine is certainly up and serving. */
    bool expected = false;
    if (tdb_ddl_recovered.compare_exchange_strong(expected, true)) tdb_ddl_recover();

    /* This statement's own records.  Whether each reached storage is the statement's verdict: the
       server committed or rolled back the transaction they were written through, so the record is
       there exactly when the schema change is. */
    auto *pending = tdb_ddl_pending_for(thd);
    if (!pending || pending->empty()) return;

    std::vector<std::pair<std::string, std::string>> mine;
    mine.swap(*pending);
    for (const auto &p : mine)
    {
        ddl_log::record rec;
        if (!ddl_log::decode_record((const uint8_t *)p.second.data(), p.second.size(), &rec))
            continue;
        const uint8_t *k = (const uint8_t *)p.first.data();
        tdb_ddl_resolve(k, p.first.size(), rec, tdb_ddl_record_survived(k, p.first.size()));
    }
}

int tdb_ddl_drop_table(THD *thd, const char *path)
{
    std::string db, table;

    /* An internal or temporary table handed over without a database component has no dictionary
       entry to consult, so there is no outcome to wait for and it is removed now. */
    if (!thd || !tdb_ddl_split_path(path, db, table)) return tdb_ddl_drop_table_cfs_now(path);

    if (!tdb_ddl_record(thd, path, ddl_log::intent::drop))
    {
        /* Without a durable record the drop could be forgotten across a crash, which would orphan
           the table's storage.  Fail the statement instead: a DROP that reports an error and
           changes nothing is recoverable, one that half-happens is not. */
        sql_print_error("[TIDESDB] could not record the pending drop of `%s`.`%s`", db.c_str(),
                        table.c_str());
        return HA_ERR_GENERIC;
    }

    /* Reported as success: nothing has failed, and nothing has happened yet either.  The statement
       is free to roll back, which is the whole point. */
    return 0;
}

void tdb_ddl_note_created(THD *thd, const char *path)
{
    std::string db, table;
    if (!thd || !tdb_ddl_split_path(path, db, table)) return;

    if (!tdb_ddl_record(thd, path, ddl_log::intent::create))
        sql_print_warning(
            "[TIDESDB] could not record the creation of `%s`.`%s`; if this statement rolls back "
            "its column families will remain until the table is created and dropped again",
            db.c_str(), table.c_str());
}

void tdb_ddl_register_hooks(handlerton *hton)
{
    if (!hton) return;

    /* Identify this server run.  Any record carrying a different value was written by a run that
       has ended, which is what lets the recovery pass resolve it without racing a live statement.
     */
    tdb_ddl_boot_id = (uint64_t)my_micro_time();

    hton->post_ddl = tdb_ddl_post_ddl;

    /* Claiming atomic DDL is what makes the server route DDL through the hook above and keep the
       engine's changes in step with the dictionary's.  It is also what lets the engine claim
       foreign-key support: the server couples the two, and takes its foreign-key path in DROP TABLE
       only for an engine that has claimed both. */
    hton->flags |= HTON_SUPPORTS_ATOMIC_DDL | HTON_SUPPORTS_FOREIGN_KEYS;
}
