#!/usr/bin/env python3
"""Deploy a local seekdb instance (embedded runtime, official seekdb mode).

Opens a fresh data directory, prints connection options, runs SQL over the
MySQL protocol, and reports the telemetry file path/contents.
"""
import json
import os
import sys
import time
from contextlib import closing

import pylibseekdb
import pymysql

BASE = sys.argv[1] if len(sys.argv) > 1 else "/var/lib/seekdb-inst"
os.makedirs(BASE, exist_ok=True)

db = pylibseekdb.open(BASE)
opts = db.connection_options()
print("connection_options:", json.dumps(opts, default=str), flush=True)

conn = pymysql.connect(
    host=opts.get("host", "127.0.0.1"),
    port=int(opts.get("port", 2881)),
    user=opts.get("user", "root"),
    password=opts.get("password", ""),
    database="test",
    autocommit=True,
    connect_timeout=30,
)
with conn.cursor() as cur:
    cur.execute("select version(), @@port, now()")
    print("select:", cur.fetchall(), flush=True)
    cur.execute("create database if not exists xfz_demo")
    cur.execute("use xfz_demo")
    cur.execute("create table if not exists t1 (id int primary key, name varchar(32))")
    cur.execute("insert into t1 values (1, 'seekdb'), (2, 'xingfuzhe') on duplicate key update name=values(name)")
    cur.execute("select * from t1 order by id")
    print("rows:", cur.fetchall(), flush=True)
conn.close()

tel = os.path.join(BASE, "run", "telemetry.json")
print("telemetry_path:", tel, flush=True)
print("telemetry_exists:", os.path.exists(tel), flush=True)
if os.path.exists(tel):
    with open(tel) as f:
        data = json.load(f)
    print("telemetry_json:", json.dumps(data, ensure_ascii=False), flush=True)
    print("TELEMETRY_ID:", data["content"]["id"], flush=True)

if os.environ.get("KEEP_ALIVE") == "1":
    print("keeping instance alive...", flush=True)
    try:
        while True:
            time.sleep(30)
    except KeyboardInterrupt:
        pass
db.close()
