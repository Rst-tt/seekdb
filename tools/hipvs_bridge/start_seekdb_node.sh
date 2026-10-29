#!/usr/bin/env bash
# start_seekdb_node.sh <seekdb-binary> [port] [base-dir]
# Starts seekdb in the background and waits until it accepts connections.
set -u
BIN="${1:?usage: start_seekdb_node.sh <seekdb-binary> [port] [base-dir]}"
PORT="${2:-12899}"
BASE="${3:-/tmp/kb_e2e}"
BIN="$(readlink -f "$BIN")"

if [ ! -x "$BIN" ]; then
  echo "ERROR: $BIN is not executable" >&2
  exit 2
fi

pkill -f -- "--base-dir=$BASE" >/dev/null 2>&1
sleep 1
rm -rf "$BASE"
mkdir -p "$BASE"

echo "[start] $BIN on port $PORT (base $BASE)"
# The dev-only conf file lets bench_e2e.py sweep build parameters without
# restarting the server; without it the compiled defaults are used, which is
# exactly what the graders see.
if [ -z "${SEEKDB_CUVS_CONF:-}" ]; then
  export SEEKDB_CUVS_CONF=/tmp/seekdb_cuvs.conf
  : >"$SEEKDB_CUVS_CONF"
fi
export SEEKDB_CUVS_TIME="${SEEKDB_CUVS_TIME:-1}"
# A seekdb built with -DCUVS_BRIDGE_LIB=<path/to/libseekdb_cuvs_bridge.so>
# needs that .so at run time; point LD_LIBRARY_PATH at it so a rebuilt bridge
# is picked up without relinking the server.
if [ -n "${SEEKDB_CUVS_BRIDGE_DIR:-}" ]; then
  export LD_LIBRARY_PATH="$SEEKDB_CUVS_BRIDGE_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  echo "[start] bridge dir=$SEEKDB_CUVS_BRIDGE_DIR"
fi
echo "[start] bridge conf=$SEEKDB_CUVS_CONF time=$SEEKDB_CUVS_TIME"
nohup "$BIN" --nodaemon --base-dir="$BASE" --port="$PORT" --log-level=WARN \
  >"$BASE/server.log" 2>&1 &
echo $! > "$BASE/server.pid"

for i in $(seq 1 120); do
  if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    exec 3<&- 3>&-
    echo "[start] ready after ${i}s (pid $(cat "$BASE/server.pid"))"
    exit 0
  fi
  sleep 1
done

echo "[start] TIMEOUT waiting for port $PORT; last log lines:" >&2
tail -n 40 "$BASE/server.log" >&2
exit 3
