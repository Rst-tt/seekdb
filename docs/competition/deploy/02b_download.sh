#!/bin/bash
set -u
BASE=http://mirrors.oceanbase.com/oceanbase/community/stable/ubuntu/jammy/amd64
PKG=seekdb_1.4.0-100000212026082616ubuntu22.04_amd64.deb
mkdir -p /root/pkg && cd /root/pkg
echo "[$(date +%T)] downloading $PKG"
curl -fSL -C - --retry 8 --retry-all-errors --retry-delay 3 -m 3600 -o "$PKG" "$BASE/$PKG"
rc=$?
echo "[$(date +%T)] curl rc=$rc"
ls -l "$PKG"
if [ $rc -eq 0 ]; then echo DOWNLOAD_OK; else echo DOWNLOAD_FAIL; fi
