#!/bin/bash
echo "=== candidate telemetry URL strings ==="
strings -n 12 /usr/bin/seekdb | grep -iE '^https?://' | sort -u | grep -viE 'relaxng|w3\.org|gnu\.org|apache|json-schema|xmlns|schemas' | head -30
echo
echo "=== strings around TELEMETRY_APP_ID/URL ==="
strings -n 4 /usr/bin/seekdb | grep -iE 'telemetry' | sort -u | head -40
echo
echo "=== tcpdump available? ==="
command -v tcpdump tshark || echo "none"
