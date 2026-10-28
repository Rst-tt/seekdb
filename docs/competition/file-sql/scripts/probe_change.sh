#!/bin/bash
B=/home/rst/seekdb-ws/seekdb/deps/3rd/u01/obclient/bin/obclient
E=/home/rst/sdbtest/edge
P=5500
run() { echo "### $1"; $B -h127.0.0.1 -P$P -uroot --batch -e "set session enable_file_sql=ON; $1" 2>&1 | head -8; echo; }
run "describe select * from read_csv('$E/allnull.csv')"
# file change detection: same SQL text twice, file modified between
printf 'v\n1\n2\n' > $E/change.csv
run "select count(*) from read_csv('$E/change.csv')"
printf 'v\n1\n2\n3\n4\n' > $E/change.csv
run "select count(*) from read_csv('$E/change.csv')"
# and via a view-less identical statement twice without change
run "select count(*) from read_csv('$E/change.csv')"
