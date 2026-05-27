# Development Environment Setup

This document describes the machine used for development and testing of the
Firebird sysbench driver. This is a development/testing environment, not a
production benchmark setup.

## Machine

- **Platform:** Windows 11 + WSL2
- **OS:** Debian 13 (trixie)
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 x86_64
- **Note:** WSL2 introduces I/O overhead. Benchmark results are relative (ratios
  between databases matter), not absolute throughput numbers.

## Installed Packages

### Build toolchain

```bash
sudo apt-get install -y \
  build-essential gcc g++ make git \
  automake autoconf libtool libtool-bin pkg-config cmake \
  python3
```

### sysbench dependencies

```bash
sudo apt-get install -y \
  libaio-dev libssl-dev zlib1g-dev \
  luajit libluajit-5.1-dev
```

### Database client libraries

```bash
# MariaDB (MySQL-compatible)
sudo apt-get install -y libmariadb-dev-compat libmariadb-dev

# PostgreSQL
sudo apt-get install -y libpq-dev

# Firebird — built from source (see docs/BUILDING_FIREBIRD.md)
```

### Database servers

```bash
# MariaDB server
sudo apt-get install -y mariadb-server

# PostgreSQL server
sudo apt-get install -y postgresql

# Firebird — built from source (see docs/BUILDING_FIREBIRD.md)
```

### Development tools

```bash
sudo apt-get install -y \
  gdb valgrind curl wget vim unzip openssh-client clang
```

### GitHub CLI

```bash
sudo mkdir -p /etc/apt/keyrings
sudo curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
  -o /etc/apt/keyrings/githubcli-archive-keyring.gpg
sudo chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg
sudo bash -c 'echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" > /etc/apt/sources.list.d/github-cli.list'
sudo apt-get update
sudo apt-get install -y gh
```

## Database Server Setup

### MariaDB

```bash
sudo service mariadb start
sudo mariadb -u root -pmasterkey -e "CREATE USER 'sbtest'@'localhost' IDENTIFIED BY 'sbtest'; CREATE DATABASE sbtest; GRANT ALL ON sbtest.* TO 'sbtest'@'localhost'; FLUSH PRIVILEGES;"
```

### PostgreSQL

```bash
sudo service postgresql start
sudo su -c "psql -c \"ALTER USER postgres PASSWORD 'masterkey';\"" postgres
sudo su -c "psql -c \"CREATE USER sbtest PASSWORD 'sbtest';\"" postgres
sudo su -c "psql -c \"CREATE DATABASE sbtest OWNER sbtest;\"" postgres
```

If `peer` auth blocks password login:

```bash
sudo sed -i 's/local.*all.*all.*peer/local   all             all                                     md5/' /etc/postgresql/17/main/pg_hba.conf
sudo service postgresql restart
```

### Firebird

See [docs/BUILDING_FIREBIRD.md](BUILDING_FIREBIRD.md) for building from source.
After installation:

```bash
# Start server
sudo /opt/firebird/bin/fbguard -daemon -forever

# Create test database
isql -user SYSDBA -password masterkey <<< "CREATE DATABASE 'localhost:/tmp/sbtest.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8; QUIT;"
```

## Tool Versions (as of 2026-05-27)

| Tool | Version |
|---|---|
| gcc / g++ | 14.2.0 (Debian) |
| clang | 19.1.7 (for Firebird 3.0 build) |
| make | 4.4.1 |
| git | 2.47.3 |
| cmake | 3.31.6 |
| automake | 1.17 |
| autoconf | 2.72 |
| libtoolize | 2.5.4 |
| pkg-config | 1.8.1 |
| python3 | 3.13.5 |
| gh | 2.92.0 |
| MariaDB | 11.8.6 |
| PostgreSQL | 17.10 |
| Firebird 5 | 5.0.4 |
| Firebird 4 | 4.0.7 |
| Firebird 3 | 3.0.14 |
