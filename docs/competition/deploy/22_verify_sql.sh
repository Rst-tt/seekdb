#!/bin/bash
echo "=== telemetry.json (docker instance) ==="
cat /var/lib/seekdb-docker/run/telemetry.json; echo
echo
echo "=== extract id ==="
/home/rst/seekdb-env/bin/python - <<'PY'
import json
with open('/var/lib/seekdb-docker/run/telemetry.json') as f:
    d = json.load(f)
print('TELEMETRY_ID =', d['content']['id'])
print('reporter     =', d['content']['reporter'])
print('sent         =', d.get('sent'))
PY
echo
echo "=== SQL verification via 127.0.0.1:2882 ==="
mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "select version(), @@port, user(), now();" 2>&1
mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "create database if not exists comp_verify; use comp_verify; create table if not exists t_hello(id int primary key auto_increment, msg varchar(64), ts timestamp default current_timestamp); insert into t_hello(msg) values ('hello seekdb from xfz'); select * from t_hello;" 2>&1
