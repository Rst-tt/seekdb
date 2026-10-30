#!/bin/bash
# Real-data recall calibration on the GPU node.
#
#   bash real_sweep.sh sift 128 /workspace/amd/data/sift_base.fvecs /workspace/amd/data/sift_query.fvecs
#   bash real_sweep.sh nytimes 256 /workspace/amd/data/nytimes_base.fvecs /workspace/amd/data/nytimes_query.fvecs
#
# Requires: tools/hipvs_bridge/seekdb_cuvs_bridge.c built into build/libseekdb_cuvs_bridge.so
# and build/bench_build linked against it (see RUNBOOK.md).
set -u
TAG=${1:?dataset tag}
DIM=${2:?dim}
DATA=${3:?base fvecs}
QRY=${4:?query fvecs}
N=${N:-250000}
NQ=${NQ:-50}
TOPK=${TOPK:-100}
AMT=/workspace/amd
export LD_LIBRARY_PATH=$AMT/build:/opt/hipvs/lib:/opt/rocm/lib
BR=$AMT/build/bench_build
LOG=$AMT/real_${TAG}.log
: > "$LOG"

run() {
  local name=$1; shift
  echo "=== $name $* $(date -Is)" >> "$LOG"
  env "$@" "$BR" --n "$N" --dim "$DIM" --nq "$NQ" --topk "$TOPK" \
      --base "$DATA" --query "$QRY" --tag "$name" 2>>"$LOG" | tee -a "$LOG"
}

# stock (build_algo=NN_DESCENT, degree 128 / intermediate 256 / niter 100)
run "${TAG}_base" SEEKDB_CUVS_TIME=1

# graph-quality cuts
run "${TAG}_n70"  SEEKDB_CUVS_NITER=70 SEEKDB_CUVS_TIME=1
run "${TAG}_n30"  SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_TIME=1
run "${TAG}_i192n30" SEEKDB_CUVS_INTERMEDIATE_DEGREE=192 SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_TIME=1
if [ "$DIM" -le 160 ]; then
  run "${TAG}_d96i192n30" SEEKDB_CUVS_GRAPH_DEGREE=96 SEEKDB_CUVS_INTERMEDIATE_DEGREE=192 SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_TIME=1
fi

# search-side recall compensation (query width) on top of a cheap graph
run "${TAG}_n30_w8"  SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_SEARCH_WIDTH=8 SEEKDB_CUVS_TIME=1
run "${TAG}_n70_w8"  SEEKDB_CUVS_NITER=70 SEEKDB_CUVS_SEARCH_WIDTH=8 SEEKDB_CUVS_TIME=1

# alternative CAGRA build algorithm (numeric value depends on the hipVS enum;
# the printed "params build_algo=" line tells us what NN_DESCENT maps to)
run "${TAG}_algo0" SEEKDB_CUVS_BUILD_ALGO=0 SEEKDB_CUVS_TIME=1
run "${TAG}_algo1" SEEKDB_CUVS_BUILD_ALGO=1 SEEKDB_CUVS_TIME=1
run "${TAG}_algo1_n30" SEEKDB_CUVS_BUILD_ALGO=1 SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_TIME=1
run "${TAG}_algo1_n70" SEEKDB_CUVS_BUILD_ALGO=1 SEEKDB_CUVS_NITER=70 SEEKDB_CUVS_TIME=1
if [ "$DIM" -gt 160 ]; then
  run "${TAG}_algo1_w8" SEEKDB_CUVS_BUILD_ALGO=1 SEEKDB_CUVS_SEARCH_WIDTH=8 SEEKDB_CUVS_TIME=1
fi

echo "DONE ${TAG}" | tee -a "$LOG"
