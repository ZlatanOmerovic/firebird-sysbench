# Shared setup for run_benchmarks.sh and run_benchmarks_readonly.sh.
# Source it after SYSBENCH, SCRIPTS and DB_DRIVER are set.
#
# TRANSPORT=tcp (default): every DBMS through TCP on the loopback, so that
#   the results are comparable. The MySQL client without a host uses the
#   Unix socket, and that alone gives MariaDB about +25 %.
# TRANSPORT=local: MySQL through its Unix socket, PostgreSQL through its
#   socket directory, Firebird embedded (a database path without a host).

TRANSPORT="${TRANSPORT:-tcp}"
case "$TRANSPORT" in
  tcp|local) ;;
  *)
    echo "TRANSPORT must be tcp or local, not '$TRANSPORT'"
    exit 1
    ;;
esac

case "$DB_DRIVER" in
  firebird)
    if [ "$TRANSPORT" = tcp ]; then
      FB_DB_DEFAULT="${FIREBIRD_HOST:-127.0.0.1}:${FIREBIRD_PATH:-/tmp/sbtest.fdb}"
    else
      FB_DB_DEFAULT="${FIREBIRD_PATH:-/tmp/sbtest.fdb}"
    fi
    DB_ARGS="--firebird-db=${FIREBIRD_DB:-$FB_DB_DEFAULT} --firebird-user=${FIREBIRD_USER:-SYSDBA} --firebird-password=${FIREBIRD_PASSWORD:-masterkey}"
    # The embedded engine needs more stack than the sysbench default of 64K.
    if [ "$TRANSPORT" = local ]; then
      DB_ARGS="$DB_ARGS --thread-stack-size=2M"
    fi
    ;;
  mysql)
    if [ "$TRANSPORT" = tcp ]; then
      MY_CONN="--mysql-host=${MYSQL_HOST:-127.0.0.1}"
    elif [ -n "${MYSQL_SOCKET:-}" ]; then
      MY_CONN="--mysql-socket=$MYSQL_SOCKET"
    else
      MY_CONN="--mysql-host=localhost"  # the client library's default socket
    fi
    DB_ARGS="$MY_CONN --mysql-user=${MYSQL_USER:-sbtest} --mysql-password=${MYSQL_PASSWORD:-sbtest} --mysql-db=${MYSQL_DB:-sbtest}"
    ;;
  pgsql)
    if [ "$TRANSPORT" = tcp ]; then
      PG_HOST="${PGSQL_HOST:-127.0.0.1}"
    else
      PG_HOST="${PGSQL_SOCKET_DIR:-/var/run/postgresql}"
    fi
    DB_ARGS="--pgsql-host=$PG_HOST --pgsql-user=${PGSQL_USER:-sbtest} --pgsql-password=${PGSQL_PASSWORD:-sbtest} --pgsql-db=${PGSQL_DB:-sbtest}"
    ;;
  *)
    echo "Usage: $0 [firebird|mysql|pgsql]"
    exit 1
    ;;
esac

# Firebird: let the caller pick the client library, and remember which one
# will actually be loaded. The driver dlopens "libfbclient.so" through the
# ldconfig cache by default, so the version measured depends on the loader.
FB_CLIENT=""
if [ "$DB_DRIVER" = firebird ]; then
  if [ -n "${FIREBIRD_CLIENT:-}" ]; then
    DB_ARGS="$DB_ARGS --firebird-client=$FIREBIRD_CLIENT"
    FB_CLIENT="$FIREBIRD_CLIENT"
  else
    FB_CLIENT=$(ldconfig -p 2>/dev/null | awk '/libfbclient\.so \(/{print $NF; exit}')
    FB_CLIENT="${FB_CLIENT:-libfbclient.so (not in ldconfig cache)}"
  fi
fi

