#!/usr/bin/env bash
# Native (docker-free) equivalent of run_ddl_persistence_smoke.sh.
#
# The shipped smoke test drives the observer inside a container and reads the
# trace file that the container image produces. This script does the same
# checks against a locally started observer, so the persistence behaviour can
# be validated on a machine without docker (the AMD evaluation images have no
# docker daemon).
#
# Usage:
#   run_ddl_persistence_native.sh --binary <seekdb> --bridge <libseekdb_cuvs_bridge.so>
#                                 [--tsv <id\tv1,v2,... rows>] [--work-dir <dir>]
#                                 [--port 2881] [--rows 10000] [--dim 128]
#
# Checks (identical to the container version):
#   * the cuvs index answers an approximate query (trace: cuvs_serve)
#   * after a restart the index is reloaded from the snapshot, not rebuilt
#     (trace: cuvs_serve, no cuvs_build; log: deserialized GPU CAGRA snapshot)
#   * the snapshot was really written (log: serialized GPU CAGRA snapshot with
#     built=true) and carries a non-empty graph (graph_bytes>0)
#   * the GPU index never degrades to the CPU index
#     (log must not contain "lazily built VSAG fallback")
#   * the lib=vsag path still reloads its own index (trace: fdeserialize)
#   * deletes and inserts are reflected by the approximate query
set -Eeuo pipefail

BINARY=${BINARY:-}
BRIDGE=${BRIDGE:-}
TSV=${TSV:-}
WORK_DIR=${WORK_DIR:-/tmp/cuvs_ddl_persistence}
PORT=${PORT:-2881}
ROWS=${ROWS:-10000}
DIM=${DIM:-128}
DATABASE=${DATABASE:-p0_persistence}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY=$2; shift 2;;
    --bridge) BRIDGE=$2; shift 2;;
    --tsv) TSV=$2; shift 2;;
    --work-dir) WORK_DIR=$2; shift 2;;
    --port) PORT=$2; shift 2;;
    --rows) ROWS=$2; shift 2;;
    --dim) DIM=$2; shift 2;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
[[ -n "$BINARY" && -x "$BINARY" ]] || { echo "--binary is required" >&2; exit 2; }
[[ -n "$BRIDGE" && -f "$BRIDGE" ]] || { echo "--bridge is required" >&2; exit 2; }
[[ "$DATABASE" =~ ^[a-zA-Z0-9_]+$ ]] || exit 2

EVIDENCE=$WORK_DIR/evidence
rm -rf "$WORK_DIR"
mkdir -p "$EVIDENCE" "$WORK_DIR/run" "$WORK_DIR/log"
LOG=$WORK_DIR/log/seekdb.log
TRACE=$WORK_DIR/p0-trace.log

fail() { echo "FAIL: $*" >&2; echo "artifacts=$EVIDENCE" >&2; exit 1; }
assert_absent() { if grep "$@"; then echo "unexpected match: $*" >&2; exit 1; else [[ $? -eq 1 ]]; fi; }

PY=${PY:-python3}
if [[ -z "$TSV" ]]; then
  TSV=$WORK_DIR/base.tsv
  "$PY" - "$TSV" "$ROWS" "$DIM" <<'PYEOF'
import sys
path, rows, dim = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, "w", newline="\n") as f:
    for i in range(rows):
        vals = ",".join(str(((i * 37 + j * 11) % 1000) / 1000.0) for j in range(dim))
        f.write(f"{i}\t{vals}\n")
PYEOF
fi

start_observer() {
  OB_VSAG_TRACE=${OB_VSAG_TRACE:-1} \
  OB_VSAG_TRACE_FILE=$TRACE \
  LD_LIBRARY_PATH="$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib:${LD_LIBRARY_PATH:-}" \
  "$BINARY" --base-dir="$WORK_DIR/run" --port="$PORT" --nodaemon \
    --parameter _memory_budget=8G \
    --parameter datafile_size=4G \
    --parameter datafile_maxsize=8G \
    --parameter log_disk_size=2G \
    --log-level INFO >> "$LOG" 2>&1 &
  OBSERVER_PID=$!
  local deadline=$((SECONDS + 180))
  until sql 'select 1' >/dev/null 2>&1; do
    if ! kill -0 "$OBSERVER_PID" 2>/dev/null; then fail "observer died during startup"; fi
    ((SECONDS < deadline)) || fail "observer did not become ready"
    sleep 1
  done
}

stop_observer() {
  [[ -n "${OBSERVER_PID:-}" ]] || return 0
  kill "$OBSERVER_PID" 2>/dev/null || true
  local deadline=$((SECONDS + 120))
  while kill -0 "$OBSERVER_PID" 2>/dev/null; do
    ((SECONDS < deadline)) || fail "observer did not stop"
    sleep 1
  done
  wait "$OBSERVER_PID" 2>/dev/null || true
  OBSERVER_PID=""
}

