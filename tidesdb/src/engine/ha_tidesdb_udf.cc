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

/* the engine's administrative operations, as functions.
 *
 * taking a backup, writing a checkpoint and rotating an encryption key are things the server does
 * once when asked.  none of them is a setting, and none leaves state behind that a later SHOW
 * VARIABLES could report -- a backup path is where a backup went, not how the engine is configured.
 * so they are functions: the statement says what it did, returns a value, and fails as a statement
 * when the operation fails, rather than a variable being set to trigger a side effect whose outcome
 * has to be looked for in the error log.
 *
 * it also settles a problem the variable form could not.  a sysvar update callback runs holding
 * LOCK_global_system_variables, and every one of these operations blocks: a backup drains the open
 * transactions and waits for the flushes to finish.  holding that mutex across the wait stops every
 * session that reads or writes any global variable, and a connection stuck on it mid-transaction
 * never drains, so the backup waits for a transaction that is waiting for the backup.  the variable
 * form worked around that by unlocking the server's mutex and taking it again afterwards, which
 * also put the engine on a symbol mysqld does not export to plugins on Windows.  a function is not
 * called holding that mutex, so there is nothing to work around. */

#include <filesystem>

#include "ha_tidesdb.h"

#include <cstring>
#include <string>

#include <mysql/components/services/udf_registration.h>
#include <mysql/service_plugin_registry.h>
#include <mysql/udf_registration_types.h>

#include "sql_class.h"
#include "src/engine/ha_tidesdb_crypto.h"
#include "src/handler/ha_tidesdb_internal.h"

/* the registry and the UDF service, held for as long as the functions are registered */
static SERVICE_TYPE(registry) *tdb_udf_registry = nullptr;
static SERVICE_TYPE(udf_registration) *tdb_udf_reg = nullptr;

/* every one of these answers with a short word, so one small buffer per call is enough and the
   return never has to allocate. */
static constexpr unsigned long TDB_UDF_RESULT_MAX = 32;
static constexpr const char TDB_UDF_OK[] = "OK";

/* ******************** shared argument handling ******************** */

/**
 * tdb_udf_init_one_string
 * accept exactly one non-null string argument
 * @param initid the UDF instance
 * @param args the call's arguments
 * @param message out -- why the call was refused
 * @param fn the function name, for the message
 * @return true when the arguments are wrong, which is how a UDF init reports refusal
 */
static bool tdb_udf_init_one_string(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *fn)
{
    if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT)
    {
        snprintf(message, MYSQL_ERRMSG_SIZE, "%s() takes one string argument", fn);
        return true;
    }
    initid->maybe_null = false;
    initid->const_item = false;
    initid->max_length = TDB_UDF_RESULT_MAX;
    return false;
}

/**
 * tdb_udf_ok
 * the value every one of these returns when the operation succeeded
 * @param result the server's result buffer
 * @param length out -- the answer's length
 * @return the answer
 */
static char *tdb_udf_ok(char *result, unsigned long *length)
{
    *length = (unsigned long)(sizeof(TDB_UDF_OK) - 1);
    memcpy(result, TDB_UDF_OK, *length);
    return result;
}

/**
 * tdb_udf_path_arg
 * the path a call names, rejected when empty
 * @param args the call's arguments
 * @param out out -- the path
 * @return true when a usable path was given
 */
static bool tdb_udf_path_arg(UDF_ARGS *args, std::string &out)
{
    if (!args->args[0] || args->lengths[0] == 0) return false;
    out.assign(args->args[0], args->lengths[0]);
    return !out.empty();
}

/**
 * tdb_udf_release_own_txn
 * end the calling connection's transaction before an operation that waits for transactions to drain
 * @param thd the session
 *
 * A backup waits for every open transaction to finish.  The calling connection may be holding one
 * itself -- opened when it first touched a table and not yet committed -- and a backup that waits
 * for it waits forever, because the connection is inside the backup.  Ending it here is what the
 * caller would have had to do anyway.
 */
static void tdb_udf_release_own_txn(THD *thd)
{
    if (!thd) return;

    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn) return;

    tidesdb_txn_rollback(trx->txn);
    tidesdb_txn_free(trx->txn);
    trx->txn = nullptr;
    trx->dirty = false;
    trx->txn_generation++;
    trx->fts_meta_pending.clear();
    trx->fts_meta_dirty = false;
}

