#!/bin/bash
echo "=== daemon state ==="
docker info --format '{{.ServerVersion}} {{.Driver}}' 2>&1 | head -3
systemctl is-active docker 2>&1
echo
echo "=== try starting docker via systemd ==="
systemctl start docker 2>&1
sleep 2
docker info --format 'OK server={{.ServerVersion}} driver={{.Driver}} cgroup={{.CgroupDriver}}' 2>&1 | head -3
