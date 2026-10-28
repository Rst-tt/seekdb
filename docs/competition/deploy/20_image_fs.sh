#!/bin/bash
IMG=docker.m.daocloud.io/oceanbase/seekdb:latest
echo "=== /root/start.sh ==="
docker run --rm --entrypoint sh "$IMG" -c 'cat /root/start.sh' 2>&1 | head -80
echo
echo "=== /var/lib/seekdb contents ==="
docker run --rm --entrypoint sh "$IMG" -c 'ls -la /var/lib/seekdb; echo ---; ls -la /var/lib/seekdb/run 2>/dev/null' 2>&1 | head -40
