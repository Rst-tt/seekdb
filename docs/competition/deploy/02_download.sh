#!/bin/bash
set -u
BASE=http://mirrors.oceanbase.com/oceanbase/community/stable/ubuntu/jammy/amd64
PKG=seekdb_1.4.0-100000212026082616ubuntu22.04_amd64.deb
mkdir -p /root/pkg && cd /root/pkg
if [ ! -s "$PKG" ]; then
  curl -fSL --retry 5 --retry-delay 2 -m 1800 -o "$PKG" "$BASE/$PKG"
fi
ls -l "$PKG"
md5sum "$PKG"
