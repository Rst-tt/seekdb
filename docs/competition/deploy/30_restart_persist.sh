#!/bin/bash
echo "=== before restart ==="
grep -o '"id": "[^"]*"' /var/lib/seekdb-docker/run/telemetry.json
docker restart seekdb-docker >/dev/null
sleep 15
echo "=== after restart ==="
grep -o '"id": "[^"]*"' /var/lib/seekdb-docker/run/telemetry.json
docker inspect seekdb-docker --format 'status={{.State.Status}} restarts={{.RestartCount}}'
mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "select id, msg from comp_verify.t_hello"
