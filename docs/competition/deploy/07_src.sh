#!/bin/bash
set -u
cd /root
if [ ! -d seekdb-src ]; then
  git clone --depth 1 --filter=blob:none --no-checkout https://github.com/oceanbase/seekdb.git seekdb-src 2>&1 | tail -3
fi
cd /root/seekdb-src
echo "=== tree names with telemetry ==="
git ls-tree -r HEAD --name-only | grep -i telemetry | head -20
echo "=== dirs of interest ==="
git ls-tree -r HEAD --name-only | grep -E 'src/share/.*telemetry|src/observer/.*telemetry' | head -20
