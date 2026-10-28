#!/bin/bash
IMG=docker.m.daocloud.io/oceanbase/seekdb:latest
IP=$(getent hosts openwebapi.oceanbase.com | awk '{print $1}' | head -1)
echo "endpoint ip: $IP"

# fresh scratch instance, separate dirs => independent telemetry bootstrap
rm -rf /var/lib/seekdb-tltest
mkdir -p /var/lib/seekdb-tltest
docker rm -f seekdb-tltest >/dev/null 2>&1

timeout 45 tcpdump -i any -s 0 -A "host $IP and tcp port 443" > /tmp/tltest.cap 2>/dev/null &
TCPDUMP_PID=$!
sleep 2

docker run -d --name seekdb-tltest -v /var/lib/seekdb-tltest:/var/lib/seekdb "$IMG" >/dev/null
echo "scratch container started, waiting 30s for bootstrap+report..."
sleep 30
kill $TCPDUMP_PID 2>/dev/null
wait $TCPDUMP_PID 2>/dev/null

echo "=== scratch telemetry.json ==="
cat /var/lib/seekdb-tltest/run/telemetry.json 2>/dev/null; echo
echo "=== SNI/host seen in capture ==="
grep -aoE 'openwebapi\.oceanbase\.com' /tmp/tltest.cap | sort | uniq -c
grep -acE 'Client Hello|POST ' /tmp/tltest.cap || true
echo "=== capture size ==="
wc -c /tmp/tltest.cap

docker stop seekdb-tltest >/dev/null 2>&1
docker rm seekdb-tltest >/dev/null 2>&1
echo "scratch container removed"
