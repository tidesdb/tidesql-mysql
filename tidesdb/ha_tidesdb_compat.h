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

#ifndef HA_TIDESDB_COMPAT_H
#define HA_TIDESDB_COMPAT_H

/* the MySQL adaptation layer.  RULES.md rule 6 confines conditional compilation to this file:
   ordinary engine code never spells a server construct directly, it uses the names defined here.

   what lives here, and why it is one file rather than scattered:

     1. a server name the engine would otherwise repeat in many places (the too-big-row error)
     2. a server call whose shape the engine would otherwise restate per call site (the dd::Table*
        arguments, the two-phase-commit callback signatures)
     3. a whole handler method set the engine declares in one line instead of twenty
     4. a MySQL behaviour worth recording next to the macro that depends on it, so the reason
        survives the next person to read it

   the plugin targets MySQL only, so where a MySQL release changes something under the engine the
   seam is a version test at the macro that needs it rather than a branch wrapping engine code.
   there is one platform branch, at the end of the file: keeping the module mapped is something
   only Windows needs, and rule 6 is why it lives here rather than at the call. */

/* ******************** server version ******************** */

/* MYSQL_VERSION_ID, for the few macros below that differ between MySQL releases.  it leads the
   file because anything testing it has to see it first. */
#include <mysql_version.h>

/* ******************** server prelude ******************** */

/* MySQL 8.0 deleted my_global.h and folded its contents into the headers that need them, and
   removed sql_priv.h in 5.7.  where they still exist they must precede handler.h and my_base.h,
   which use typedefs (ulonglong, int64, sql_mode_t) those headers define; the .clang-format
   IncludeCategories rule pins my_global.h to sort first for that reason, and stays correct here
   because on MySQL there is no include to sort. */

/* the query-option bit flags the transaction and DML paths test -- OPTION_BEGIN,
   OPTION_NOT_AUTOCOMMIT, OPTION_NO_FOREIGN_KEY_CHECKS.  MySQL keeps them in query_options.h, a
   leaf header declaring nothing but constants, so including it here costs nothing and lets every
   engine source name the flags without repeating the include. */
#include "query_options.h"

/* the index-condition pushdown result enum.  MySQL declares it in its own leaf header, which
   handler.h does not pull in for a plugin, so the engine names it through the alias below and this
   is where the declaration comes from. */
#include "my_icp.h"

/* ******************** types ******************** */

/* MySQL 8.0 removed my_bool in favour of plain bool.  the plugin uses it only for sysvar backing
   stores, where the storage width matters to the sysvar framework rather than to engine logic. */
typedef bool tdb_sysvar_bool_t;

/* the row buffers the server hands the engine are offsets from table->record[0], and the row and
   key codecs carry that offset as a signed difference.  MySQL 8.0 dropped the my_ptrdiff_t alias
   in favour of the standard ptrdiff_t; both name the same type. */
#include <cstddef>
typedef std::ptrdiff_t my_ptrdiff_t;

/* ******************** debug assertions ******************** */

/* MySQL 8.0 retired DBUG_ASSERT in favour of plain assert().  the engine still writes DBUG_ASSERT,
   which reads as the intent it is; this is where the spelling lands. */
#include <cassert>
#define DBUG_ASSERT(x) assert(x)

/* ******************** row-found status ******************** */

/* the scan paths tell the server whether record[0] holds a row.  MySQL made TABLE::status private
   as m_status and publishes set_no_row() / set_found_row(); the engine names the intent rather
   than the accessor, so a later rename lands here and nowhere else. */
#define TDB_TABLE_SET_NO_ROW(tbl) ((tbl)->set_no_row())
#define TDB_TABLE_SET_FOUND_ROW(tbl) ((tbl)->set_found_row())

/* ******************** handler error codes ******************** */

/* error 139, a row too large to store.  the engine writes HA_ERR_TO_BIG_ROW throughout, so the
   one-letter-shorter spelling it inherited stays correct and lands on MySQL's name here. */
#define HA_ERR_TO_BIG_ROW HA_ERR_TOO_BIG_ROW

/* the interrupted-query code needs no alias.  MySQL defines HA_ERR_QUERY_INTERRUPTED (196)
   directly, so the engine names it as-is. */

/* ******************** index condition pushdown ******************** */

/* MySQL's pushed-condition callback reports one of three outcomes:

     ICP_RESULT { ICP_NO_MATCH, ICP_MATCH, ICP_OUT_OF_RANGE }

   there is no error state and no killed-query state among them, which is what the note below is
   about.

   the consequence for this engine is bounded and worth stating.  the index scans in
   ha_tidesdb_index.cc translate a killed-query result from the pushed condition into
   HA_ERR_QUERY_INTERRUPTED in four places.  on MySQL that signal never arrives through the
   condition callback, so the kill is observed instead by the thd_killed() check the same scan
   loops already run on every row.  a killed query therefore notices at the next row boundary
   rather than inside condition evaluation.  that is a real behavioural difference, small and
   bounded, recorded here because no macro removes it. */
/* these are macros rather than typedefs on purpose.  this header supplies the server prelude and
   therefore has to be included ahead of handler.h, at which point neither enum is declared yet; a
   typedef would be evaluated here and fail, a macro is not evaluated until the engine names it. */
#define tdb_icp_result_t ICP_RESULT
#define TDB_ICP_NO_MATCH ICP_NO_MATCH
#define TDB_ICP_MATCH ICP_MATCH
#define TDB_ICP_OUT_OF_RANGE ICP_OUT_OF_RANGE
/* the pushed condition cannot report a kill here, so the scan asks the thread directly.  the test
   sits at the same point in the loop, so a killed query still stops at the same row -- it is
   observed one level out rather than inside condition evaluation. */
