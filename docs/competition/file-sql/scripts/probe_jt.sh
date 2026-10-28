#!/bin/bash
B=/home/rst/seekdb-ws/seekdb/deps/3rd/u01/obclient/bin/obclient
P=5500
run() { echo "### $1"; $B -h127.0.0.1 -P$P -uroot -Dtest --batch -e "$1" 2>&1 | head -6; echo; }
run "select * from json_table('{\"a\":1}', '\$' columns(a int path '\$.a')) jt"
run "select a from json_table('[{\"a\":1}]', '\$[*]' columns(a int path '\$.a')) jt where a = 1"
run "select * from unnest(1,2) t"
run "select * from json_table('{\"a\":1}', '\$' columns(a int path '\$.a'))"