sql() {
  mysql --local-infile=1 -h127.0.0.1 -P"$PORT" -uroot -ANB \
    -e "set ob_query_timeout=300000000; $1"
}

trap 'stop_observer || true' EXIT

start_observer

sql "create database if not exists $DATABASE; use $DATABASE;
drop table if exists items;
create table items(id bigint primary key, v vector($DIM));
load data local infile '$TSV' into table items
fields terminated by '\\t' lines terminated by '\\n' (id,v);"
count=$(sql "select count(*) from $DATABASE.items")
((count >= 10000)) || fail "need at least 10000 pre-DDL rows, got $count"
probe=$(head -n 1 "$TSV" | cut -f2)
log_offset=$(wc -c < "$LOG")
trace_offset=$(wc -c < "$TRACE" 2>/dev/null || echo 0)

sql "use $DATABASE; create vector index idx on items(v)
with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/first.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/exact.ids"
[[ $(wc -l < "$EVIDENCE/first.ids") -eq 10 ]] || fail "approximate query did not return 10 rows"
diff -u "$EVIDENCE/exact.ids" "$EVIDENCE/first.ids"
tail -c +$((trace_offset + 1)) "$TRACE" > "$EVIDENCE/first.trace" 2>/dev/null || true
grep -q cuvs_serve "$EVIDENCE/first.trace" || fail "no cuvs_serve trace for the first query"

# a CPU (lib=vsag) index must keep working next to the GPU one
sql "use $DATABASE; drop table if exists cpu_items;
create table cpu_items(id bigint primary key, v vector($DIM));
load data local infile '$TSV' into table cpu_items
fields terminated by '\\t' lines terminated by '\\n' (id,v);
create vector index idx_cpu on cpu_items(v)
with (distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200);"

stop_observer
start_observer

trace_offset=$(wc -c < "$TRACE")
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/restart.ids"
diff -u "$EVIDENCE/exact.ids" "$EVIDENCE/restart.ids"
tail -c +$((trace_offset + 1)) "$TRACE" > "$EVIDENCE/restart.trace"
grep -q cuvs_serve "$EVIDENCE/restart.trace" || fail "no cuvs_serve trace after the restart"
assert_absent -q cuvs_build "$EVIDENCE/restart.trace" || fail "the GPU index was rebuilt after the restart"

cpu_trace_offset=$(wc -c < "$TRACE")
sql "select id from $DATABASE.cpu_items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/cpu-restart.ids"
diff -u "$EVIDENCE/exact.ids" "$EVIDENCE/cpu-restart.ids"
tail -c +$((cpu_trace_offset + 1)) "$TRACE" > "$EVIDENCE/cpu-restart.trace"
grep -q fdeserialize "$EVIDENCE/cpu-restart.trace" || fail "the lib=vsag index was not reloaded"

tail -c +$((log_offset + 1)) "$LOG" > "$EVIDENCE/lifecycle.log"
grep -a 'serialized GPU CAGRA snapshot' "$EVIDENCE/lifecycle.log" > "$EVIDENCE/gpu.log"
grep -a ' serialized GPU CAGRA snapshot' "$EVIDENCE/gpu.log" | grep -aq 'built=true' \
  || fail "no 'serialized GPU CAGRA snapshot ... built=true' in the log"
grep -a 'deserialized GPU CAGRA snapshot' "$EVIDENCE/gpu.log" | grep -aqE 'graph_bytes=[1-9]' \
  || fail "no 'deserialized GPU CAGRA snapshot ... graph_bytes>0' in the log"
assert_absent -aq 'lazily built VSAG fallback' "$EVIDENCE/lifecycle.log" \
  || fail "the GPU index degraded to the CPU index"

first_id=$(head -n 1 "$TSV" | cut -f1)
sql "delete from $DATABASE.items where id=$first_id"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/deleted.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/deleted-exact.ids"
diff -u "$EVIDENCE/deleted-exact.ids" "$EVIDENCE/deleted.ids"
assert_absent -qx -- "$first_id" "$EVIDENCE/deleted.ids"
sql "insert into $DATABASE.items values(900000000000,$probe)"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/inserted.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/inserted-exact.ids"
diff -u "$EVIDENCE/inserted-exact.ids" "$EVIDENCE/inserted.ids"
grep -qx '900000000000' "$EVIDENCE/inserted.ids"

printf 'PASS pre_ddl_rows=%s first_rows=10 restart_rows=10 exact_match=1 graph_reload=1 gpu_serve=1 rebuild_after_restart=0 eager_vsag=0 cpu_reload=1 stale_ids=0\n' "$count" | tee "$EVIDENCE/result.txt"
stop_observer
