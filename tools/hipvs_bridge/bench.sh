#!/usr/bin/env bash
# Build the bridge + bench harness and run a parameter matrix on the GPU node.
#   ./bench.sh quick            fp32 vs fp16 at the contest defaults
#   ./bench.sh matrix           quick + shrunk nn-descent / intermediate degree
#   ./bench.sh one "<env>"      a single config, e.g.
#        ./bench.sh one "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_NITER=50"
# Extra args are forwarded to bench_build (--n, --dim, --nq, --base, ...).
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
MODE=${1:-quick}
shift || true
EXTRA_ARGS=("$@")

echo "== host =="
uname -a | head -1
command -v rocminfo >/dev/null && (rocminfo | grep -m1 -E "Marketing Name|gfx" || true)
echo "HIPVS_ROOT=$HIPVS_ROOT"

bash "$HERE/build.sh" "$HERE/libseekdb_cuvs_bridge.so"

"$HIPCC" -O2 -D__HIP_PLATFORM_AMD__ \
  -I"$HERE" -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" \
  "$HERE/bench_build.c" \
  -L"$HERE" -Wl,-rpath,"$HERE:$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lseekdb_cuvs_bridge -lcuvs_c \
  -o "$HERE/bench_build"
echo "built $HERE/bench_build"

run_one() {
  local tag="$1"; shift
  local envs="$1"; shift
  echo "---- $tag :: $envs"
  # shellcheck disable=SC2086
  env $envs "$HERE/bench_build" --tag "$tag" "${EXTRA_ARGS[@]}" 2>&1 | tee -a "$HERE/bench.log" | grep -E 'AVERAGE|failed|error' || true
}

: > "$HERE/bench.log"
case "$MODE" in
  quick)
    run_one fp32_default ""
    run_one fp16_default "SEEKDB_CUVS_FP16=1"
    ;;
  matrix)
    run_one fp32_default ""
    run_one fp16_default "SEEKDB_CUVS_FP16=1"
    run_one fp32_n50 "SEEKDB_CUVS_NITER=50"
    run_one fp16_n50 "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_NITER=50"
    run_one fp16_n20 "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_NITER=20"
    run_one fp16_n50_i128 "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_NITER=50 SEEKDB_CUVS_INTERMEDIATE_DEGREE=128"
    run_one fp16_n20_i128 "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_NITER=20 SEEKDB_CUVS_INTERMEDIATE_DEGREE=128"
    run_one fp32_n50_g64 "SEEKDB_CUVS_NITER=50 SEEKDB_CUVS_GRAPH_DEGREE=64 SEEKDB_CUVS_INTERMEDIATE_DEGREE=128"
    ;;
  one)
    run_one custom "$@"
    ;;
  *)
    echo "unknown mode $MODE (quick|matrix|one)" >&2; exit 2;;
esac

echo "== summary (bench.log) =="
cat "$HERE/bench.log"
