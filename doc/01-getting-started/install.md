---
title: Installing TideSQL
description: Building the plugin against a server source tree, loading it, and the one linkage detail that decides whether it loads at all.
---

# Installing TideSQL

TideSQL is a loadable plugin — `ha_tidesdb.so` on Linux and macOS, `ha_tidesdb.dll` on Windows. It
links the TidesDB library, so that has to be installed first: the plugin's build looks for
`libtidesdb` and fails configuration without it.

Everything below names the `.so`. On Windows the file is the `.dll` and the rest reads the same.

## The quick way: install.sh

`install.sh` in the repository root does the whole thing — dependencies, the TidesDB library, a
MySQL server built with the plugin in it, and an initialised data directory with a `my.cnf` that
already loads the engine:

```bash
./install.sh
```

It detects the platform and installs dependencies through whichever package manager belongs to it:
`apt`, `dnf` or `pacman` on Linux, Homebrew on macOS, vcpkg on Windows under MSYS2 or Git Bash.
Then it builds and installs `libtidesdb`, clones the server, copies the engine into `storage/`,
copies the test suites into `mysql-test/suite/`, builds, installs, and prints the commands to start
the server and run the tests.

The flags worth knowing:

| Flag | What it does |
|------|--------------|
| `--tidesdb-version VERSION` | TidesDB release tag; defaults to the latest on GitHub |
| `--mysql-version VERSION` | MySQL branch or tag; defaults to the latest on GitHub |
| `--tidesdb-prefix DIR`, `--mysql-prefix DIR` | Where each is installed |
| `--build-dir DIR` | Working directory for the build |
| `--jobs N` | Parallel build jobs; auto-detected otherwise |
| `--skip-deps`, `--skip-tidesdb` | Skip dependency installation, or the library build if it is already installed |
| `--skip-engines ENGINES` | Comma-separated engines to leave out of the server; `--list-engines` shows what can be skipped |
| `--rebuild-plugin` | Rebuild only the plugin against an existing server build, for a fast edit-build-test cycle |
| `--allocator NAME` | Allocator for `libtidesdb`: `system` (default), `jemalloc`, `mimalloc` or `tcmalloc`. See the loading note below |
| `--s3` | Build the library's S3 object-store connector; needs libcurl |
| `--pgo` | Three-phase profile-guided build: instrument, train on the test suite, rebuild optimised |

```bash
./install.sh --mysql-version mysql-9.7.0 --jobs 8
./install.sh --skip-deps --skip-tidesdb        # rebuild against what is already installed
./install.sh --rebuild-plugin                  # just the plugin, after an edit
```

`--rebuild-plugin` needs a full run to have happened first; it reuses that build tree rather than
configuring a new one. It does not rebuild `libtidesdb`, so a changed `--allocator` or `--s3` needs
a full run to take effect.

The rest of this page is the manual path, which is what to read if you are building against a server
tree you already have.

## Building the plugin

The plugin builds as part of a server source tree, using the server's own plugin machinery. Put the
plugin sources under the tree's `storage/` directory and configure the server as you normally would:

```bash
# 1. the TidesDB library, installed where the plugin's build can find it
git clone https://github.com/tidesdb/tidesdb.git
cmake -S tidesdb -B build-tidesdb -DCMAKE_BUILD_TYPE=Release
cmake --build build-tidesdb --parallel && sudo cmake --install build-tidesdb

# 2. the plugin, inside the server tree
git clone https://github.com/tidesdb/tidesql.git
cp -r tidesql/tidesdb /path/to/mysql-server/storage/tidesdb

# 3. the test suites, where MySQL looks for a suite
cp -r tidesql/tidesdb/mysql-test/tidesdb     /path/to/mysql-server/mysql-test/suite/tidesdb
cp -r tidesql/tidesdb/mysql-test/tidesdb_rpl /path/to/mysql-server/mysql-test/suite/tidesdb_rpl

# 4. configure and build the server; the plugin is the `tidesdb` target
cmake -S /path/to/mysql-server -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_UNIT_TESTS=OFF
cmake --build build --target tidesdb --parallel
```

Step 3 is easy to skip and it is not optional. MySQL discovers a test suite under
`mysql-test/suite`, and nowhere else — it does not look inside a storage engine's own directory, so
the suites shipped in `tidesdb/mysql-test/` are invisible until they are copied. `mtr` then finds
them by name:

