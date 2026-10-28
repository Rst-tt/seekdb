#!/bin/bash
set -u
D=/home/rst/seekdb-test
echo "=== tree ==="
find "$D" -maxdepth 2 -printf '%y %10s %p\n' 2>/dev/null | head -30
echo "=== telemetry log lines ==="
grep -ri "telemetry" "$D"/log/*.log 2>/dev/null | head -20
echo "=== reporter/interval hints ==="
grep -riE "reporter|report_interval|telemetry" "$D"/log/*.log 2>/dev/null | wc -l
