# Building Firebird from Source

This document describes how to compile Firebird 3, 4, 5 and 6 from source on
Debian 13 (trixie) for use with the sysbench Firebird driver.

The OO API driver on this branch needs **Firebird 5 headers or newer** to
build (`fb_c_api.h` first shipped in FB5) and refuses an FB3 client library
at runtime. Build FB3 only if you intend to use the `firebird-isc` branch.
FB4 is useful as a *server*; its client library also works at runtime, it
just can't be built against.

## Prerequisites

```bash
sudo apt-get install -y \
  build-essential autoconf automake libtool libtool-bin pkg-config \
  libicu-dev zlib1g-dev libedit-dev libncurses-dev unzip clang
```

## Clone the Repository

All versions are in the same repository:

```bash
git clone https://github.com/FirebirdSQL/firebird.git firebird-source
cd firebird-source
```

## Firebird 5.0.4

```bash
git checkout v5.0.4

./autogen.sh --prefix=/opt/firebird \
  --with-system-editline --with-system-icu \
  --with-builtin-tommath --with-builtin-tomcrypt

make -j$(nproc)
sudo make install

echo "/opt/firebird/lib" | sudo tee /etc/ld.so.conf.d/firebird.conf
sudo ldconfig
```

Verify: `/opt/firebird/bin/firebird -z`

## Firebird 6.0 (trunk)

Firebird 6 has no release tag yet; build the `master` branch and record the
build number it reports, since trunk moves.

```bash
git checkout master

./autogen.sh --prefix=/opt/firebird6 \
  --with-system-editline --with-system-icu \
  --with-builtin-tommath --with-builtin-tomcrypt

make -j$(nproc)
sudo make install
```

Verify: `/opt/firebird6/bin/firebird -z` — the measurements in
`BENCHMARK_FIREBIRD_6_vs_5.md` were taken against
`LI-T6.0.0.2012 Firebird 6.0 Initial`.

Deliberately **not** registered in `/etc/ld.so.conf.d`, unlike the FB4 and
FB5 installs. The driver's default `--firebird-client=libfbclient.so` goes
through the ldconfig cache, and the cross-version benchmarks depend on that
resolving to the FB5 client; adding more entries changes which library wins.
Check what the default resolves to, and name the FB6 client explicitly when
you want it:

```bash
ldconfig -p | grep libfbclient.so        # first hit is what the default gets
--firebird-client=/opt/firebird6/lib/libfbclient.so
```

## Firebird 4.0.7

```bash
git checkout v4.0.7

./autogen.sh --prefix=/opt/firebird4 \
  --with-system-editline --with-system-icu \
  --with-builtin-tommath --with-builtin-tomcrypt

make -j$(nproc)
sudo make install

echo "/opt/firebird4/lib" | sudo tee /etc/ld.so.conf.d/firebird4.conf
sudo ldconfig
```

Verify: `/opt/firebird4/bin/firebird -z`

If GCC 14 causes build errors, add relaxed warning flags:

```bash
CFLAGS="-Wno-error" CXXFLAGS="-Wno-error" ./autogen.sh --prefix=/opt/firebird4 ...
```

## Firebird 3.0.14

Only needed for the `firebird-isc` branch. The OO API driver rejects FB3's
client library at startup (`IUtil` vtable v2), and FB3 does not install
`fb_c_api.h`, so it cannot be built against either.

Firebird 3.0 requires **clang** — the codebase uses C++03 which is
incompatible with GCC 14's defaults.

```bash
git checkout v3.0.14

CC=clang CXX=clang++ ./autogen.sh --prefix=/opt/firebird3 \
  --with-system-editline --with-system-icu \
  --with-builtin-tommath

make -j$(nproc)
sudo make install

echo "/opt/firebird3/lib" | sudo tee /etc/ld.so.conf.d/firebird3.conf
sudo ldconfig
```

Verify: `/opt/firebird3/bin/firebird -z`

**Note:** Stop any running Firebird server before `make install` — the
installer checks for running instances:

```bash
sudo killall fbguard firebird 2>/dev/null
```

## Running Multiple Versions Simultaneously

Each server needs its own port and must know its root directory.

### Configure ports

Edit each `firebird.conf`:

| Version | Port | Config file |
|---|---|---|
| Firebird 5 | 3050 (default) | `/opt/firebird/firebird.conf` |
| Firebird 3 | 3053 | `/opt/firebird3/firebird.conf` |
| Firebird 4 | 3054 | `/opt/firebird4/firebird.conf` |
| Firebird 6 | 3056 | `/opt/firebird6/firebird.conf` |

Set `RemoteServicePort` in each config:

```
RemoteServicePort = 3054
```

For FB3 and FB4, also add `SecurityDatabase` pointing to the correct file:

```
SecurityDatabase = /opt/firebird4/security4.fdb
```

### Set SYSDBA password

For fresh installations, SYSDBA may not have a password. Set it via embedded
isql:

