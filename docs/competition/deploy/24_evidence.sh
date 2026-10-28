#!/bin/bash
set -e
EV=/mnt/e/seekdb/deploy/evidence
mkdir -p "$EV"

# 1) make the deployment survive reboots
docker update --restart unless-stopped seekdb-docker || true
systemctl enable docker >/dev/null 2>&1 || true

# 2) capture evidence
{
  echo "# seekdb docker deployment evidence ($(date -Iseconds))"
  echo
  echo "\$ docker ps"
  docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}'
  echo
  echo "\$ docker images | grep seekdb"
  docker images --format '{{.Repository}}:{{.Tag}} {{.ID}} {{.Size}} {{.CreatedSince}}' | grep seekdb
  echo
  echo "\$ docker inspect digest"
  docker inspect seekdb-docker --format '{{.Image}}'
  echo
  echo "\$ cat /var/lib/seekdb-docker/run/telemetry.json"
  cat /var/lib/seekdb-docker/run/telemetry.json
  echo
} > "$EV/01_docker_and_telemetry.txt"

{
  echo "\$ mysql -h127.0.0.1 -P2882 -uroot -e 'select version(), @@port, now()'"
  mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "select version(), @@port, now()"
  echo
  echo "\$ mysql ... -e 'show databases'"
  mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "show databases"
  echo
  echo "\$ mysql ... comp_verify"
  mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "use comp_verify; select * from t_hello"
} > "$EV/02_sql_verification.txt" 2>&1

cp /var/lib/seekdb-docker/run/telemetry.json "$EV/03_telemetry.json"
docker logs seekdb-docker > "$EV/04_container_logs.txt" 2>&1

echo "=== evidence files ==="
ls -la "$EV"
echo
echo "=== restart policy ==="
docker inspect seekdb-docker --format '{{.HostConfig.RestartPolicy.Name}}'
