#!/bin/bash
echo "=== start.sh: telemetry-related lines ==="
docker exec seekdb-docker sh -c 'grep -n -i telemetry /root/start.sh' 2>&1
echo
echo "=== seekdb log dir ==="
ls -la /var/lib/seekdb-docker/log 2>/dev/null
echo
echo "=== log lines mentioning telemetry ==="
grep -rih 'telemetry' /var/lib/seekdb-docker/log/ 2>/dev/null | tail -20
echo
echo "=== container logs mentioning telemetry ==="
docker logs seekdb-docker 2>&1 | grep -i -E 'telemetry|report' | tail -20
echo "(end)"