/**
 * tdb_udf_dest_writable
 * whether a backup destination satisfies the library's requirement that it be absent or empty
 * @param path the destination named by the caller
 * @param why out -- on refusal, the reason to show the client
 * @return true when the copy may proceed
 *
 * The library refuses a destination that already holds files, so that a backup cannot be written
 * over another one and leave a directory holding half of each.  Checked here so the caller is told
 * which of the two reasons applies, rather than being handed the io error the library would
 * otherwise return.
 */
static bool tdb_udf_dest_writable(const std::string &path, std::string &why)
{
    std::error_code ec;
    const std::filesystem::path dir(path);

    if (!std::filesystem::exists(dir, ec)) return true; /* absent is fine; the library creates it */

    if (!std::filesystem::is_directory(dir, ec))
    {
        why = "the path exists and is not a directory";
        return false;
    }
    if (std::filesystem::directory_iterator(dir, ec) != std::filesystem::directory_iterator())
    {
        why = "the directory is not empty; a backup is written only to an absent or empty path";
        return false;
    }
    return true;
}

/* ******************** tidesdb_backup ******************** */

extern "C" bool tidesdb_backup_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    return tdb_udf_init_one_string(initid, args, message, "tidesdb_backup");
}

extern "C" char *tidesdb_backup_udf(UDF_INIT *, UDF_ARGS *args, char *result, unsigned long *length,
                                    unsigned char *, unsigned char *error)
{
    std::string path;
    if (!tdb_global)
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] the engine is not open", MYF(0));
        *error = 1;
        return nullptr;
    }
    if (!tdb_udf_path_arg(args, path))
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] tidesdb_backup() needs a destination path",
                        MYF(0));
        *error = 1;
        return nullptr;
    }

    std::string why;
    if (!tdb_udf_dest_writable(path, why))
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] backup to '%s' refused: %s", MYF(0),
                        path.c_str(), why.c_str());
        *error = 1;
        return nullptr;
    }

    tdb_udf_release_own_txn(current_thd);

    const int rc = tidesdb_backup(tdb_global, path.c_str());
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] backup to '%s' failed (err=%d)", path.c_str(), rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] backup to '%s' failed (err=%d)", MYF(0),
                        path.c_str(), rc);
        *error = 1;
        return nullptr;
    }
    return tdb_udf_ok(result, length);
}

/* ******************** tidesdb_checkpoint ******************** */

extern "C" bool tidesdb_checkpoint_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    return tdb_udf_init_one_string(initid, args, message, "tidesdb_checkpoint");
}

extern "C" char *tidesdb_checkpoint_udf(UDF_INIT *, UDF_ARGS *args, char *result,
                                        unsigned long *length, unsigned char *,
                                        unsigned char *error)
{
    std::string path;
    if (!tdb_global)
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] the engine is not open", MYF(0));
        *error = 1;
        return nullptr;
    }
    if (!tdb_udf_path_arg(args, path))
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] tidesdb_checkpoint() needs a destination path",
                        MYF(0));
        *error = 1;
        return nullptr;
    }

    std::string why;
    if (!tdb_udf_dest_writable(path, why))
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] checkpoint to '%s' refused: %s", MYF(0),
                        path.c_str(), why.c_str());
        *error = 1;
        return nullptr;
    }

    tdb_udf_release_own_txn(current_thd);

    /* A checkpoint is two things in order, and the library spells them separately: the durability
       barrier on the live database, then the copy of it written to the given path. */
    int rc = tidesdb_checkpoint(tdb_global);
    if (rc == TDB_SUCCESS) rc = tidesdb_backup(tdb_global, path.c_str());
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] checkpoint to '%s' failed (err=%d)", path.c_str(), rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] checkpoint to '%s' failed (err=%d)", MYF(0),
                        path.c_str(), rc);
        *error = 1;
        return nullptr;
    }
    return tdb_udf_ok(result, length);
}

/* ******************** tidesdb_rotate_table_key ******************** */

extern "C" bool tidesdb_rotate_table_key_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT)
    {
        snprintf(message, MYSQL_ERRMSG_SIZE,
                 "tidesdb_rotate_table_key() takes one encryption key id");
        return true;
    }
    initid->maybe_null = false;
    initid->const_item = false;
    initid->max_length = TDB_UDF_RESULT_MAX;
    return false;
}

