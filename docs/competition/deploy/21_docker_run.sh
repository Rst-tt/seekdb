#!/bin/bash
IMG=docker.m.daocloud.io/oceanbase/seekdb:latest
mkdir -p /var/lib/seekdb-docke
docker rm -f seekdb-docker >/dev/null 2>&1
echo "=== run container ==="
docker run -d --name seekdb-docker -p 2882:2881 -v /var/lib/seekdb-docker:/var/lib/seekdb "$IMG"
sleep 25
echo "=== container state ==="
docker ps --filter name=seekdb-docker --format '{{.Status}}'
echo "=== host bind-mount dir ==="
ls -la /var/lib/seekdb-docker | head -20
echo "=== run subdir ==="
ls -la /var/lib/seekdb-docker/run 2>/dev/null
echo "=== telemetry.json ==="
cat /var/lib/seekdb-docker/run/telemetry.json 2>/dev/null || echo "(not present)"
echo
echo "=== container logs tail ==="
docker logs --tail 20 seekdb-docker 2>&1
