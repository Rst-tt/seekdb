#!/bin/bash
df -h /var/lib/docker | tail -1
echo "=== pull via daocloud mirror ==="
timeout 420 docker pull docker.m.daocloud.io/oceanbase/seekdb:latest 2>&1 | tail -8
echo "=== images ==="
docker images --format '{{.Repository}}:{{.Tag}} {{.Size}}' | head -5
