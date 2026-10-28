#!/bin/bash
set -u
for h in /root/.bash_history /home/rst/.bash_history; do
  echo "=== $h ==="
  [ -f "$h" ] && tail -60 "$h" || echo "(none)"
done
echo "=== pylibseekdb presence ==="
ls -d /home/rst/*venv* /home/rst/.local/lib/python3*/site-packages/pylibseekdb 2>/dev/null | head
find / -xdev -maxdepth 6 -name "pylibseekdb*" -not -path "*/proc/*" 2>/dev/null | head -5
echo "=== any libseekdb ==="
find / -xdev -name "libseekdb*" 2>/dev/null | head -5
