#!/bin/bash
set -u
echo "=== grep telemetry in log ==="
grep -i "telemetry" /var/lib/seekdb/log/seekdb.log 2>/dev/null | tail -30
echo "=== count ==="
grep -ic "telemetry" /var/lib/seekdb/log/seekdb.log 2>/dev/null
echo "=== grep report ==="
grep -iE "report(er|ing)?_?(interval|server|url)|openwebapi" /var/lib/seekdb/log/seekdb.log 2>/dev/null | tail -15
