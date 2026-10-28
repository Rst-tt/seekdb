#!/bin/bash
B=/home/rst/seekdb-ws/seekdb/deps/3rd/u01/obclient/bin/obclient
P=5500
run() { echo "### $1"; $B -h127.0.0.1 -P$P -uroot -Dtest --batch -e "$1" 2>&1 | head -6; echo; }
run "select * from (values (1,2),(3,4)) t"
run "select * from (values (1,2),(3,4)) t(a,b)"
run "select * from (values (1,2)) t(x,y) where x = 1"
run "create table if not exists reg_t1 (id int primary key, name varchar(20))"
run "insert into reg_t1 values (1,'a'),(2,'b')"
run "select id,name from reg_t1 where id > 0 order by id"
run "select json_table('{\"a\":1}', '\$' columns(a int path '\$.a')) from dual"
run "drop table reg_t1"