#define TDB_ICP_ABORTED(icp_result, thd) ((void)(icp_result), (thd) != nullptr && thd_killed(thd))

/* ******************** table_flags ******************** */

/* capability bits MySQL does not define.  none has a MySQL equivalent to map onto -- they describe
   optimizer and server behaviours MySQL either lacks or drives a different way -- so on MySQL the
   group contributes nothing and table_flags() names one symbol rather than carrying seven
   conditional terms:

     HA_CAN_VIRTUAL_COLUMNS         MySQL drives generated columns through a different path
     HA_REC_NOT_IN_SEQ              no MySQL equivalent
     HA_ONLINE_ANALYZE              no MySQL equivalent
     HA_CAN_ONLINE_BACKUPS          no MySQL equivalent; the backup_dir sysvar path is unaffected
     HA_CONCURRENT_OPTIMIZE         no MySQL equivalent
     HA_CAN_TABLES_WITHOUT_ROLLBACK no MySQL equivalent
     HA_CAN_FORCE_BULK_DELETE       no MySQL equivalent; MySQL has no bulk-delete envelope

   generated-column behaviour in particular must be re-verified by test on MySQL rather than
   assumed from the flag's absence. */
/* capability bits only MySQL asks an engine to declare.

   HA_DESCENDING_INDEX: the engine's key codec already inverts a descending key part's bytes so a
   forward scan reads it in reverse order, which is the whole of what descending index support means
   here; without the flag MySQL refuses the syntax outright and the capability is lost silently.

   HA_GENERATED_COLUMNS, HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN, HA_SUPPORTS_DEFAULT_EXPRESSION: a
   generated column's value, and a column default that is an expression rather than a constant,
   both reach the engine the same way -- the server evaluates them into record[0] before the row is
   written or deleted, and the engine encodes whatever it finds there.  MySQL gates each on a
   declared capability, refusing an index on a virtual column without the second and a default
   expression without the third.  the engine keeps
   no background row maintenance of its own, so it never needs to evaluate one of these expressions
   away from a server row. */
#define TDB_TABLE_FLAGS_SERVER_EXTRA                                                      \
    (HA_DESCENDING_INDEX | HA_GENERATED_COLUMNS | HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN | \
     HA_SUPPORTS_DEFAULT_EXPRESSION)

/* MySQL asks whether the primary key is clustered through the primary_key_is_clustered() virtual
   rather than a table flag, so this contributes nothing to table_flags() and the handler answers
   through the override instead. */
#define TDB_TABLE_FLAGS_CLUSTERED 0

/* ******************** handlerton field names ******************** */

/* the same hook under two names.  the file-extension list has identical type and meaning either
   way.  the shutdown hook does not: MySQL's pre_dd_shutdown fires before the data dictionary
   closes, a narrower window, but still after the point where syncing the write-ahead log is the
   right thing to do. */
#define TDB_HTON_FILE_EXTENSIONS file_extensions
#define TDB_HTON_PRE_SHUTDOWN pre_dd_shutdown

/* ******************** data dictionary parameters ******************** */

/* MySQL 8.0 abolished .frm files and gave every DDL-adjacent handler method a dd::Table parameter
   carrying the server's own copy of the table definition.  TidesDB keeps its own catalog and does
   not read the dictionary, so each of these is accepted and ignored -- but the parameter must be
   present or the override does not bind.

   that binding is the whole reason these are macros rather than two hand-written method sets.  a
   declaration carrying the wrong arity alongside the override keyword fails to compile, which is
   what we want; the danger is a port that drops override to silence the error, after which the
   base-class method runs and tables quietly stop being created.  every method keeps override and
   takes its arity from here.

   these expand to a leading comma plus the parameter, so a call site reads

       int create(const char *name, TABLE *form, HA_CREATE_INFO *info TDB_DD_CREATE_ARG) override;

   which is where RULES.md rule 6's "no token pasting" bound is stretched, to parameter fragments.
   the alternative is two full copies of the handler declaration. */
#define TDB_DD_OPEN_ARG , const dd::Table *dd_table_def [[maybe_unused]]
#define TDB_DD_CREATE_ARG , dd::Table *dd_table_def [[maybe_unused]]
#define TDB_DD_TABLE_ARG dd_table_def
#define TDB_DD_DELETE_ARG , const dd::Table *dd_table_def [[maybe_unused]]
#define TDB_DD_RENAME_ARG \
    , const dd::Table *dd_from_def [[maybe_unused]], dd::Table *dd_to_def [[maybe_unused]]
#define TDB_DD_ALTER_ARG \
    , const dd::Table *dd_old_def [[maybe_unused]], dd::Table *dd_new_def [[maybe_unused]]
#define TDB_TRUNCATE_ARG dd::Table *dd_table_def [[maybe_unused]]

/* the largest unsigned long long, which one server publishes from its global header and MySQL
   leaves to the standard limits header.  the sysvar declarations name it as a range bound. */
#include <climits>
#define ULONGLONG_MAX ULLONG_MAX

/* the enum-name table a sysvar's TYPELIB is built from.  MySQL's struct ends after the length
   array; others carry a further array of retired enum values.  the tail supplies whichever the
   target expects, so one initialiser serves both. */
#define TDB_TYPELIB_TAIL NULL

/* the system-variable type a sysvar update callback is handed. */
#define TDB_SYS_VAR SYS_VAR

/* ******************** handler method shapes ******************** */

