#!/bin/bash
set -u
echo "=== venvs with pylibseekdb ==="
for v in /home/rst/seekdb-env /home/rst/final-env /home/rst/sdd-conda; do
  if [ -x "$v/bin/python" ]; then
    echo "--- $v"
    "$v/bin/python" -c "import pylibseekdb; print(pylibseekdb.__file__)" 2>&1 | head -2
    "$v/bin/python" -c "import pylibseekdb, pymysql; print('pymysql ok')" 2>&1 | head -2
  fi
done
echo "=== docs: embedded deploy page ==="
curl -sS -m 30 https://docs.seekdb.ai/seekdb/deploy-embedded-seekdb -o /tmp/emb.html -w "http=%{http_code}\n"
grep -o -i "telemetry[^<]\{0,80\}" /tmp/emb.html | head -10
