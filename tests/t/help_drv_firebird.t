Skip test if the Firebird driver is not available.

  $ if [ -z "$SBTEST_HAS_FIREBIRD" ]
  > then
  >   exit 80
  > fi

  $ sysbench --help | sed -n '/firebird options:/,/^$/p'
  firebird options:
    --firebird-db=STRING                  Firebird database connection string [localhost:/tmp/sbtest.fdb]
    --firebird-user=STRING                Firebird user [SYSDBA]
    --firebird-password=STRING            Firebird password [masterkey]
    --firebird-client=STRING              Path to libfbclient.so to load at runtime (overrides default) [libfbclient.so]
    --firebird-select-mode=STRING         How a prepared SELECT runs: auto (one-row execute; a cursor after it returns more than one row), singleton (always one-row execute), cursor (openCursor and fetch) [auto]
    --firebird-read-tpb=STRING            Transaction for a SELECT outside BEGIN, kept open between statements: snapshot-rw, snapshot-ro, rc-rw or rc-ro (rc = read committed) [rc-ro]
    --firebird-autocommit-select[=on|off] Commit the read transaction after each SELECT outside BEGIN [off]
    --firebird-begin-tpb=STRING           Transaction that BEGIN starts: snapshot-rw, snapshot-ro, rc-rw or rc-ro [snapshot-rw]
    --firebird-write-tpb=STRING           Transaction for a statement outside BEGIN that changes data: snapshot-rw or rc-rw [snapshot-rw]
    --firebird-stats[=on|off]             Print the database page and record counters per query, from the first connection to the last disconnect of the process [off]
  