```bash
# Firebird 5
/opt/firebird/bin/isql -user SYSDBA /opt/firebird/security5.fdb \
  -i /dev/stdin <<< "CREATE USER SYSDBA PASSWORD 'masterkey' USING PLUGIN Srp; COMMIT; QUIT;"

# Firebird 4
FIREBIRD=/opt/firebird4 LD_LIBRARY_PATH=/opt/firebird4/lib \
  /opt/firebird4/bin/isql -user SYSDBA /opt/firebird4/security4.fdb \
  -i /dev/stdin <<< "CREATE USER SYSDBA PASSWORD 'masterkey' USING PLUGIN Srp; COMMIT; QUIT;"

# Firebird 3
FIREBIRD=/opt/firebird3 LD_LIBRARY_PATH=/opt/firebird3/lib \
  /opt/firebird3/bin/isql -user SYSDBA /opt/firebird3/security3.fdb \
  -i /dev/stdin <<< "CREATE USER SYSDBA PASSWORD 'masterkey' USING PLUGIN Srp; COMMIT; QUIT;"

# Firebird 6
FIREBIRD=/opt/firebird6 LD_LIBRARY_PATH=/opt/firebird6/lib \
  /opt/firebird6/bin/isql -user SYSDBA /opt/firebird6/security6.fdb \
  -i /dev/stdin <<< "CREATE USER SYSDBA PASSWORD 'masterkey' USING PLUGIN Srp; COMMIT; QUIT;"
```

### Start servers

Each server needs `FIREBIRD` and `LD_LIBRARY_PATH` set to prevent it from
loading the wrong libraries:

```bash
# Firebird 5 (default paths, no env needed)
sudo /opt/firebird/bin/fbguard -daemon -forever

# Firebird 4
sudo bash -c 'FIREBIRD=/opt/firebird4 LD_LIBRARY_PATH=/opt/firebird4/lib /opt/firebird4/bin/fbguard -daemon -forever'

# Firebird 3
sudo bash -c 'FIREBIRD=/opt/firebird3 LD_LIBRARY_PATH=/opt/firebird3/lib /opt/firebird3/bin/fbguard -daemon -forever'

# Firebird 6
sudo bash -c 'FIREBIRD=/opt/firebird6 LD_LIBRARY_PATH=/opt/firebird6/lib /opt/firebird6/bin/fbguard -daemon -forever'
```

Each `fbguard` must be started from its own installation — a guardian from
one version happily supervises another version's `firebird` binary if the
environment is wrong, which makes the running server hard to identify later.
Confirm what you actually have with:

```bash
ps -eo user,pid,ppid,args | grep -E 'fbguard|firebird'
```

### Verify

```bash
ss -tlnp | grep -E "305[0-9]"
```

A listening socket alone does not prove which version answers on it — a
running server opens auxiliary sockets on nearby ports, and under WSL2
mirrored networking, Windows-side servers appear in this list too. Check by
attaching:

```bash
echo "SELECT rdb\$get_context('SYSTEM','ENGINE_VERSION') FROM rdb\$database; QUIT;" \
  | /opt/firebird/bin/isql -user SYSDBA -password masterkey \
    localhost/3056:/var/lib/firebird6/sbtest.fdb
```

### Create test databases

Put them **on disk, not on tmpfs**. `/tmp` is tmpfs on this setup, and an
in-memory Firebird database compared against on-disk MariaDB/PostgreSQL was
the single largest source of unfairness in the earlier benchmark rounds. The
published results use `/var/lib/firebird{,4,6}/sbtest.fdb`, on the same
physical disk and mount options as the other engines' data directories.

```bash
# Firebird 5
isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost:/var/lib/firebird/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"

# Firebird 4
FIREBIRD=/opt/firebird4 LD_LIBRARY_PATH=/opt/firebird4/lib \
  /opt/firebird4/bin/isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost/3054:/var/lib/firebird4/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"

# Firebird 6
FIREBIRD=/opt/firebird6 LD_LIBRARY_PATH=/opt/firebird6/lib \
  /opt/firebird6/bin/isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost/3056:/var/lib/firebird6/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"
```

### Match the buffer cache

For a fair comparison, give every engine ~128 MB of cache. With an 8 KB page
size that is 16,384 pages — either globally in `firebird.conf`
(`DefaultDbCachePages = 16384`) or per database in `databases.conf`:

```
sbtest = /var/lib/firebird/sbtest.fdb
{
    DefaultDBCachePages = 16384
    LockMemSize = 10M
    TempCacheLimit = 128M
}
```

### Run benchmarks against each version

```bash
# Firebird 5 (default)
./run_benchmarks.sh firebird

# Firebird 4 server (FB5 client by default; an FB4 client works too)
FIREBIRD_DB=localhost/3054:/var/lib/firebird4/sbtest.fdb ./run_benchmarks.sh firebird

# Firebird 6 server, FB5 client
FIREBIRD_DB=localhost/3056:/var/lib/firebird6/sbtest.fdb ./run_benchmarks.sh firebird
```

Firebird 3 cannot be benchmarked from this branch — see the note in its
build section above.

## Firebird 2.5

Firebird 2.5 (tag `R2_5_9`) is not supported for building on modern systems.
The codebase uses C++03 with constructs removed in C++17, and bundles ancient
ICU 3.0 which is incompatible with modern toolchains.

The ISC API is binary-compatible across all versions, so the driver on the
`firebird-isc` branch can talk to a FB 2.5 server if one is available. The OO
API driver on this branch cannot: FB 2.5 predates the OO API entirely.
