#!/bin/bash
IMG=docker.m.daocloud.io/oceanbase/seekdb:latest
echo "=== image config ==="
docker inspect "$IMG" --format '{{json .Config}}' | head -c 2000; echo
echo
echo "=== entrypoint/cmd/volumes/ports ==="
docker inspect "$IMG" --format 'EXPOSE={{json .Config.ExposedPorts}} VOLUMES={{json .Config.Volumes}} ENV={{json .Config.Env}} ENTRYPOINT={{json .Config.Entrypoint}} CMD={{json .Config.Cmd}} WORKDIR={{.Config.WorkingDir}}'
