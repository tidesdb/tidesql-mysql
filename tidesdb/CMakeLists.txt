# Copyright (c) 2026 TidesDB Corp.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# Optional Plugin-level ASAN + UBSAN
#
# You enable with cmake -DTIDESDB_WITH_ASAN=ON -DTIDESDB_WITH_UBSAN=ON ..
# This instruments only the TidesDB plugin .so, not the whole server, though its good to build the server with sanitizers too.
# At runtime, LD_PRELOAD the ASAN runtime for shared-lib mode:
#   export LD_PRELOAD=$(gcc -print-file-name=libasan.so)
#   export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
OPTION(TIDESDB_WITH_ASAN "Enable AddressSanitizer for TidesDB plugin" OFF)
OPTION(TIDESDB_WITH_UBSAN "Enable UndefinedBehaviorSanitizer for TidesDB plugin" OFF)

SET(TIDESDB_SANITIZER_COMPILE_FLAGS "")
SET(TIDESDB_SANITIZER_LINK_FLAGS "")
IF(TIDESDB_WITH_ASAN)
  LIST(APPEND TIDESDB_SANITIZER_COMPILE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  LIST(APPEND TIDESDB_SANITIZER_LINK_FLAGS -fsanitize=address)
ENDIF()
IF(TIDESDB_WITH_UBSAN)
  LIST(APPEND TIDESDB_SANITIZER_COMPILE_FLAGS -fsanitize=undefined -fno-sanitize=alignment)
  LIST(APPEND TIDESDB_SANITIZER_LINK_FLAGS -fsanitize=undefined)
ENDIF()

FIND_LIBRARY(TIDESDB_LIB
        NAMES tidesdb
        HINTS
        ENV TIDESDB_ROOT
        ENV LIBRARY_PATH
        PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/local/lib
        /opt/homebrew/lib
        "C:/Program Files/tidesdb/lib"
        "C:/Program Files (x86)/tidesdb/lib"
        PATH_SUFFIXES lib
)

# The compression backends all resolve to full paths through FIND_LIBRARY.
# pkg-config cannot be used here because it returns bare names like "zstd", which
# collide with a same-named MODULE target in the other server tree this plugin builds
# against, leaving the linker to resolve "zstd" to that target instead of the library.
# All three live in the same system and vcpkg locations, so one shared path
# list drives every search.
SET(TIDESDB_COMPRESSION_PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/homebrew/lib
        "C:/vcpkg/installed/x64-windows/lib"
        "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib")

FIND_LIBRARY(ZSTD_LIBRARY
        NAMES zstd zstd_static
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS ${TIDESDB_COMPRESSION_PATHS})

FIND_LIBRARY(LZ4_LIBRARY
        NAMES lz4
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS ${TIDESDB_COMPRESSION_PATHS})

FIND_LIBRARY(SNAPPY_LIBRARY
        NAMES snappy
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS ${TIDESDB_COMPRESSION_PATHS})

# We find include directories
# and look for tidesdb/db.h to get the parent include directory
# The plugin primarily uses #include <tidesdb/db.h> so we need the parent of the tidesdb folder
FIND_PATH(TIDESDB_INCLUDE_DIR
        NAMES tidesdb/db.h
        HINTS
        ENV TIDESDB_ROOT
        ENV CPATH
        PATHS
        /usr/local/include
        /usr/include
        /opt/local/include
        /opt/homebrew/include
        "C:/tidesdb/include"
        "C:/Program Files/tidesdb/include"
        "C:/Program Files (x86)/tidesdb/include"
        PATH_SUFFIXES include
)

# We validate required dependencies
IF(NOT TIDESDB_LIB)
  MESSAGE(STATUS "TidesDB: library not found - plugin will not be built. Install TidesDB or set TIDESDB_ROOT.")
  RETURN()
ENDIF()

IF(NOT TIDESDB_INCLUDE_DIR)
  MESSAGE(STATUS "TidesDB: tidesdb/db.h header not found - plugin will not be built. Install TidesDB headers or set TIDESDB_ROOT.")
  RETURN()
ENDIF()

# The library major this plugin major is paired with.  A TidesDB major opens a new on-disk format
# line, so a plugin built against a different one would read and write a format it does not claim to
# support -- see VERSIONING.md.  Checked here rather than left to the linker, which would bind an
# incompatible libtidesdb happily: the soname carries the major, so the mismatch surfaces at load
# time as a missing library, or at run time as a format the engine misreads.
SET(TIDESQL_REQUIRED_TIDESDB_MAJOR 10)

IF(NOT EXISTS "${TIDESDB_INCLUDE_DIR}/tidesdb/tidesdb_version.h")
  MESSAGE(FATAL_ERROR
          "TidesDB: ${TIDESDB_INCLUDE_DIR}/tidesdb/tidesdb_version.h is missing, so the library "
          "version cannot be read.  TideSQL requires TidesDB v${TIDESQL_REQUIRED_TIDESDB_MAJOR}.x; "
          "a release that old predates it.  Install a current TidesDB or point TIDESDB_ROOT at one.")
ENDIF()

FILE(STRINGS "${TIDESDB_INCLUDE_DIR}/tidesdb/tidesdb_version.h" _tdb_version_line
     REGEX "^#define[ \t]+TIDESDB_VERSION[ \t]+\"[0-9]+\\.[0-9]+\\.[0-9]+\"")
STRING(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1" TIDESDB_VERSION "${_tdb_version_line}")

IF(NOT TIDESDB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  MESSAGE(FATAL_ERROR
          "TidesDB: could not read TIDESDB_VERSION from "
          "${TIDESDB_INCLUDE_DIR}/tidesdb/tidesdb_version.h.")
ENDIF()

STRING(REGEX REPLACE "^([0-9]+)\\..*" "\\1" TIDESDB_VERSION_MAJOR "${TIDESDB_VERSION}")

IF(NOT TIDESDB_VERSION_MAJOR EQUAL TIDESQL_REQUIRED_TIDESDB_MAJOR)
  MESSAGE(FATAL_ERROR
          "TidesDB version mismatch: found v${TIDESDB_VERSION} at ${TIDESDB_INCLUDE_DIR}, but this "
          "TideSQL pairs with TidesDB v${TIDESQL_REQUIRED_TIDESDB_MAJOR}.x.  A different library "
          "major is a different on-disk format, so the two cannot be mixed.  Install TidesDB "
          "v${TIDESQL_REQUIRED_TIDESDB_MAJOR}.x, or use the TideSQL release that pairs with "
          "v${TIDESDB_VERSION_MAJOR}.x -- the pairing is recorded in VERSIONING.md.")
ENDIF()

MESSAGE(STATUS "TidesDB: v${TIDESDB_VERSION} (major ${TIDESDB_VERSION_MAJOR}, as required)")

INCLUDE_DIRECTORIES(${TIDESDB_INCLUDE_DIR})

# The compression backends are optional.  The library encodes with NONE when it is
# built without them, and a shared libtidesdb already carries its own zstd, lz4 and
# snappy in its needed-library list, so the plugin only has to link a backend when
# one is present, which matters for a statically linked libtidesdb built with that
# backend.  A missing backend is therefore not a reason to skip building the plugin.
SET(TIDESDB_LIBS ${TIDESDB_LIB})
IF(ZSTD_LIBRARY)
  LIST(APPEND TIDESDB_LIBS ${ZSTD_LIBRARY})
ENDIF()
IF(LZ4_LIBRARY)
  LIST(APPEND TIDESDB_LIBS ${LZ4_LIBRARY})
ENDIF()
IF(SNAPPY_LIBRARY)
  LIST(APPEND TIDESDB_LIBS ${SNAPPY_LIBRARY})
ENDIF()

# On Windows, we need pthreads library
IF(WIN32)
  FIND_LIBRARY(PTHREADS_LIBRARY
          NAMES pthreadVC3 pthreadVC2 pthread pthreads
          HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
          PATHS
          "C:/vcpkg/installed/x64-windows/lib"
          "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib"
  )
  IF(PTHREADS_LIBRARY)
    LIST(APPEND TIDESDB_LIBS ${PTHREADS_LIBRARY})
  ENDIF()
ENDIF()

# Server-free core modules (unit-tested standalone under test/), compiled into the plugin.
SET(TIDESDB_CORE_SOURCES
        src/core/spatial.cc
        src/core/fts_text.cc
        src/core/sort_key.cc
        src/core/row_format.cc
        src/core/rir.cc
        src/core/cf_name.cc
        src/core/fts_score.cc
        src/core/crypto_keyenc.cc
        src/core/cost_model.cc
        src/core/ddl_log_rec.cc
        src/core/table_options.cc)

# Server-coupled handler modules split out of the ha_tidesdb.cc monolith.  Each is a thin
# adapter over a src/core unit or a cohesive slice of the handler, kept under the 1000-line rule.
SET(TIDESDB_HANDLER_SOURCES
        src/handler/ha_tidesdb_spatial.cc
        src/handler/ha_tidesdb_fts.cc
        src/handler/ha_tidesdb_fts_search.cc
        src/handler/ha_tidesdb_row.cc
        src/handler/ha_tidesdb_keycodec.cc
        src/handler/ha_tidesdb_dml_update.cc
        src/handler/ha_tidesdb_dml.cc
        src/handler/ha_tidesdb_scan.cc
        src/handler/ha_tidesdb_index.cc
        src/handler/ha_tidesdb_optimizer.cc
        src/handler/ha_tidesdb_admin.cc
        src/handler/ha_tidesdb_ddl.cc
        src/handler/ha_tidesdb_lifecycle.cc
        src/handler/ha_tidesdb_open.cc
        src/handler/ha_tidesdb_read.cc
        src/handler/ha_tidesdb_txn.cc
        src/engine/ha_tidesdb_config.cc
        src/engine/ha_tidesdb_udf.cc
        src/engine/ha_tidesdb_status.cc)

# Optimizer cost.  The arithmetic lives in src/core/cost_model; this adapter presents it in the
# flat-double shape MySQL's cost methods ask for.
# Per-table storage options.  MySQL cannot parse engine-specific option syntax, so the options
# arrive as one ENGINE_ATTRIBUTE JSON object and this reads them back.
  LIST(APPEND TIDESDB_HANDLER_SOURCES src/handler/ha_tidesdb_table_options.cc)

# Row sampling and the emptiness question, which MySQL asks an engine for.
  LIST(APPEND TIDESDB_HANDLER_SOURCES src/handler/ha_tidesdb_sampling.cc)

  LIST(APPEND TIDESDB_HANDLER_SOURCES src/engine/ha_tidesdb_cost_double.cc)

# DDL timing.  MySQL resolves table definitions through a transactional data dictionary, so the
# engine defers creating and destroying column families until that resolves.
  LIST(APPEND TIDESDB_HANDLER_SOURCES src/handler/ha_tidesdb_ddl_deferred.cc)

# Data-at-rest key custody.  Two backends implement src/engine/ha_tidesdb_crypto.h and exactly one
# is compiled, so neither carries a preprocessor branch for the server it does not serve.
#
#   keyring  MySQL, whose keyring stores keys but supplies no cipher, so the engine assembles the
#            two-tier master-key / table-key arrangement over the keyring component services and
#            the server's own AES routines
#   service  a server publishing a versioned-key encryption service, which already implements the
#            interface and is delegated to directly
  LIST(APPEND TIDESDB_HANDLER_SOURCES src/engine/ha_tidesdb_crypto_keyring.cc)

# Subsystems with no MySQL counterpart.  Both are excluded from the source list rather than
# guarded internally, so neither file carries a preprocessor branch for a server it cannot run on.

# Where a foreign key's definition is read from at CREATE time, and where the parent's candidate
# key comes from.  One server hands the engine the parser's key list and reads the parent off disk;
# MySQL keeps both in its data dictionary.  Everything downstream is shared.
  LIST(APPEND TIDESDB_HANDLER_SOURCES src/handler/ha_tidesdb_fk_source_dd.cc)

# Foreign-key persistence and enforcement, compiled everywhere.
LIST(APPEND TIDESDB_HANDLER_SOURCES src/handler/ha_tidesdb_fk.cc)

# Define the storage engine using the server's MYSQL_ADD_PLUGIN macro.  The STORAGE_ENGINE,
# MODULE_ONLY and LINK_LIBRARIES keywords come from the server's own plugin machinery.
MYSQL_ADD_PLUGIN(tidesdb ha_tidesdb.cc ${TIDESDB_CORE_SOURCES} ${TIDESDB_HANDLER_SOURCES}
        STORAGE_ENGINE MODULE_ONLY LINK_LIBRARIES ${TIDESDB_LIBS})

# RULES.md documents @param for struct fields as well as for function parameters, and the
# server-free core headers follow it.  clang's -Wdocumentation only accepts @param on a comment
# attached to a function declaration, so it flags every struct doc block in those headers.  The
# convention is deliberate and written down, so the check is turned off for this target rather than
# several hundred doc comments rewritten to suit one compiler's reading of doxygen.
IF(TARGET tidesdb AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  TARGET_COMPILE_OPTIONS(tidesdb PRIVATE -Wno-documentation)
ENDIF()

# The split translation units under src/handler and src/engine reach the root headers
# (ha_tidesdb.h) and the src/core and src/handler headers by their path-qualified names, so
# the plugin source directory must be on the include path for every source, not just the ones
# that happen to sit at the root.
IF(TARGET tidesdb)
  TARGET_INCLUDE_DIRECTORIES(tidesdb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ENDIF()

# The engine includes the server headers by bare name -- "handler.h", "key.h", "sql_class.h".
# Some servers put sql/ on every plugin's include path so that resolves; MySQL puts the source
# root on the path instead and spells the same includes "sql/handler.h".  Adding the directory
# here keeps one spelling working on both, so no engine source carries a conditional include.
IF(TARGET tidesdb)
  TARGET_INCLUDE_DIRECTORIES(tidesdb PRIVATE ${CMAKE_SOURCE_DIR}/sql)

  # field.h reaches the data-dictionary column type, which forward-declares its serialised-
  # dictionary types through rapidjson.  The server locates that header itself and publishes the
  # path, so the plugin borrows it rather than guessing at a vendored location.
  IF(RAPIDJSON_INCLUDE_DIR)
    TARGET_INCLUDE_DIRECTORIES(tidesdb SYSTEM PRIVATE ${RAPIDJSON_INCLUDE_DIR})
  ENDIF()
ENDIF()

# We apply sanitizer flags to the plugin target only
IF(TIDESDB_SANITIZER_COMPILE_FLAGS)
  IF(TARGET tidesdb)
    TARGET_COMPILE_OPTIONS(tidesdb PRIVATE ${TIDESDB_SANITIZER_COMPILE_FLAGS})
    TARGET_LINK_OPTIONS(tidesdb PRIVATE ${TIDESDB_SANITIZER_LINK_FLAGS})
    MESSAGE(STATUS "TidesDB: Sanitizer compile flags: ${TIDESDB_SANITIZER_COMPILE_FLAGS}")
    MESSAGE(STATUS "TidesDB: Sanitizer link flags: ${TIDESDB_SANITIZER_LINK_FLAGS}")
  ENDIF()
ENDIF()

# Plugin-only -Wmaybe-uninitialized.  GCC reports may-uninit reads with
# considerably less noise than clang's -Wsometimes-uninitialized and is
# the canonical signal we lean on to catch uninitialized SHOW_FUNC
# fillers and missed branch initialization.  We always enable the
# warning on GCC; CI also flips TIDESDB_WERROR_UNINIT=ON so the warning
# is a hard error rather than something easy to overlook in a long
# build log.
OPTION(TIDESDB_WERROR_UNINIT "Promote -Wmaybe-uninitialized to a hard error in the plugin" OFF)
IF(TARGET tidesdb AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  TARGET_COMPILE_OPTIONS(tidesdb PRIVATE -Wmaybe-uninitialized)
  IF(TIDESDB_WERROR_UNINIT)
    TARGET_COMPILE_OPTIONS(tidesdb PRIVATE -Werror=maybe-uninitialized)
    MESSAGE(STATUS "TidesDB: -Werror=maybe-uninitialized enabled for plugin")
  ELSE()
    MESSAGE(STATUS "TidesDB: -Wmaybe-uninitialized enabled for plugin (warning only)")
  ENDIF()
ENDIF()