```bash
cd build/mysql-test
./mtr --suite=tidesdb --parallel=4
```

On Windows that needs one more flag. With no `--plugin-dir` the server looks in
`<basedir>/lib/plugin`, and on a build tree that directory only exists on Unix — MySQL's own
`CMakeLists.txt` links it to `plugin_output_directory` under `IF(UNIX AND BUILD_IS_SINGLE_CONFIG)`
and does nothing otherwise. Visual Studio also writes one directory deeper, under the configuration
name. So point the server at the plugins, MySQL's own components included:

```bash
perl mysql-test-run.pl --suite=tidesdb --parallel=4 \
  --mysqld=--plugin-dir=C:/path/to/build/plugin_output_directory/RelWithDebInfo
```

`libtidesdb.dll` and the compression DLLs it links have to be on `PATH` as well, or the plugin
fails to load with `errno: 126` even though the file is there — on Windows that error means either
the library or something it depends on could not be found.

That leaves the plugin in the build's plugin output directory. Copy it into the server's
`plugin_dir`, or point `plugin_dir` at the build output while developing. Visual Studio is a
multi-configuration generator, so on Windows the file is one level deeper, under the configuration
name — `plugin_output_directory/RelWithDebInfo/ha_tidesdb.dll`.

The plugin declares itself with the server's `MYSQL_ADD_PLUGIN` macro as `MODULE_ONLY`, so it is
always a loadable module and never linked into the server binary.

## Loading it

At startup, from `my.cnf`:

```ini
[mysqld]
plugin-load-add=ha_tidesdb.so
```

or into a running server:

```sql
INSTALL PLUGIN TidesDB SONAME 'ha_tidesdb.so';
```

Once loaded it appears in `SHOW ENGINES`:

```
      ENGINE: TidesDB
     SUPPORT: YES
     COMMENT: LSM B+tree engine with ACID transactions, MVCC concurrency, secondary, spatial
              and full-text indexes, and encryption
TRANSACTIONS: YES
          XA: YES
  SAVEPOINTS: YES
```

and reports its version through the plugin table and its own status variables:

```sql
SELECT PLUGIN_VERSION FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'TidesDB';   -- 2.0
SHOW STATUS LIKE 'tidesdb_version%';   -- tidesdb_version 2.0.0, tidesdb_version_hex 131072
```

## The allocator, and why the plugin may fail to load

This is the detail worth reading before the first install, because it decides whether the plugin
loads at all.

TidesDB can be built against a non-default allocator. `jemalloc`, `mimalloc` and `tcmalloc` place
their thread-local state in the initial-exec TLS model, which needs its space reserved when the
program starts. A plugin is loaded late, with `dlopen`, well after startup, so when `libtidesdb.so`
is linked against one of those the loader has no room left to reserve and the plugin does not load:

```
[ERROR] [MY-010901] [Server] Can't open shared library 'ha_tidesdb.so'
    (errno: 0 /lib/x86_64-linux-gnu/libjemalloc.so.2: cannot allocate memory in static TLS block).
[ERROR] [MY-010736] [Server] Couldn't load plugin named 'ha_tidesdb.so' with soname 'ha_tidesdb.so'.
```

Every statement then fails with `ERROR 1286 (42000): Unknown storage engine 'TidesDB'`, which is the
symptom you are likely to see first.

The fix is to put the allocator in the process image at startup, so its TLS is reserved up front:

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 mysqld --defaults-file=/etc/my.cnf
```

Under systemd, set it in the unit rather than the shell:

```ini
[Service]
Environment=LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2
```

A library built with the default system allocator has no such requirement and loads with no
preload. Check what a given build linked against with:

```bash
ldd /usr/local/lib/libtidesdb.so | grep -E 'jemalloc|mimalloc|tcmalloc'
```

This is a property of how the library was built, not of the plugin, so rebuilding the plugin alone
does not change it.

## Where the data lives

TidesDB data files live in a directory of their own rather than among the server's tablespaces,
`tidesdb_data` beside the server's data directory by default. `tidesdb_data_home_dir` sets it
explicitly at startup. See [System Variables](/reference/system-variables).
