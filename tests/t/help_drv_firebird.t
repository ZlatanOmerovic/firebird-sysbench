Skip test if the Firebird driver is not available.

  $ if [ -z "$SBTEST_HAS_FIREBIRD" ]
  > then
  >   exit 80
  > fi

  $ sysbench --help | sed -n '/firebird options:/,/^$/p'
  firebird options:
    --firebird-db=STRING       Firebird database connection string [localhost:/tmp/sbtest.fdb]
    --firebird-user=STRING     Firebird user [SYSDBA]
    --firebird-password=STRING Firebird password [masterkey]
  
