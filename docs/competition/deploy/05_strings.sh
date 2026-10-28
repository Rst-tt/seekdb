#!/bin/bash
set -u
BIN=/usr/bin/seekdb
echo "=== files named telemetry in binary strings ==="
strings -a "$BIN" | grep -i 'telemetry' | sort -u | head -40
echo "=== run/ path strings ==="
strings -a "$BIN" | grep -E '^run/|/run/telemetry' | sort -u | head -20
