#!/bin/bash
echo "=== evidence 01 ==="
cat /mnt/e/seekdb/deploy/evidence/01_docker_and_telemetry.txt
echo "=== evidence 02 ==="
cat /mnt/e/seekdb/deploy/evidence/02_sql_verification.txt
echo
echo "=== telemetry endpoint strings in seekdb binary ==="
strings -n 10 /usr/bin/seekdb 2>/dev/null | grep -iE 'telemetry|https://|http://' | grep -viE 'github|gitee|docs|example' | sort -u | head -30