/* the row buffer the server hands the write paths.  MySQL passes it mutable, so a declaration that
   keeps it const does not override the base method -- and, with override on every method, says so
   at compile time rather than by quietly running the base implementation. */
#define TDB_ROW_IN uchar

/* the range bounds handed to records_in_range, mutable on MySQL, and the trailing page-range
   estimate MySQL does not ask for. */
#define TDB_KEY_RANGE key_range
#define TDB_RECORDS_IN_RANGE_TAIL

/* multi-range read.  MySQL identifies a range by a raw pointer where others carry a typed range id,
   and its cost entry point takes a force-default flag in place of a row limit. */
#define TDB_MRR_RANGE_ID char *
#define TDB_MRR_INFO_CONST_TAIL bool *force_default_mrr
#define TDB_MRR_INFO_CONST_TAIL_ARG force_default_mrr

/* bulk update.  MySQL counts duplicates in a uint and reports end-of-bulk through a void return,
   so the engine's error path there has nowhere to go and must surface errors as it finds them. */
#define TDB_DUPKEY_COUNT uint
#define TDB_END_BULK_UPDATE_RET void
#define TDB_END_BULK_UPDATE_RETURN(rc) ((void)(rc))
#define TDB_INDEX_FLAGS_CLUSTERED 0

/* the bulk-insert hint.  MySQL passes only the row estimate. */
#define TDB_BULK_INSERT_ARGS ha_rows rows [[maybe_unused]]

/* a column's name.  MySQL stores it as a bare pointer; others carry a counted string. */
#define TDB_FIELD_NAME(f) ((f)->field_name)
#define TDB_KEY_NAME_LEN(ki) (strlen((ki)->name))

/* the referential action on a foreign key.  same enumerators, different enum name. */
#define TDB_FK_OPTION fk_option

/* a table reference in the server's list of open tables.  MySQL renamed the class in 8.0. */
#define TDB_TABLE_REF Table_ref

/* copying one field's value into another of a possibly different type. */
#define TDB_FIELD_CONV(to, from) field_conv_slow((to), (from))

/* whether a table the engine reached through prelocking is genuinely prepared for this statement's
   writes.  a write-lock alone is not enough on a server that also assigns a row-logging identity
   when it prepares a table: writing to a table without one builds a malformed row event out of an
   unassigned id, which is a crash rather than an error.  the check is expressed here so the
   cascade paths ask one question rather than repeating the two-part test at each call site. */
#define TDB_TABLE_WRITABLE_IN_STMT(tbl)                                                          \
    ((tbl)->file != nullptr && (tbl)->file->get_lock_type() == F_WRLCK && (tbl)->s != nullptr && \
     (tbl)->s->table_map_id.is_valid())

/* how a cascade writes the child row.
 *
   the engine enforces its own foreign keys, so a cascade has to modify a second table from inside
   the first table's own write.  which entry point that goes through matters: the server's wrapper
   also row-logs, and row-logging the child from inside the parent's row-logging re-enters the
   server's table-map machinery mid-statement, which does not survive it.

   InnoDB has the same shape and solves it the same way -- its cascades run inside the engine and
   never re-enter the server for the child row -- so where that hazard exists the engine calls its
   own method directly.  the child's own indexes, and any foreign key the child is itself a parent
   of, are maintained either way, because those live below this line.

   what differs is that the cascaded child rows are not written to the row-based binary log by this
   statement.  a replica running this engine re-derives them from the same constraint; a replica
   that does not carries the parent change alone.  that is the documented behaviour of engine-side
   cascades rather than a gap introduced here, and it is why this is confined to the one server
   whose row logging cannot be re-entered. */
#define TDB_CASCADE_DELETE_ROW(child_ha, file, rec) \
    ((child_ha) ? (child_ha)->delete_row(rec) : (file)->ha_delete_row(rec))
#define TDB_CASCADE_UPDATE_ROW(child_ha, file, old_rec, new_rec) \
    ((child_ha) ? (child_ha)->update_row((old_rec), (new_rec))   \
                : (file)->ha_update_row((old_rec), (new_rec)))

/* whether an externally prepared XA transaction stays attached to the connection that prepared it.
 *
   MySQL detaches the transaction at prepare and resolves every prepared transaction by XID,
   including in the connection that prepared it -- rather than leaving it attached and resolving it
   through the ordinary commit hook.
 *
   that matters because the by-XID resolver owns and frees the transaction.  leaving the connection
   pointing at it there means the next statement commits through a handle the resolver has already
   freed, which surfaces as an internal handler error on a statement that looks unrelated.  so where
   the server detaches, the engine detaches too and hands ownership to the registry. */
#define TDB_XA_PREPARE_DETACHES 1
/* the prepared-in-coordinator state.
 *
   MySQL's recovery distinguishes a transaction the engine prepared from one the coordinator also
   prepared, and the distinction decides an externally prepared XA transaction's fate after a
   restart: the coordinator looks for its XID among the ones the engine reports as prepared in the
   coordinator, and rolls back anything it does not find.  an engine that does not implement these
   loses every user XA PREPARE across a restart -- silently, since rolling back an in-doubt
   transaction is a legitimate recovery outcome and is reported as success.
 *
   it also cannot be answered from what the engine already knows.  a transaction that reached the
   engine's own prepare but crashed before the coordinator finished must still be rolled back, so
   reporting every prepared transaction as prepared-in-coordinator would resurrect exactly the
   transactions recovery exists to discard.  the state has to be recorded when it happens. */
