#!/bin/bash
echo "=== server data dir tree (telemetry-related) ==="
find /var/lib/seekdb -maxdepth 4 \( -iname '*telemetry*' -o -iname '*local_store*' -o -iname '*store*' \) -not -path '*/sstable/*' 2>/dev/null | head -40
echo
echo "=== grep for telemetryVersion / hostHash in data dir (text files only, maxdepth 5) ==="
grep -rl --include='*' --exclude-dir=sstable -I -e 'telemetryVersion' -e 'hostHash' -e '"bootstraped"' /var/lib/seekdb 2>/dev/null | head -20
echo
echo "=== recent log lines containing telemetry ==="
grep -rih 'telemetry' /var/lib/seekdb/log/*.log 2>/dev/null | tail -15
echo
echo "=== run dir listing ==="
ls -la /var/lib/seekdb/run/ 2>/dev/null
