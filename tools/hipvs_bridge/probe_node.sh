#!/usr/bin/env bash
# One-shot inventory of a Radeon Cloud seekdb-hipvs node. Prints everything the
# benchmark plan needs; run it first, before touching any source tree.
set -uo pipefail
echo "===== host ====="
uname -a
nproc
free -g | head -2
df -h / /workspace 2>/dev/null | head -5
echo
echo "===== gpu ====="
command -v rocminfo >/dev/null && rocminfo 2>/dev/null | grep -E "^  Name:|gfx|Marketing" | head -8
command -v rocm-smi >/dev/null && rocm-smi --showproductname --showmeminfo vram 2>/dev/null | head -12
echo
echo "===== toolchain ====="
for c in hipcc /opt/rocm/bin/hipcc gcc g++ cmake ninja python3 git; do
  printf '%-22s %s\n' "$c" "$(command -v $c 2>/dev/null || echo MISSING)"
done
echo "hipvs:  $(ls -d /opt/hipvs 2>/dev/null || echo MISSING)"
ls /opt/hipvs/include/cuvs/core/c_api.h /opt/hipvs/lib/libcuvs_c.so 2>&1 | head -4
echo
echo "===== seekdb artefacts ====="
find / -xdev \( -name 'seekdb' -o -name 'observer' \) -type f -perm -u+x 2>/dev/null | head -10
find / -xdev -name 'libseekdb_cuvs_bridge.so' 2>/dev/null | head -5
find / -xdev -name 'ob_vsag_adaptor*' 2>/dev/null | head -5
echo
echo "===== workspaces ====="
ls -la /workspace 2>/dev/null | head -20
ls -la /root 2>/dev/null | head -20
ls -la /data 2>/dev/null | head -20
echo
echo "===== datasets ====="
find / -xdev \( -name '*.fvecs' -o -name '*.ivecs' -o -iname '*sift*' -o -iname '*hidden*' \) 2>/dev/null | head -20
echo
echo "===== python / notebook ====="
python3 -c 'import sys; print(sys.version)' 2>/dev/null
jupyter --version 2>/dev/null | head -3
echo
echo "===== network ====="
getent hosts github.com 2>/dev/null | head -2
timeout 8 curl -sS -o /dev/null -w 'github %{http_code} %{time_total}s\n' https://github.com 2>&1 | tail -1
echo "===== done ====="