#define TDB_HTON_SET_PREPARED_IN_TC(hton, fn) ((hton)->set_prepared_in_tc = (fn))
#define TDB_HTON_RECOVER_PREPARED_IN_TC(hton, fn) ((hton)->recover_prepared_in_tc = (fn))
#define TDB_HTON_SET_PREPARED_IN_TC_BY_XID(hton, fn) ((hton)->set_prepared_in_tc_by_xid = (fn))
#define TDB_XA_HAS_PREPARED_IN_TC 1

/* ******************** online DDL flags ******************** */

/* the bitmask describing what an ALTER asks for.  MySQL scopes every enumerator inside
   Alter_inplace_info and drops the ALTER_ prefix, so the engine names the two sets it cares about
   once here rather than spelling them at each test.  the sets themselves are the engine's policy:

     instant  operations that touch no row.  add and drop column are here because the row format
              carries its own null-bitmap width and field count, so a row written under any prior
              schema still deserialises.
     index    operations that rebuild or drop a secondary index, which run inplace under MVCC
              without server-level blocking.

   the mapping is deliberately conservative: where a server has no counterpart for one of the
   engine's instant operations, the flag is simply absent from its set, which downgrades that ALTER
   to a copy rather than claiming an instant it cannot deliver. */

#define TDB_ALTER_FLAGS_T Alter_inplace_info::HA_ALTER_FLAGS

#define TDB_ALTER_INSTANT_SET                                                               \
    (Alter_inplace_info::ALTER_COLUMN_NAME | Alter_inplace_info::ALTER_COLUMN_DEFAULT |     \
     Alter_inplace_info::CHANGE_CREATE_OPTION | Alter_inplace_info::DROP_CHECK_CONSTRAINT | \
     Alter_inplace_info::ALTER_VIRTUAL_GCOL_EXPR | Alter_inplace_info::ALTER_RENAME |       \
     Alter_inplace_info::RENAME_INDEX | Alter_inplace_info::CHANGE_INDEX_OPTION |           \
     Alter_inplace_info::ADD_COLUMN | Alter_inplace_info::DROP_COLUMN |                     \
     Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |                                        \
     Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER |                                       \
     Alter_inplace_info::ALTER_COLUMN_COLUMN_FORMAT |                                       \
     Alter_inplace_info::ALTER_COLUMN_STORAGE_TYPE)

/* ADD_SPATIAL_INDEX is a flag of its own on MySQL rather than part of the general add-index flag,
   so it has to be named here explicitly.  the engine's own check inside the set is what
   refuses a spatial index inplace, and it refuses it with a reason -- left outside the set, the
   statement falls through to the unreasoned refusal and the user is told only that the algorithm
   is unsupported. */
#define TDB_ALTER_INDEX_SET                                                         \
    (Alter_inplace_info::ADD_INDEX | Alter_inplace_info::DROP_INDEX |               \
     Alter_inplace_info::ADD_UNIQUE_INDEX | Alter_inplace_info::DROP_UNIQUE_INDEX | \
     Alter_inplace_info::ADD_SPATIAL_INDEX)

#define TDB_ALTER_PK_SET (Alter_inplace_info::ADD_PK_INDEX | Alter_inplace_info::DROP_PK_INDEX)
#define TDB_ALTER_CHANGE_CREATE_OPTION Alter_inplace_info::CHANGE_CREATE_OPTION

/* ******************** assorted server helpers ******************** */

/* the pushed index condition.  one server publishes a shared evaluator that also checks the kill
   flag and the scan's end range; MySQL leaves every engine to assemble the same thing from the
   handler's own members, which is what InnoDB's innobase_index_cond does. */
#define TDB_INDEX_COND_CHECK(h)                                   \
    (((h)->end_range && (h)->compare_key_icp((h)->end_range) > 0) \
         ? ICP_OUT_OF_RANGE                                       \
         : ((h)->pushed_idx_cond->val_int() ? ICP_MATCH : ICP_NO_MATCH))

/* the length of a key prefix.  MySQL derives it from the keypart map alone, taking no key bytes. */
#define TDB_CALC_KEY_LEN(tbl, idx, key, map) calculate_key_len((tbl), (idx), (map))

/* Field internals the row and key codecs reach into.  MySQL made these private and published
   accessors, so the codecs name the intent and the accessor spelling lands here. */
#define TDB_VARSTRING_LENGTH_BYTES(f) ((f)->get_length_bytes())
#define TDB_FIELD_PTR(f) ((f)->field_ptr())
#define TDB_FIELD_HAS_FLAG(f, flag) ((f)->is_flag_set(flag))
#define TDB_FIELD_MAYBE_NULL(f) ((f)->is_nullable())

/* per-column CREATE TABLE options.  the engine has exactly one -- which column, if any, supplies a
   row's own expiry -- and it is asked for as a question rather than a struct.

   a column's ENGINE_ATTRIBUTE reaches the engine on the Field itself: the server reads it out of
   the dictionary while it builds the share and copies it to Field::m_engine_attribute, which it
   does before handing the table to either create() or open().  so the engine never reads the
   dictionary for this, and never has to match a column by name to find it. */
#define TDB_FIELD_IS_TTL_SOURCE(tbl, i) tdb_field_is_ttl_source((tbl), (i))

/* ******************** handler method sets ******************** */

