#!/bin/bash
echo "=== endpoint reachability from WSL host ==="
curl -sS -o /dev/null -m 12 -w 'http_code=%{http_code} dns=%{time_namelookup}s connect=%{time_connect}s total=%{time_total}s\n' https://openwebapi.oceanbase.com/api/web/oceanbase/report || echo "curl failed rc=$?"
echo
echo "=== resolve DNS ==="
getent hosts openwebapi.oceanbase.com || true
echo
echo "=== container has curl/wget? ==="
docker exec seekdb-docker sh -c 'for c in curl wget nc; do command -v $c; done' 2>&1
