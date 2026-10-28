#!/bin/bash
echo "=== docker/podman presence ==="
command -v docker podman nerdctl 2>/dev/null
docker version 2>&1 | head -5
echo
echo "=== registry reachability (5s each) ==="
for u in https://docker.m.daocloud.io/v2/ https://dockerproxy.com/v2/ https://registry-1.docker.io/v2/ https://quay.io/v2/ https://ghcr.io/v2/; do
  code=$(curl -s -o /dev/null -m 5 -w '%{http_code}' "$u" 2>/dev/null)
  echo "$u -> $code"
done
