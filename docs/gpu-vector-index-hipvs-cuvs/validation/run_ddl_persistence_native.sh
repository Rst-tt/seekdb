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
# Deterministic vectors with a period far beyond "rows" so that no two rows
# hold the same vector (identical vectors would make the exact top-k a tie
# and any approximate result a different - but equally correct - ordering).
with open(path, "w", newline="\n") as f:
    for i in range(rows):
        vals = ",".join(str(((i * 43561 + j * 11) % 100000) / 100000.0) for j in range(dim))
        f.write(f"{i}\t{vals}\n")
PYEOF
fi

# The approximate result is compared with the exact one.  The comparison is
# "at least $MIN_OVERLAP of the $K ids are the same", because on a tie the
# exact and the approximate query may return the same neighbours in a
# different order.
MIN_OVERLAP=${MIN_OVERLAP:-9}
K=${K:-10}
overlap_ok() {
  local common
  common=$(comm -12 <(sort -n "$1") <(sort -n "$2") | wc -l)
  ((common >= MIN_OVERLAP)) || { echo "only $common/$K ids match between $1 and $2" >&2; return 1; }
}

# Bulk load the TSV with INSERT statements.  LOAD DATA LOCAL INFILE needs the
# client and the server to agree on the local-infile protocol, which is not
# available on every client build, so the rows are inserted explicitly.
load_table() {
  local table=$1
  # Note: the mysql client only reads the statements from stdin when it is
  # not given -e, so the "use <db>" has to be part of the generated script.
  {
    echo "set ob_query_timeout=7200000000;"
    echo "use $DATABASE;"
    "$PY" - "$TSV" "$table" <<'PYEOF'
import sys
path, table = sys.argv[1], sys.argv[2]
batch, out = [], []
with open(path) as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        row_id, values = line.split("\t", 1)
        batch.append(f"({row_id},[{values}])")
        if len(batch) >= 200:
            out.append("insert into %s values %s;" % (table, ",".join(batch)))
            batch = []
if batch:
    out.append("insert into %s values %s;" % (table, ",".join(batch)))
sys.stdout.write("\n".join(out) + "\n")
PYEOF
  } > "$WORK_DIR/load_$table.sql"
  mysql -h127.0.0.1 -P"$PORT" -uroot -ANB < "$WORK_DIR/load_$table.sql"
}

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
  mysql -h127.0.0.1 -P"$PORT" -uroot -ANB \
    -e "set ob_query_timeout=300000000; $1"
}

trap 'stop_observer || true' EXIT

start_observer

sql "create database if not exists $DATABASE; use $DATABASE;
drop table if exists items;
create table items(id bigint primary key, v vector($DIM));"
load_table items
count=$(sql "select count(*) from $DATABASE.items")
((count >= 10000)) || fail "need at least 10000 pre-DDL rows, got $count"
probe="[$(head -n 1 "$TSV" | cut -f2)]"
log_offset=$(wc -c < "$LOG")
trace_offset=$( [[ -f "$TRACE" ]] && wc -c < "$TRACE" || echo 0 )

sql "use $DATABASE; create vector index idx on items(v)
with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/first.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/exact.ids"
[[ $(wc -l < "$EVIDENCE/first.ids") -eq 10 ]] || fail "approximate query did not return 10 rows"
overlap_ok "$EVIDENCE/exact.ids" "$EVIDENCE/first.ids" || fail "the first approximate query does not match the exact one"
tail -c +$((trace_offset + 1)) "$TRACE" > "$EVIDENCE/first.trace" 2>/dev/null || true
grep -q cuvs_serve "$EVIDENCE/first.trace" || fail "no cuvs_serve trace for the first query"

# a CPU (lib=vsag) index must keep working next to the GPU one
sql "use $DATABASE; drop table if exists cpu_items;
create table cpu_items(id bigint primary key, v vector($DIM));"
load_table cpu_items
sql "use $DATABASE; create vector index idx_cpu on cpu_items(v)
with (distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200);"

stop_observer
start_observer

trace_offset=$( [[ -f "$TRACE" ]] && wc -c < "$TRACE" || echo 0 )
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/restart.ids"
overlap_ok "$EVIDENCE/exact.ids" "$EVIDENCE/restart.ids" || fail "the reloaded index does not match the exact result"
tail -c +$((trace_offset + 1)) "$TRACE" > "$EVIDENCE/restart.trace"
grep -q cuvs_serve "$EVIDENCE/restart.trace" || fail "no cuvs_serve trace after the restart"
assert_absent -q cuvs_build "$EVIDENCE/restart.trace" || fail "the GPU index was rebuilt after the restart"

cpu_trace_offset=$( [[ -f "$TRACE" ]] && wc -c < "$TRACE" || echo 0 )
sql "select id from $DATABASE.cpu_items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/cpu-restart.ids"
overlap_ok "$EVIDENCE/exact.ids" "$EVIDENCE/cpu-restart.ids" || fail "the lib=vsag index does not match the exact result"
tail -c +$((cpu_trace_offset + 1)) "$TRACE" > "$EVIDENCE/cpu-restart.trace"
grep -q fdeserialize "$EVIDENCE/cpu-restart.trace" || fail "the lib=vsag index was not reloaded"

# The observer writes its own log files below <base-dir>/log (they rotate),
# so the markers are searched in all of them instead of in the console log.
OBS_LOG_DIR=$WORK_DIR/run/log
OBS_LOGS=$(ls -1 "$OBS_LOG_DIR"/seekdb.log* 2>/dev/null || true)
[[ -n "$OBS_LOGS" ]] || fail "the observer did not write any log file"
cat $OBS_LOGS > "$EVIDENCE/lifecycle.log"
grep -a 'serialized GPU CAGRA snapshot' "$EVIDENCE/lifecycle.log" > "$EVIDENCE/gpu.log" || true
grep -a ' serialized GPU CAGRA snapshot' "$EVIDENCE/gpu.log" | grep -aq 'built=true' \
  || fail "no 'serialized GPU CAGRA snapshot ... built=true' in the log"
grep -a 'deserialized GPU CAGRA snapshot' "$EVIDENCE/lifecycle.log" | grep -aqE 'graph_bytes=[1-9]' \
  || fail "no 'deserialized GPU CAGRA snapshot ... graph_bytes>0' in the log"
if grep -aq 'lazily built VSAG fallback' "$EVIDENCE/lifecycle.log"; then
  fail "the GPU index degraded to the CPU index"
fi

first_id=$(head -n 1 "$TSV" | cut -f1)
sql "delete from $DATABASE.items where id=$first_id"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/deleted.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/deleted-exact.ids"
overlap_ok "$EVIDENCE/deleted-exact.ids" "$EVIDENCE/deleted.ids" || fail "the deleted row is still returned"
assert_absent -qx -- "$first_id" "$EVIDENCE/deleted.ids"
sql "insert into $DATABASE.items values(900000000000,$probe)"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) approximate limit 10" > "$EVIDENCE/inserted.ids"
sql "select id from $DATABASE.items order by l2_distance(v,$probe) limit 10" > "$EVIDENCE/inserted-exact.ids"
overlap_ok "$EVIDENCE/inserted-exact.ids" "$EVIDENCE/inserted.ids" || fail "the inserted row is not returned"
grep -qx '900000000000' "$EVIDENCE/inserted.ids"

printf 'PASS pre_ddl_rows=%s first_rows=10 restart_rows=10 exact_match=1 graph_reload=1 gpu_serve=1 rebuild_after_restart=0 eager_vsag=0 cpu_reload=1 stale_ids=0\n' "$count" | tee "$EVIDENCE/result.txt"
stop_observer
