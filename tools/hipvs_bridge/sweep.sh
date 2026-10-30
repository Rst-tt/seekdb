#!/usr/bin/env bash
# sweep.sh <seekdb-binary> [port] [base.fvecs] [n] [dim]
#
# One server, data loaded once, one timed `CREATE VECTOR INDEX` per candidate
# parameter set.  The bridge re-reads SEEKDB_CUVS_CONF on every build, so no
# restart and no re-load is needed between candidates.
#
#   SEEKDB_CUVS_BRIDGE_DIR=/out bash sweep.sh /out/seekdb 2881 /data/sift/base.fvecs 250000 128
#
# Anything after the four positional arguments is forwarded to bench_e2e.py
# (for example --nq 50 or --repeat 2).
set -u
BIN="${1:?usage: sweep.sh <seekdb-binary> [port] [base.fvecs] [n] [dim]}"
PORT="${2:-12899}"
BASE_FV="${3:-}"
N="${4:-250000}"
DIM="${5:-128}"
for _ in 1 2 3 4 5; do
  [ "$#" -gt 0 ] && shift
done

DIR="$(cd "$(dirname "$0")" && pwd)"
export SEEKDB_CUVS_CONF="${SEEKDB_CUVS_CONF:-/tmp/seekdb_cuvs.conf}"
export SEEKDB_CUVS_TIME="${SEEKDB_CUVS_TIME:-1}"

CONFIGS=(
  "fp16=0"
  "fp16=1"
  "fp16=1,intermediate_degree=128"
  "fp16=1,intermediate_degree=128,graph_degree=96"
  "fp16=1,intermediate_degree=128,graph_degree=96,niter=60"
  "fp16=1,intermediate_degree=128,graph_degree=96,niter=20"
  "fp16=1,intermediate_degree=64,graph_degree=64,niter=20"
)

bash "$DIR/start_seekdb_node.sh" "$BIN" "$PORT" /tmp/kb_sweep || exit 1

conf_args=()
for c in "${CONFIGS[@]}"; do
  conf_args+=(--conf "$c")
done

python3 -u "$DIR/bench_e2e.py" \
  --port "$PORT" --n "$N" --dim "$DIM" \
  ${BASE_FV:+--base "$BASE_FV"} \
  --recall --nq 20 --keep \
  "${conf_args[@]}" "$@" | tee "$(dirname "$(readlink -f "$BIN")")/sweep.log"
