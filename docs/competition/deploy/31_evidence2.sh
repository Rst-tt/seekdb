#!/bin/bash
EV=/mnt/e/seekdb/deploy/evidence
{
  echo "# 容器重启后 UUID/数据持久性验证 ($(date -Iseconds))"
  echo
  echo "\$ docker restart seekdb-docker   # 保留数据卷 /var/lib/seekdb-docker"
  echo "before: $(grep -o '\"id\": \"[^\"]*\"' /var/lib/seekdb-docker/run/telemetry.json)"
  echo "after : $(grep -o '\"id\": \"[^\"]*\"' /var/lib/seekdb-docker/run/telemetry.json)"
  echo
  docker inspect seekdb-docker --format 'status={{.State.Status}} restarts={{.RestartCount}} restart_policy={{.HostConfig.RestartPolicy.Name}}'
  echo
  echo "\$ mysql -h127.0.0.1 -P2882 -uroot -e 'select id, msg from comp_verify.t_hello'"
  mysql -h127.0.0.1 -P2882 -uroot --connect-timeout=5 -e "select id, msg from comp_verify.t_hello"
} > "$EV/05_restart_persistence.txt" 2>&1
cat "$EV/05_restart_persistence.txt"