/* two groups of overrides exist on one server and not the other, and neither reduces to a
   parameter difference: the methods are simply absent from the MySQL handler, so declaring them
   with override does not compile and declaring them without it defines a method the server never
   calls.  the whole group is therefore supplied from here.
 *
   these are the one place the layer declares members rather than naming a type or an argument.
   the alternative is two hand-maintained copies of the handler declaration, which is exactly the
   drift this file exists to prevent -- so the exception is taken here, in the compat layer, where
   rule 6 confines it, and nowhere else.

   the cost model.  MySQL costs in flat doubles, split across read_time and index_only_read_time.
   the arithmetic behind them lives in src/core/cost_model, so this declares only the adapters.

   the foreign-key catalog.  MySQL moved foreign keys into the data dictionary in 8.0 and removed
   every one of these methods from the handler, along with the FOREIGN_KEY_INFO type they describe
   a constraint with.  the enforcement paths are unaffected; it is the read-back half that has no
   MySQL counterpart, and ha_tidesdb_fk.cc is excluded from the build there. */

#define TDB_COST_METHOD_OVERRIDES                                     \
    double scan_time() override;                                      \
    double read_time(uint index, uint ranges, ha_rows rows) override; \
    double index_only_read_time(uint keynr, double records) override;

#define TDB_FOREIGN_KEY_METHOD_OVERRIDES /* MySQL answers these from its data dictionary */

/* these three are not handler methods on MySQL, but two of them still do work the engine needs, so
   they stay declared as ordinary members rather than being deleted along with their overrides.
   reset_auto_increment is what makes TRUNCATE restart the counter, and MySQL routes TRUNCATE to
   handler::truncate, which calls it; ft_end releases the full-text scan state that index_end would
   otherwise leak.  index_type has no caller here and only affects SHOW INDEX output. */
#define TDB_INDEX_TYPE_OVERRIDE const char *index_type(uint key_number);

#define TDB_RESET_AUTO_INCREMENT_OVERRIDE int reset_auto_increment(ulonglong value);

#define TDB_FT_END_OVERRIDE void ft_end();

/* handler methods MySQL offers that the engine implements.  each is declared here and defined in
   its own translation unit, so the declaration and the definition stay in step. */

/* row sampling.  the base implementation reads every row and throws most of them away, which costs
   a full decode per row to keep a few; the engine can decide before it decodes. */
/* the row the server's sampler keeps next.  only the middle of the three sampling methods is
   replaced: the server's own sample_init and sample_end already open and close an ordinary table
   scan, which is what this engine wants, and they seed the generator the replacement draws from. */
#define TDB_SAMPLING_METHOD_OVERRIDES int sample_next(void *scan_ctx, uchar *buf) override;

/* whether the table holds a row at all.  the base answer is "assume it does", and the server asks
   before choosing a plan that is only worth it on an empty table. */
#define TDB_IS_TABLE_EMPTY_OVERRIDE bool is_table_empty() const override;

/* the handler factory the handlerton points at.  MySQL passes a partitioned flag the engine has no
   use for -- it exposes no partitioning -- but the signature must match or the assignment is a
   type error. */
#define TDB_HTON_CREATE_ARGS handlerton *hton, TABLE_SHARE *table, bool, MEM_ROOT *mem_root

/* custom CREATE TABLE options.  the option tables and the handlerton hooks that publish them exist
   only where the server can parse engine-specific option syntax; MySQL has no such mechanism, so
   neither the tables nor the registration are compiled there.  the option set itself is not lost --
   the 12 tidesdb_default_* session variables that supply their defaults are declared normally and
   work on every target, and remain the way to change any of these on MySQL until the option
   decision lands. */

#define TDB_TABLE_OPTION_LISTS /* no option tables on this server */
#define TDB_REGISTER_TABLE_OPTIONS(hton) ((void)(hton))

/* the server stored the attribute without reading it, so the engine is the only thing that can
   tell a mistyped option from a real one, and CREATE and ALTER ask here before accepting. */
#define TDB_TABLE_OPTIONS_ERROR(tbl, error) tdb_table_options_error((tbl), (error))
#define TDB_COLUMN_OPTIONS_ERROR(tbl, error) tdb_column_options_error((tbl), (error))

/* and because the server records only what the table named, not what the unnamed options resolved
   to, the engine writes the resolved set into the table's own column family when the table is
   created and reads it back when the table is opened.  see OPTIONS_META_KEY. */
#define TDB_TABLE_OPTIONS_STORE(cf, opts) tdb_table_options_store((cf), (opts))
#define TDB_TABLE_OPTIONS_LOAD(cf, out) tdb_table_options_load((cf), (out))

/* handlerton hooks whose shape or presence differs.
 *
   drop_database takes its path const on MySQL, and the engine only reads it.
   flush_logs carries a binlog-group flag this engine ignores: the write-ahead log is
   database-level, so one sync covers every column family whatever prompted the call.
   drop_table has no MySQL counterpart at all -- there the server reaches an engine's table teardown
   only through handler::delete_table, which already carries the same logic. */
#define TDB_HTON_DROP_DATABASE_PATH const char
#define TDB_HTON_FLUSH_LOGS_ARGS handlerton *, bool
#define TDB_HTON_SET_DROP_TABLE(hton, fn) ((void)sizeof(&(fn)))
#define TDB_HTON_PRE_SHUTDOWN_ARGS handlerton *

/* the microsecond wall clock the statistics refresh gate compares against. */
#include "my_systime.h"
#define TDB_MICRO_TIME() my_micro_time()

/* the boolean-to-int helper one server publishes from its global header. */
#define MY_TEST(a) ((a) ? 1 : 0)
#include "strxnmov.h"

/* the diagnostic-area severity a note is pushed at.  MySQL spells the enumerators SL_*; others
   spell them WARN_LEVEL_*.  Both live on Sql_condition. */