extern "C" char *tidesdb_rotate_table_key_udf(UDF_INIT *, UDF_ARGS *args, char *result,
                                              unsigned long *length, unsigned char *,
                                              unsigned char *error)
{
    if (!args->args[0])
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] tidesdb_rotate_table_key() needs a key id",
                        MYF(0));
        *error = 1;
        return nullptr;
    }

    const long long key_id = *((long long *)args->args[0]);
    if (key_id <= 0 || key_id > (long long)TIDESDB_MAX_ENCRYPTION_KEY_ID)
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] encryption key id %lld is out of range",
                        MYF(0), key_id);
        *error = 1;
        return nullptr;
    }

    if (tdb_crypto_rotate_table_key((unsigned int)key_id) != TDB_CRYPTO_STATUS_OK)
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] could not rotate encryption key %lld", MYF(0),
                        key_id);
        *error = 1;
        return nullptr;
    }
    return tdb_udf_ok(result, length);
}

/* ******************** tidesdb_rotate_master_key ******************** */

extern "C" bool tidesdb_rotate_master_key_udf_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    if (args->arg_count != 0)
    {
        snprintf(message, MYSQL_ERRMSG_SIZE, "tidesdb_rotate_master_key() takes no arguments");
        return true;
    }
    initid->maybe_null = false;
    initid->const_item = false;
    initid->max_length = TDB_UDF_RESULT_MAX;
    return false;
}

extern "C" char *tidesdb_rotate_master_key_udf(UDF_INIT *, UDF_ARGS *, char *result,
                                               unsigned long *length, unsigned char *,
                                               unsigned char *error)
{
    if (tdb_crypto_rotate_master_key() != TDB_CRYPTO_STATUS_OK)
    {
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] could not rotate the master key", MYF(0));
        *error = 1;
        return nullptr;
    }
    return tdb_udf_ok(result, length);
}

/* ******************** registration ******************** */

/* one row per function, so registering and unregistering cannot drift apart */
struct tdb_udf_entry
{
    const char *name;
    Udf_func_any func;
    Udf_func_init init;
};

static const tdb_udf_entry tdb_udfs[] = {
    {"tidesdb_backup", (Udf_func_any)tidesdb_backup_udf, tidesdb_backup_udf_init},
    {"tidesdb_checkpoint", (Udf_func_any)tidesdb_checkpoint_udf, tidesdb_checkpoint_udf_init},
    {"tidesdb_rotate_table_key", (Udf_func_any)tidesdb_rotate_table_key_udf,
     tidesdb_rotate_table_key_udf_init},
    {"tidesdb_rotate_master_key", (Udf_func_any)tidesdb_rotate_master_key_udf,
     tidesdb_rotate_master_key_udf_init},
};

void tdb_udf_register_all()
{
    tdb_udf_registry = mysql_plugin_registry_acquire();
    if (!tdb_udf_registry) return;

    my_h_service svc = nullptr;
    if (tdb_udf_registry->acquire("udf_registration", &svc) || !svc)
    {
        mysql_plugin_registry_release(tdb_udf_registry);
        tdb_udf_registry = nullptr;
        sql_print_warning(
            "[TIDESDB] the UDF registration service is unavailable; backup, checkpoint and key "
            "rotation cannot be called");
        return;
    }
    tdb_udf_reg = reinterpret_cast<SERVICE_TYPE(udf_registration) *>(svc);

    for (const tdb_udf_entry &e : tdb_udfs)
        if (tdb_udf_reg->udf_register(e.name, STRING_RESULT, e.func, e.init, nullptr))
            sql_print_warning("[TIDESDB] could not register %s()", e.name);
}

void tdb_udf_unregister_all()
{
    if (!tdb_udf_reg || !tdb_udf_registry) return;

    for (const tdb_udf_entry &e : tdb_udfs)
    {
        int was_present = 0;
        (void)tdb_udf_reg->udf_unregister(e.name, &was_present);
    }

    using reg_t = SERVICE_TYPE_NO_CONST(udf_registration);
    tdb_udf_registry->release(reinterpret_cast<my_h_service>(const_cast<reg_t *>(tdb_udf_reg)));
    mysql_plugin_registry_release(tdb_udf_registry);
    tdb_udf_reg = nullptr;
    tdb_udf_registry = nullptr;
}
