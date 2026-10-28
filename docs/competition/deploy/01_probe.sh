#!/bin/bash
set -u
echo "user=$(whoami) host=$(hostname)"
echo "=== mirror index ==="
curl -sS -m 40 http://mirrors.oceanbase.com/oceanbase/community/stable/ubuntu/jammy/amd64/ -o /tmp/idx.html
wc -c /tmp/idx.html
grep -oE 'seekdb[^"<>]*\.deb' /tmp/idx.html | sort -u | head -20
echo "=== speed test (range 10MB) ==="
F=$(grep -oE 'seekdb[^"<>]*\.deb' /tmp/idx.html | sort -u | tail -1)
echo "target=$F"
curl -sS -m 30 -w 'http=%{http_code} size=%{size_download} speed=%{speed_download}\n' -o /dev/null --range 0-10000000 "http://mirrors.oceanbase.com/oceanbase/community/stable/ubuntu/jammy/amd64/$F"
