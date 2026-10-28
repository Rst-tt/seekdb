#!/bin/bash
set -u
echo "=== service ==="
systemctl is-active seekdb
echo "=== base dir tree (depth 2) ==="
find /var/lib/seekdb -maxdepth 2 -printf '%y %10s %p\n' 2>/dev/null | head -40
echo "=== log dir ==="
ls -la /var/lib/seekdb/log/ 2>&1 | head -20
echo "=== mysql ==="
mysql -h127.0.0.1 -P2881 -uroot -A -e "select version() as v, @@port as port, now() as t" 2>&1 | head -10
echo "=== run dir ==="
ls -la /var/lib/seekdb/run/ 2>&1