#define TDB_WARN_LEVEL_NOTE Sql_condition::SL_NOTE
#define TDB_WARN_LEVEL_WARN Sql_condition::SL_WARNING

/* the bounded min and max one server publishes as macros from its global header.  MySQL has none,
   and the standard library's templates are the right answer there. */
#define MY_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MY_MAX(a, b) ((a) > (b) ? (a) : (b))

/* record which index a duplicate-key error was raised on, so print_error names it.  MySQL carries
   one field; others carry a second recording the index the failing lookup used.  this is expressed
   as an operation rather than a member alias on purpose: aliasing both names onto MySQL's single
   field turns the two-member assignment into a self-assignment, which is undefined behaviour. */
#define TDB_SET_DUP_KEY(h, idx) ((h)->errkey = (idx))

/* one index's name.  MySQL stores it as a bare pointer; others carry a counted string, so the
   engine asks for the characters through this rather than naming either representation. */
#define TDB_KEY_NAME(ki) ((ki)->name)

/* the key-part count including the primary-key parts a secondary index carries.  the concept is
   the same; the member is named differently. */
#define TDB_KEY_EXT_PARTS(ki) ((ki)->actual_key_parts)

/* whether a field may hold NULL, and how a field writes its comparable sort bytes.  MySQL renamed
   both when it reworked Field.

   the buffer is cleared first, and that is not belt and braces.  a field writes its sort weights
   and returns how many bytes it wrote; it fills the rest of the buffer only for a collation that
   pads.  a NO PAD collation -- which every utf8mb4_0900_* collation is, including the server
   default -- leaves the remainder untouched, so a key built over an unclouded buffer carries
   whatever was in that memory.  two keys for the same value then differ, which loses index lookups
   and lets a duplicate past a UNIQUE index, and the bytes themselves reach the on-disk index.
   clearing first makes the tail deterministic zeros, which is also the ordering NO PAD asks for:
   a shorter weight string sorts before a longer one sharing its prefix. */
#define TDB_FIELD_IS_NULLABLE(f) ((f)->is_nullable())
#define TDB_FIELD_SORT_STRING(f, buf, len) \
    (memset((buf), 0, (len)), (void)(f)->make_sort_key((buf), (len)))

/* temporarily reading every column.  MySQL takes the bitmap by value and hands back its raw map;
   others take it by address and hand back the bitmap.  the engine names the intent. */
#define TDB_COLUMN_MAP_SAVED my_bitmap_map *
#define TDB_USE_ALL_COLUMNS(tbl, bmp) tmp_use_all_columns((tbl), (tbl)->bmp)
#define TDB_RESTORE_COLUMN_MAP(tbl, bmp, saved) tmp_restore_column_map((tbl)->bmp, (saved))

/* the scratch buffer a status callback may format into.  MySQL hands a char*, others a void*; the
   engine's callbacks all point var->value at their own storage and never write to it, so the name
   exists only to satisfy the signature. */

/* ******************** two-phase commit ******************** */

/* the parts of an XID the engine serialises.  the transaction is stored under a key it must be
   reconstructable from, because recovery has to hand the server back the same XID the user named,
   so every part is read and written explicitly rather than copied as a struct image.  MySQL keeps
   the fields private behind accessors, so only the reads need naming here; set() is used directly.
 */
#define TDB_XID_FORMAT_ID(xid) ((long)(xid)->get_format_id())
#define TDB_XID_GTRID_LEN(xid) ((long)(xid)->get_gtrid_length())
#define TDB_XID_BQUAL_LEN(xid) ((long)(xid)->get_bqual_length())
#define TDB_XID_DATA(xid) ((const char *)(xid)->get_data())

/* whether a table reaching the engine is partitioned.
 *
   a partitioned table is served by the server's partitioning handler, which dispatches the
   multi-range read across child handlers with its own ordering state; an engine that accepts the
   read there is called back without that state.  the engine therefore declines, and asks here.

   MySQL has no general partitioning layer, so no table reaches the engine partitioned and the
   question answers itself. */
#define TDB_TABLE_IS_PARTITIONED(tbl) (false)

/* the leading parameter of a transaction or savepoint handlerton callback.
 *
   MySQL passes the handlerton the callback was reached through as its first argument, so every
   transaction and savepoint callback below carries it.  named once here, a future release that
   drops it changes this line rather than each signature. */
#define TDB_HTON_CB_ARG handlerton *,

/* the phase-two resolvers.  MySQL returns a typed XA status rather than a plain int, and an
   unknown XID is a distinct outcome rather than a generic failure. */
#define TDB_XA_RESULT xa_status_code
#define TDB_XA_OK XA_OK
#define TDB_XA_ERR_NOTA XAER_NOTA
/* a phase-two failure.  MySQL wants the typed resource-manager error rather than the handler error
   code, so the handler code is discarded and the coordinator is told only that the RM failed. */
#define TDB_XA_FAILED(ha_err) ((void)(ha_err), XAER_RMERR)

/* recovery enumeration.  MySQL hands a richer per-transaction record and a MEM_ROOT to allocate
   from; others hand a bare XID array.  the engine writes the XID either way, so only the shape of
   the slot and the extra parameter differ. */
#define TDB_RECOVER_ARGS handlerton *, XA_recover_txn *xid_list, uint len, MEM_ROOT *mem_root
#define TDB_RECOVER_SLOT(xid_list, n) (&(xid_list)[n].id)

/* the tables a recovered in-doubt transaction wrote to.  MySQL holds a metadata lock on each of
   them for as long as the transaction stays in doubt, so that a DDL statement cannot drop a table
   a prepared transaction is still entitled to commit into; it takes the list per transaction,
   allocated from the recovery MEM_ROOT it also hands the engine.

   the names are copied onto the MEM_ROOT because the server reads them well after this returns. */
