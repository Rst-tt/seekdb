#!/bin/bash
set -u
run_sql() { mysql -h127.0.0.1 -P2881 -uroot -A -e "$1" 2>&1 | head -20; }
echo "=== parameters like telemetry ==="
run_sql "show parameters like '%telemetry%';"
echo "=== variables like telemetry ==="
run_sql "show variables like '%telemetry%';"
echo "=== event: scheduler jobs ==="
run_sql "show databases;"
echo "=== observer status ==="
run_sql "select svr_ip, svr_port, zone, build_version from oceanbase.DBA_OB_SERVERS;"
