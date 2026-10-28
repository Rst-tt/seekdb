#!/bin/bash
B=/home/rst/seekdb-ws/seekdb/deps/3rd/u01/obclient/bin/obclient
E=/home/rst/sdbtest/edge
P=5500
mkdir -p $E
# all null column
printf 'a,b\n,\n,\n' > $E/allnull.csv
# widening int -> double
printf 'n\n1\n2.5\n' > $E/widen.csv
# int boundary
printf 'v\n9223372036854775807\n1\n' > $E/bigok.csv
printf 'v\n9223372036854775807\n9223372036854775808\n' > $E/bigoverflow.csv
# date validity
printf 'd\n2026-02-28\n2026-03-01\n' > $E/dateok.csv
printf 'd\n2026-02-30\n2026-03-01\n' > $E/datebad.csv
# utf8 + LF only
printf 'name,city\n张伟,上海\n李娜,北京\n' > $E/utf8.csv
# quotes, escapes, embedded newline (LF)
printf 'id,txt\n1,"a,b"\n2,"line1\nline2"\n3,"say ""hi"""\n' > $E/quotes.csv
# boolean variants
printf 'b\ntrue\nfalse\n' > $E/boolws.csv
# jsonl type conflict
printf '{"a":1}\n{"a":"x"}\n' > $E/conflict.jsonl
# jsonl nested -> should fail
printf '{"a":{"b":1}}\n' > $E/nested.jsonl

run() { echo "### $1"; $B -h127.0.0.1 -P$P -uroot --batch -e "set session enable_file_sql=ON; $1" 2>&1 | head -14; echo; }
run "describe select * from read_csv('$E/allnull.csv')"
run "describe select * from read_csv('$E/widen.csv')"
run "describe select * from read_csv('$E/bigok.csv')"
run "describe select * from read_csv('$E/bigoverflow.csv')"
run "describe select * from read_csv('$E/dateok.csv')"
run "describe select * from read_csv('$E/datebad.csv')"
run "select * from read_csv('$E/utf8.csv')"
run "select id, txt from read_csv('$E/quotes.csv') order by id"
run "describe select * from read_csv('$E/boolws.csv')"
run "describe select * from read_json('$E/conflict.jsonl')"
run "select * from read_json('$E/nested.jsonl')"
run "select count(*) from read_csv('$E/allnull.csv') where a is null"
run "select * from read_csv('$E/utf8.csv') a join read_csv('$E/allnull.csv') b on true limit 2"
