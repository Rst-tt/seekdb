#!/bin/bash
set -u
P=/home/rst/seekdb-env/lib/python3.12/site-packages/pylibseekdb
echo "=== package files ==="
ls -la "$P" | head -20
echo "=== __init__.py head ==="
head -60 "$P/__init__.py"
echo "=== grep options/port/env ==="
grep -rniE "def open|port|SEEKDB_|tcp" "$P"/*.py | head -40