# Warm-up before each timed run, so that a run measures a warm cache.
WARMUP="${WARMUP:-10}"
# Firebird: keep the database open this many seconds after the last
# connection ends. With LINGER 0 the SuperServer drops its page cache
# between sysbench processes, and each run starts cold.
FIREBIRD_LINGER="${FIREBIRD_LINGER:-60}"
# prepare runs in one thread unless asked otherwise.
PREPARE_THREADS="${PREPARE_THREADS:-1}"

# Run one SQL statement through the sysbench driver and print the first
# column of each row. No DBMS client tools are necessary.
sb_sql() {
  local lua rc
  lua=$(mktemp /tmp/sb_sql_XXXXXX.lua)
  cat > "$lua" <<'EOF'
sysbench.cmdline.commands = {
  sql = { function()
    local con = sysbench.sql.driver():connect()
    local rs = con:query(os.getenv("SB_SQL"))
    if rs ~= nil and rs.nrows ~= nil then
      for i = 1, rs.nrows do
        print(rs:fetch_row()[1])
      end
    end
    con:disconnect()
  end }
}
EOF
  SB_SQL="$1" $SYSBENCH "$lua" --db-driver=$DB_DRIVER $DB_ARGS sql
  rc=$?
  rm -f "$lua"
  return $rc
}

# One line with the server settings that change the results.
server_settings() {
  local sql
  case "$DB_DRIVER" in
    firebird)
      sql="SELECT 'Firebird ' || rdb\$get_context('SYSTEM', 'ENGINE_VERSION')
        || ', page size ' || m.MON\$PAGE_SIZE
        || ', page buffers ' || m.MON\$PAGE_BUFFERS
        || ', forced writes ' || TRIM(IIF(m.MON\$FORCED_WRITES = 1, 'on', 'off'))
        || ', linger ' || COALESCE(r.RDB\$LINGER, 0) || ' s'
        || ', protocol ' || COALESCE(rdb\$get_context('SYSTEM', 'NETWORK_PROTOCOL'), 'embedded')
        || ', wire encrypted ' || COALESCE(rdb\$get_context('SYSTEM', 'WIRE_ENCRYPTED'), 'n/a')
        || ', wire compressed ' || COALESCE(rdb\$get_context('SYSTEM', 'WIRE_COMPRESSED'), 'n/a')
        FROM MON\$DATABASE m CROSS JOIN RDB\$DATABASE r"
      ;;
    mysql)
      sql="SELECT CONCAT('MySQL ', @@version,
        ', innodb_buffer_pool_size ', @@innodb_buffer_pool_size,
        ', innodb_flush_log_at_trx_commit ', @@innodb_flush_log_at_trx_commit,
        ', innodb_adaptive_hash_index ', @@innodb_adaptive_hash_index)"
      ;;
    pgsql)
      sql="SELECT 'PostgreSQL ' || current_setting('server_version')
        || ', shared_buffers ' || current_setting('shared_buffers')
        || ', synchronous_commit ' || current_setting('synchronous_commit')"
      ;;
  esac
  sb_sql "$sql" 2>/dev/null | grep -v '^$' | tail -n 1
}

# Firebird only: set LINGER once, so that the cache stays warm between runs.
firebird_set_linger() {
  if [ "$DB_DRIVER" = firebird ] && [ "$TRANSPORT" = tcp ]; then
    sb_sql "ALTER DATABASE SET LINGER TO $FIREBIRD_LINGER" > /dev/null 2>&1 ||
      echo "WARNING: could not set LINGER on the Firebird database"
  fi
}

# Header lines that make a results file repeatable.
write_settings_header() {
  local settings
  settings=$(server_settings || true)
  echo "Transport: $TRANSPORT" | tee -a "$RESULTS_FILE"
  echo "Warm-up: ${WARMUP}s, prepare threads: $PREPARE_THREADS" | tee -a "$RESULTS_FILE"
  echo "Server: ${settings:-unknown}" | tee -a "$RESULTS_FILE"
}
