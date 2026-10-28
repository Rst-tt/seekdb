#!/bin/bash
set -u
echo "=== now: systemd instance run dir ==="
date
ls -la /var/lib/seekdb/run/
echo "=== embedded telemetry mention ==="
grep -ri "telemetry" /home/rst/seekdb-test/log/*.log 2>/dev/null | head -5
echo "=== embedded startup lines ==="
grep -iE "reporter|startup|bootstrap" /home/rst/seekdb-test/log/seekdb.log 2>/dev/null | head -15
echo "=== python pkgs ==="
/usr/bin/python3 -c "import pylibseekdb, sys; print('pylibseekdb', pylibseekdb.__version__ if hasattr(pylibseekdb,'__version__') else 'ok')" 2>&1 | head -3