#define TDB_RECOVER_ATTACH_MOD_TABLES(xid_list, n, mem_root, names)                          \
    do                                                                                       \
    {                                                                                        \
        auto *tdb_mt_ = new ((mem_root)) List<st_handler_tablename>();                       \
        (xid_list)[n].mod_tables = tdb_mt_;                                                  \
        if (tdb_mt_)                                                                         \
            for (const auto &tdb_nm_ : (names))                                              \
            {                                                                                \
                auto *tdb_tn_ = new ((mem_root)) st_handler_tablename();                     \
                if (!tdb_tn_) break;                                                         \
                tdb_tn_->db =                                                                \
                    strmake_root((mem_root), tdb_nm_.first.c_str(), tdb_nm_.first.size());   \
                tdb_tn_->tablename =                                                         \
                    strmake_root((mem_root), tdb_nm_.second.c_str(), tdb_nm_.second.size()); \
                if (!tdb_tn_->db || !tdb_tn_->tablename) break;                              \
                if (tdb_mt_->push_back(tdb_tn_, (mem_root))) break;                          \
            }                                                                                \
    } while (0)

/* ordered group commit.  MySQL orders commits in the binlog coordinator and publishes no engine
   hook for it, so the engine's handoff is simply not registered there; the plain commit path
   already does the work. */

/* ******************** misc helpers ******************** */

/* the set-bit count of a keypart map.  MySQL leaves it to the standard library. */
#include <bit>
#define TDB_COUNT_BITS(v) ((uint)std::popcount((unsigned long long)(v)))

/* Field pack and unpack.  MySQL folds the caller-supplied bound into the pack signature and drops
   the end pointer from unpack; the engine already sizes its buffer from serialize_estimate_size, so
   it passes that bound through. */
#define TDB_FIELD_PACK(f, to, from, max_len) ((f)->pack((to), (from), (max_len)))
#define TDB_FIELD_UNPACK(f, to, from, from_end) ((f)->unpack((to), (from), 0U))

/* whether reading a blob-family column back has to be done by hand rather than through the field's
   own unpack.
 *
   MySQL's Field_blob::unpack calls Field_blob::store, which copies the bytes into a buffer the
   Field itself owns and shares, rather than pointing the field at the caller's buffer so each
   record keeps its own view of it.
 *
   that difference is not cosmetic.  an UPDATE saves the old row with store_record, which copies
   the field's pointer, so both records end up addressing the same field-owned buffer; writing the
   new value overwrites it in place and the server's old-versus-new comparison then finds the two
   records identical.  the update is dropped with "Changed: 0" and no error -- silently, and only
   when every column being set is blob-family, since any other changed column makes the comparison
   differ for its own reasons.  BLOB, TEXT, etc all sit behind this.
 *
   so rather than let unpack copy, the engine points the field at its own row buffer.  that buffer
   outlives the field's use of it: it is the handler's row buffer, replaced only on the next read
   of the same handler. */
#define TDB_FIELD_UNPACK_COPIES_BLOBS 1

/* the handlerton capability flags.
 *
   foreign keys are the delicate one.  MySQL couples HTON_SUPPORTS_FOREIGN_KEYS to
   HTON_SUPPORTS_ATOMIC_DDL and says so in its own source -- "we don't have any SEs which support
   FKs but do not support atomic DDL", guarded by an assert that vanishes in a release build.  an
   engine that claims the first without the second takes the server's foreign-key branch in
   DROP TABLE while the parent invalidator it dereferences there is still null, and the server
   segfaults.

   both are claimed on MySQL, and not from here: src/handler/ha_tidesdb_ddl_deferred.cc sets them
   together at registration, beside the post-DDL hook that makes the pair honest by holding a
   column-family create or drop until the dictionary transaction has committed.  keeping the claim
   there means it cannot outlive the machinery behind it.

   ENGINE_ATTRIBUTE is how the per-table storage options reach the engine on MySQL, which has no
   engine-specific option syntax; without the flag the server refuses the clause before the engine
   sees it, and every table takes the session defaults with no way to say otherwise. */
#define TDB_HTON_FLAGS HTON_SUPPORTS_ENGINE_ATTRIBUTE

/* ******************** plugin declaration ******************** */

/* the descriptor the server registers the engine through.  the initialisers are positional, so the
   block is assembled from these names rather than spelled out at the declaration: MySQL inserts a
   check-uninstall callback between init and deinit, and ends with a reserved slot and a flags word.

   MySQL's descriptor has no field for a version string or a maturity level.  neither is lost -- the
   engine publishes both as status variables instead. */
#define TDB_DECLARE_PLUGIN(name) mysql_declare_plugin(name)
#define TDB_PLUGIN_LIFECYCLE(init_fn, deinit_fn) init_fn, nullptr, deinit_fn
#define TDB_PLUGIN_TAIL nullptr, 0
#define TDB_DECLARE_PLUGIN_END mysql_declare_plugin_end

/* ******************** status variables ******************** */

/* MySQL's status-variable record carries a scope field and a char* value:

     SHOW_VAR { const char *name; char *value; enum_mysql_show_type type;
                enum_mysql_show_scope scope; }

   which means every one of the engine's 65 status entries carries a fourth initialiser.
   TDB_SHOW_VAR_ENTRY supplies the shape from one call site so the table is written once.

   the callback takes three arguments and a char* scratch buffer:

     int (*)(THD *, SHOW_VAR *, char *)

   MySQL publishes no SHOW_FUNC_ENTRY helper, so function-valued entries are built longhand, and
   TDB_SHOW_FUNC_SIG supplies the parameter list so each body names it once. */
