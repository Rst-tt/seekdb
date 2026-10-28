#!/bin/bash
set -u
echo "=== connectivity to telemetry endpoint ==="
curl -sS -m 12 -o /dev/null -w "http=%{http_code} time=%{time_total}\n" https://openwebapi.oceanbase.com/api/web/oceanbase/report 2>&1 | tail -2
echo
echo "=== manual start test (no REPORTER env, port 2882) ==="
mkdir -p /root/seekdb-manual
cd /root/seekdb-manual
env -u REPORTER /usr/bin/seekdb --nodaemon --base-dir=/root/seekdb-manual \
  --data-dir=/root/seekdb-manual/store --redo-dir=/root/seekdb-manual/store/redo \
  --port=2882 --parameter cpu_count=2 --parameter memory_budget=2G \
  > /root/seekdb-manual/console.log 2>&1 &
PID=$!
echo "pid=$PID"
for i in $(seq 1 60); do
  if [ -f /root/seekdb-manual/run/telemetry.json ]; then echo "telemetry.json appeared after ${i}s"; break; fi
  sleep 1
done
echo "=== run dir ==="
ls -la /root/seekdb-manual/run/ 2>&1
echo "=== telemetry content ==="
cat /root/seekdb-manual/run/telemetry.json 2>&1
echo
echo "=== sql check ==="
mysql -h127.0.0.1 -P2882 -uroot -A -e "select version()" 2>&1 | head -3
echo "=== stop manual instance ==="
kill $PID 2>/dev/null
sleep 3
ps -p $PID >/dev/null 2>&1 && kill -9 $PID 2>/dev/null
echo "stopped"
