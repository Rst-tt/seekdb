#!/bin/bash
# Reproducible A/B for the seekdb x AMD hipVS CAGRA contest.
#
#   run_bench_ab.sh [workdir] [dataset] [dim] [nq]
#
# default: workdir=/workspace/ab, dataset=sift250k, dim=128, nq=4096
#
# It builds two bridge libraries from the same tree:
#   * base   - the frozen V5 baseline bridge (bench/baseline_seekdb_cuvs_bridge.c)
#   * tuned  - the bridge in tools/hipvs_bridge (this submission)
# then runs the identical SQL workload against both and prints the ratio.
#
# Prerequisites (evaluation box):
#   * /opt/hipvs and /opt/rocm present, hipcc on PATH or in $ROCM_ROOT/bin
#   * a seekdb observer built with -DOB_BUILD_CUVS=ON (or pass $SEEKDB_BIN)
#   * prepared data files produced by bench/prep_data.py
set -uo pipefail

WORK=${1:-/workspace/ab}
DS=${2:-sift250k}
DIM=${3:-128}
NQ=${4:-4096}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
PY=${PY:-python3}
SEEKDB_BIN=${SEEKDB_BIN:-$REPO/build_release/src/observer/seekdb}
PREPARED=${PREPARED:-/workspace/data/prepared}

mkdir -p "$WORK/bridge/base" "$WORK/bridge/tuned" "$WORK/logs"

echo "== building baseline bridge (frozen V5)"
"$HIPCC" -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ \
    -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" \
    "$HERE/baseline_seekdb_cuvs_bridge.c" \
    -L"$HIPVS_ROOT/lib" -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" -lcuvs_c \
    -o "$WORK/bridge/base/libseekdb_cuvs_bridge.so" || exit 1

echo "== building tuned bridge (this submission)"
HIPVS_ROOT=$HIPVS_ROOT ROCM_ROOT=$ROCM_ROOT \
    bash "$REPO/tools/hipvs_bridge/build.sh" "$WORK/bridge/tuned/libseekdb_cuvs_bridge.so" || exit 1

run_one () {   # name bridge outjson
  local name=$1 bridge=$2 out=$3
  rm -rf "$WORK/run_$name"
  "$PY" "$HERE/bench_sql.py" --binary "$SEEKDB_BIN" --bridge "$bridge" \
      --prepared "$PREPARED" --dataset "$DS" --dim "$DIM" --nq "$NQ" \
      --k-list 10 100 --work-dir "$WORK/run_$name" --out "$out" \
      > "$WORK/logs/sql_$name.log" 2>&1
  echo "-- $name finished (rc=$?), log: $WORK/logs/sql_$name.log"
}

run_one base  "$WORK/bridge/base/libseekdb_cuvs_bridge.so"  "$WORK/sql_base_$DS.json"
run_one tuned "$WORK/bridge/tuned/libseekdb_cuvs_bridge.so" "$WORK/sql_tuned_$DS.json"

"$PY" - "$WORK/sql_base_$DS.json" "$WORK/sql_tuned_$DS.json" <<'EOF'
import json, sys
base = json.load(open(sys.argv[1]))
tuned = json.load(open(sys.argv[2]))
def g(d, k, default=None):
    return d.get(k, default)
print("\n%-22s %14s %14s %10s" % ("metric", "baseline(V5)", "tuned", "speedup"))
for k, label, better_is_lower in (
        ("build_window_s", "build window (s)", True),
        ("ddl_s", "CREATE VECTOR INDEX (s)", True),
        ("first_query_s", "first query (s)", True),
        ("qps_k10", "QPS Top-10", False),
        ("qps_k100", "QPS Top-100", False),
        ("recall_k10", "recall@10", False),
        ("recall_k100", "recall@100", False)):
    b, t = g(base, k), g(tuned, k)
    if b in (None, 0) or t in (None, 0):
        print("%-22s %14s %14s %10s" % (label, b, t, "-"))
        continue
    sp = (b / t) if better_is_lower else (t / b)
    print("%-22s %14s %14s %9.3fx" % (label, b, t, sp))
print("\nintegrity: distinct_ids_ok base=%s tuned=%s ; vsag_fallback base=%s tuned=%s" % (
    g(base, "distinct_ids_ok"), g(tuned, "distinct_ids_ok"),
    g(base, "saw_vsag_fallback"), g(tuned, "saw_vsag_fallback")))
EOF
echo "done."