#define TDB_SHOW_VAR_TYPE SHOW_VAR
#define TDB_SHOW_VAR_ENTRY(name, value, type)              \
    {                                                      \
        (name), (char *)(value), (type), SHOW_SCOPE_GLOBAL \
    }
#define TDB_SHOW_FUNC_ENTRY(name, fn)                      \
    {                                                      \
        (name), (char *)(fn), SHOW_FUNC, SHOW_SCOPE_GLOBAL \
    }
#define TDB_SHOW_VAR_END                               \
    {                                                  \
        NullS, NullS, SHOW_LONGLONG, SHOW_SCOPE_GLOBAL \
    }
#define TDB_SHOW_FUNC_SIG(var_arg, buf_arg) (MYSQL_THD, SHOW_VAR * var_arg, char *buf_arg)

/* ******************** per-table options ******************** */

/* MySQL has no handlerton::table_options, no field_options, and no mechanism for an engine to
   extend CREATE TABLE grammar, so the engine's 15 table options have no grammar of their own and
   arrive through ENGINE_ATTRIBUTE instead, described at the accessor below. */

/* the per-table option accessor.  ENGINE_ATTRIBUTE is a JSON object
   the server stores in the data dictionary verbatim and hands back on TABLE_SHARE without looking
   inside it -- so the option names, their types and their ranges are the engine's to define and to
   check, and src/handler/ha_tidesdb_table_options.cc does both over the reader in
   src/core/table_options.  a table names its options the same way, in the same order, with the
   same meanings:

     CREATE TABLE t (...) ENGINE=TidesDB
       ENGINE_ATTRIBUTE='{"compression": "ZSTD", "ttl": 3600, "encrypted": true}'

   what MySQL does not offer is a per-column equivalent the engine can reach: dd::Column carries an
   engine attribute, but nothing puts it on the Field the row codec holds, so the per-row TTL source
   column has no MySQL spelling and a table there takes the table-level TTL only. */
#define TDB_TABLE_OPTIONS(tbl) tdb_table_options(tbl)

/* ******************** index column length ******************** */

/* the longest a single indexed column may be.  the server defaults to 255 bytes, a limit inherited
   from index formats that hold a key part inside a fixed-width page; this engine has no such
   format.  a key part goes into the sort-key encoding as its bytes and costs the same whatever it
   holds, so the engine's real limit is the whole-key one max_supported_key_length() already
   reports.

   the default is not merely conservative, it changes results: a server that truncates a longer
   column to a 255-byte prefix builds an index that cannot tell apart rows agreeing in their first
   255 bytes, and one that refuses instead rejects ordinary tables outright -- a VARCHAR(100) is
   400 bytes in a four-byte character set, which a default character set of utf8mb4 makes the
   common case rather than an unusual one.

   MySQL passes the CREATE options, since an engine may vary the limit by row format; this engine
   does not, so the answer is the same whatever it is asked with. */
#define TDB_MAX_KEY_PART_LENGTH_OVERRIDE                                \
    uint max_supported_key_part_length(HA_CREATE_INFO *) const override \
    {                                                                   \
        return MAX_KEY_LENGTH;                                          \
    }

/* ******************** keeping the module mapped ******************** */

/* The one platform branch in this file, and rule 6 is why it is here rather than at the call.
 *
 * The linked TidesDB library keeps one thread-local key for the life of the database.  On Windows
 * its shim allocates that key with FlsAlloc, which takes a callback to run when a thread exits,
 * and the library is linked into this plugin rather than loaded beside it -- only tidesdb.lib is
 * installed, there is no DLL -- so that callback's code lives inside ha_tidesdb.dll.  Nothing
 * frees the key, which the shim says plainly: the key "lives for the process".
 *
 * That holds for anything that keeps the library loaded until it exits.  MySQL does not.
 * plugin_shutdown (sql_plugin.cc) reaches dlclose, which my_sharedlib.h defines as FreeLibrary, and
 * only afterwards does mysqld_exit call exit().  So the module is gone before the C runtime walks
 * the fiber-local list at ExitProcess, and RtlProcessFlsData calls an address that is no longer
 * mapped: mysqld dies with an access violation after every statement has already run.  The suite
 * shows it exactly -- of the nine tests that restart the server, the eight that shut it down
 * cleanly failed and crash_recovery, which kills it instead and so never runs exit(), passed.
 *
 * Pinning the module leaves it mapped for the life of the process, which is the assumption the
 * library already makes about itself, and the callback stays valid.  UNINSTALL PLUGIN still
 * unloads the engine as far as the server is concerned; only the code stays resident.
 *
 * It is the plugin's to fix because the key is file-static in the library and cannot be reached
 * from here.  Were the library to free its key on close, this could go. */
#ifdef _WIN32
#include <windows.h>
#define TDB_PIN_PLUGIN_MODULE()                                                          \
    do                                                                                   \
    {                                                                                    \
        HMODULE tdb_self_ = NULL;                                                        \
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |                          \
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,              \
                                reinterpret_cast<LPCWSTR>(&tidesdb_hton), &tdb_self_))   \
            sql_print_warning(                                                           \
                "[TIDESDB] could not pin the plugin module (error %lu); the server may " \
                "fault while exiting",                                                   \
                GetLastError());                                                         \
    } while (0)
#else
#define TDB_PIN_PLUGIN_MODULE() ((void)0)
#endif

#endif
