#!/bin/bash
# kill any seekdb on our base-dir, start fresh binary, then run public test
set -u
BIN=/home/rst/seekdb-ws/seekdb/build_release/src/observer/seekdb
BASE=/home/rst/sdbtest
PORT=5500
CLIENT=/home/rst/seekdb-ws/seekdb/deps/3rd/u01/obclient/bin
DATADIR=$BASE/data_for_mysqltest_file_sql

for p in $(pgrep -f "observer/seekdb --nodaemon --base-dir=$BASE"); do kill $p 2>/dev/null; done
sleep 6
for p in $(pgrep -f "observer/seekdb --nodaemon --base-dir=$BASE"); do kill -9 $p 2>/dev/null; done
sleep 2
rm -f $BASE/export.csv $DATADIR/export.csv

nohup setsid $BIN --nodaemon --base-dir=$BASE --port=$PORT --log-level=INFO \
  --variable secure_file_priv=$BASE --parameter datafile_size=8G \
  >> $BASE/log/console.log 2>&1 &
echo "launch pid=$!"
for i in $(seq 1 150); do
  if $CLIENT/obclient -h127.0.0.1 -P$PORT -uroot --batch -e "select 1" >/dev/null 2>&1; then
    echo "ready after $i tries"; break
  fi
  sleep 2
done
$CLIENT/obclient -h127.0.0.1 -P$PORT -uroot -A -N -s --batch -e "
create user if not exists 'admin' identified by 'admin';
grant all on *.* to 'admin';
create database if not exists test;
select @@secure_file_priv, @@enable_file_sql;" 2>&1 | tail -3

echo "=== run mysqltest ==="
TESTDIR=/tmp/fstest
mkdir -p $TESTDIR
ROOT=/home/rst/seekdb-ws/seekdb
sed -e '/copy_file_sql_data\.inc/d' -e '/remove_file_sql_data\.inc/d' \
    -e "s|let \$FILE_SQL_DATA_DIR = \$OBSERVER_DIR/data_for_mysqltest_file_sql;|let \$FILE_SQL_DATA_DIR = $DATADIR;|" \
  $ROOT/tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test > $TESTDIR/file_sql_basic.test
cp $ROOT/tools/deploy/mysql_test/test_suite/file_sql/r/mysql/file_sql_basic.result $TESTDIR/file_sql_basic.result
cd $ROOT/tools/deploy || exit 1
export PATH="$CLIENT:$PATH"
export LD_LIBRARY_PATH="$CLIENT/../lib:${LD_LIBRARY_PATH:-}"
export OBMYSQL_PORT=$PORT OBMYSQL_MS0=127.0.0.1 OBMYSQL_MS0_DEV=127.0.0.1
export OBMYSQL_PWD=admin OBMYSQL_USR=admin OBSERVER_DIR=$BASE IS_BUSINESS=0 TENANT=mysql
mkdir -p /tmp/mslog /tmp/mstmp
rm -rf /tmp/mslog/*
mysqltest --host=127.0.0.1 --port=$PORT --user=admin --password=admin \
  --database=test --tmpdir=/tmp/mstmp --logdir=/tmp/mslog --silent \
  --test-file=$TESTDIR/file_sql_basic.test \
  --result-file=$TESTDIR/file_sql_basic.result \
  --timer-file=/tmp/mslog/timer --tail-lines=60
echo "TEST_EXIT=$?"
