# Building Firebird from Source

This document describes how to compile Firebird 3, 4, and 5 from source on
Debian 13 (trixie) for use with the sysbench Firebird driver.

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
| Firebird 4 | 3054 | `/opt/firebird4/firebird.conf` |
| Firebird 3 | 3053 | `/opt/firebird3/firebird.conf` |

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
```

### Verify

```bash
ss -tlnp | grep -E "3050|3053|3054"
```

### Create test databases

```bash
# Firebird 5
isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost:/tmp/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"

# Firebird 4
FIREBIRD=/opt/firebird4 LD_LIBRARY_PATH=/opt/firebird4/lib \
  /opt/firebird4/bin/isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost/3054:/tmp/sbtest_fb4.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"

# Firebird 3
FIREBIRD=/opt/firebird3 LD_LIBRARY_PATH=/opt/firebird3/lib \
  /opt/firebird3/bin/isql -user SYSDBA -password masterkey \
  -i /dev/stdin <<< "CREATE DATABASE 'localhost/3053:/tmp/sbtest_fb3.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"
```

### Run benchmarks against each version

```bash
# Firebird 5 (default)
./run_benchmarks.sh firebird

# Firebird 4
FIREBIRD_DB=localhost/3054:/tmp/sbtest_fb4.fdb ./run_benchmarks.sh firebird

# Firebird 3
FIREBIRD_DB=localhost/3053:/tmp/sbtest_fb3.fdb ./run_benchmarks.sh firebird
```

## Firebird 2.5

Firebird 2.5 (tag `R2_5_9`) is not supported for building on modern systems.
The codebase uses C++03 with constructs removed in C++17, and bundles ancient
ICU 3.0 which is incompatible with modern toolchains. The ISC API is
binary-compatible across all versions, so the sysbench driver compiled against
FB5's libfbclient can connect to a FB 2.5 server if one is available.
