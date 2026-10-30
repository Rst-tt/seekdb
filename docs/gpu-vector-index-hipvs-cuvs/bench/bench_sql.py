#!/usr/bin/env python3
"""SQL level A/B for the seekdb x hipVS CAGRA contest.

Starts the observer that was built with -DOB_BUILD_CUVS=ON, loads the dataset,
then measures exactly what the contest measures:

  * build window  = time of `CREATE VECTOR INDEX ... lib=cuvs` + the first
    fixed approximate query (the first query is part of the build window),
  * query throughput for nq single row approximate queries (Top-10 / Top-100),
  * recall@k against a brute force ground truth file and the "k distinct ids
    per query" integrity check.

The same script is used for the frozen V5 baseline bridge and for the tuned
bridge, so the numbers are directly comparable.

usage: bench_sql.py --binary <seekdb> --bridge <libseekdb_cuvs_bridge.so>
                    --prepared <dir> --dataset sift250k --dim 128 --nq 4096
                    --work-dir <dir> --out <result.json>
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np

MYSQL = ["mysql", "-h127.0.0.1", "-P2881", "-uroot"]


def sh(cmd, inp=None, timeout=None):
    return subprocess.run(cmd, input=inp, capture_output=True, text=True, timeout=timeout)


def sql_rows(q, timeout=None):
    r = sh(MYSQL + ["-N", "-e", q], timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"mysql failed: {r.stderr[:400]}")
    return [ln.split("\t") for ln in r.stdout.strip().splitlines() if ln]


def sql_exec(q, timeout=None):
    r = sh(MYSQL + ["-e", q], timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"mysql failed: {r.stderr[:400]}")
    return r.stdout


def start_observer(binary, bridge, base_dir, log_level="INFO"):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ":".join([os.path.dirname(os.path.abspath(bridge)),
                                       "/opt/hipvs/lib", "/opt/rocm/lib",
                                       env.get("LD_LIBRARY_PATH", "")])
    cmd = [binary, f"--base-dir={base_dir}", "--port=2881", "--nodaemon",
           "--parameter", "_memory_budget=16G",
           "--parameter", "datafile_size=4G",
           "--parameter", "datafile_maxsize=8G",
           "--parameter", "log_disk_size=2G",
           "--log-level", log_level]
    out = open(base_dir + ".out", "w")
    proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env)
    for _ in range(180):
        time.sleep(1)
        if os.path.exists(os.path.join(base_dir, "log", "seekdb.log")):
            try:
                txt = open(os.path.join(base_dir, "log", "seekdb.log"), errors="ignore").read()
            except OSError:
                txt = ""
            if "bootstrap success" in txt:
                break
        if proc.poll() is not None:
            raise RuntimeError("observer died: " + open(base_dir + ".out", errors="ignore").read()[-2000:])
    for _ in range(60):
        r = sh(MYSQL + ["-e", "select 1"], timeout=20)
        if r.returncode == 0:
            return proc
        time.sleep(1)
    raise RuntimeError("observer not ready")


def stop_observer(proc):
    try:
        proc.terminate()
        proc.wait(timeout=60)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def load_data(base_dir, dataset, dim, prepared_dir, rows_per_stmt=2000):
    """Create the table and load base rows (no vector index yet)."""
    base = np.fromfile(os.path.join(prepared_dir, f"{dataset}_base.f32"), dtype=np.float32)
    n = base.size // dim
    base = base.reshape(n, dim)
    q = np.fromfile(os.path.join(prepared_dir, f"{dataset}_query.f32"), dtype=np.float32)
    nq = q.size // dim
    q = q.reshape(nq, dim)

    sql = ["create database if not exists vec;", "use vec;",
           "alter system set ob_vector_memory_limit_percentage=30;",
           "drop table if exists items;",
           "create table items(id bigint primary key, v vector(%d));" % dim,
           "drop table if exists probes;",
           "create table probes(id int primary key, pv vector(%d));" % dim]
    with open("/tmp/load_items.sql", "w") as fh:
        fh.write("\n".join(sql) + "\n")
        for s in range(0, n, rows_per_stmt):
            chunk = base[s:s + rows_per_stmt]
            vals = ",".join("(%d,[%s])" % (i, ",".join("%.6g" % x for x in row))
                            for i, row in enumerate(chunk, start=s))
            fh.write("insert into items values %s;\n" % vals)
        fh.write("use vec;\n")
        for s in range(0, nq, 1000):
            chunk = q[s:s + 1000]
            vals = ",".join("(%d,[%s])" % (i, ",".join("%.6g" % x for x in row))
                            for i, row in enumerate(chunk, start=s))
            fh.write("insert into probes values %s;\n" % vals)
    t0 = time.time()
    with open("/tmp/load_items.sql") as fh:
        r = subprocess.run(MYSQL, stdin=fh, capture_output=True, text=True, timeout=7200)
    if r.returncode != 0:
        raise RuntimeError("load failed: " + r.stderr[-2000:])
    return n, nq, time.time() - t0


def vec_lit(row):
    return "[" + ",".join("%.6g" % x for x in row) + "]"


def run_bench(args):
    prepared = args.prepared
    dim = args.dim
    nq = args.nq
    work = args.work_dir
    if os.path.exists(work):
        shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    data_dir = os.path.join(work, "obs")

    t_prep0 = time.time()
    if args.template:
        shutil.copytree(args.template, data_dir)
    else:
        os.makedirs(data_dir, exist_ok=True)
    res = {"dataset": args.dataset, "nq": nq, "dim": dim,
           "binary": args.binary, "bridge": args.bridge}

    proc = start_observer(args.binary, args.bridge, data_dir)
    try:
        n, nq_loaded, load_s = load_data(data_dir, args.dataset, dim, prepared)
        res["n_rows"] = n
        res["load_s"] = round(load_s, 2)
        print(f"[bench] loaded {n} rows in {load_s:.1f}s", flush=True)
        if args.save_template and not os.path.exists(args.save_template):
            shutil.copytree(data_dir, args.save_template)
            print(f"[bench] saved template -> {args.save_template}", flush=True)

        # ---- build window: CREATE VECTOR INDEX (+ first query) ----
        ddl = ("use vec; create vector index idx_items on items(v) with "
               "(distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);")
        t0 = time.time()
        sql_exec("set session ob_query_timeout = 7200000000; " + ddl, timeout=7200)
        t_ddl = time.time() - t0
        probe = vec_lit(np.fromfile(os.path.join(prepared, f"{args.dataset}_query.f32"),
                                    dtype=np.float32).reshape(-1, dim)[0])
        t0 = time.time()
        rows = sql_rows("use vec; select id from items order by l2_distance(v,%s) "
                        "approximate limit 10;" % probe, timeout=3600)
        t_first = time.time() - t0
        res["ddl_s"] = round(t_ddl, 3)
        res["first_query_s"] = round(t_first, 4)
        res["build_window_s"] = round(t_ddl + t_first, 3)
        res["first_query_rows"] = len(rows)
        print(f"[bench] DDL {t_ddl:.2f}s, first query {t_first*1000:.1f}ms, "
              f"window {t_ddl + t_first:.2f}s", flush=True)

        # ---- query throughput: nq single-row approximate queries (evaluation shape) ----
        try:
            qvecs = np.fromfile(os.path.join(prepared, f"{args.dataset}_query.f32"),
                                dtype=np.float32).reshape(-1, dim)[:nq]
            gt = np.fromfile(os.path.join(prepared, f"{args.dataset}_gt100.i32"),
                             dtype=np.int32).reshape(-1, 100)
            for k in args.k_list:
                parts = ["set session ob_query_timeout = 7200000000;", "use vec;"]
                for i in range(nq):
                    parts.append("select id from items order by l2_distance(v,%s) "
                                 "approximate limit %d;" % (vec_lit(qvecs[i]), k))
                payload = "\n".join(parts) + "\n"
                t0 = time.time()
                r = sh(MYSQL + ["-N"], inp=payload, timeout=7200)
                dt = time.time() - t0
                if r.returncode != 0:
                    raise RuntimeError("mysql failed: %s" % r.stderr[:400])
                out = [ln for ln in r.stdout.splitlines() if ln.strip()]
                res["q_s_k%d" % k] = round(dt, 3)
                res["qps_k%d" % k] = round(nq / dt, 2)
                res["rows_k%d" % k] = len(out)
                hit = tot = 0
                bad_distinct = 0
                pos = 0
                for i in range(nq):
                    ids = []
                    for _ in range(k):
                        if pos < len(out):
                            ids.append(int(out[pos].split("\t")[0]))
                            pos += 1
                    if len(set(ids)) != len(ids):
                        bad_distinct += 1
                    gset = set(gt[i % gt.shape[0]].tolist())
                    for x in ids:
                        tot += 1
                        hit += 1 if x in gset else 0
                res["recall_k%d" % k] = round(hit / tot, 5) if tot else 0.0
                res["distinct_ids_ok"] = (bad_distinct == 0)
                res["rows_expected_k%d" % k] = nq * k
                print(f"[bench] k={k}: {dt:.2f}s -> {nq/dt:.1f} q/s, "
                      f"rows={len(out)}/{nq*k}, recall@{k}={res['recall_k%d' % k]:.5f}, "
                      f"distinct ids ok: {res['distinct_ids_ok']}", flush=True)
        except Exception as e:
            res["query_phase_error"] = repr(e)
            print("[bench] query phase error: %r" % (e,), flush=True)

        # ---- gate flags from the observer log ----
        log = open(os.path.join(data_dir, "log", "seekdb.log"), errors="ignore").read()
        res["saw_cuvs_build"] = "cuvs_build" in log
        res["saw_cuvs_serve"] = "cuvs_serve" in log
        res["saw_vsag_fallback"] = "lazily built VSAG fallback" in log
        res["saw_serialized"] = "serialized GPU CAGRA snapshot" in log
        for line in log.splitlines():
            if "[OBVSAG][cuVS]" in line and ("snapshot" in line or "built GPU" in line):
                print("[log]", line.strip()[:300], flush=True)
        try:
            obs_out = open(data_dir + ".out", errors="ignore").read()
            for line in obs_out.splitlines():
                if "[seekdb_cuvs]" in line:
                    print("[bridge]", line.strip()[:300], flush=True)
                    res.setdefault("bridge_log", []).append(line.strip()[:300])
        except OSError:
            pass
    finally:
        stop_observer(proc)
        if args.keep_template and not args.template:
            shutil.rmtree(os.path.join(work, "tmpl"), ignore_errors=True)
    res["total_s"] = round(time.time() - t_prep0, 2)
    with open(args.out, "w") as fh:
        json.dump(res, fh, indent=2)
    print("[bench] result:", json.dumps(res, indent=2), flush=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--bridge", required=True)
    ap.add_argument("--prepared", default="/workspace/data/prepared")
    ap.add_argument("--dataset", default="sift250k")
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--nq", type=int, default=4096)
    ap.add_argument("--k-list", type=int, nargs="+", default=[10, 100])
    ap.add_argument("--work-dir", default="/workspace/ab/run")
    ap.add_argument("--template", default=None,
                    help="pre-loaded data dir to copy (avoids re-loading rows)")
    ap.add_argument("--keep-template", action="store_true")
    ap.add_argument("--save-template", default=None,
                    help="after loading the rows, copy the data dir here so later "
                         "runs can reuse it with --template")
    ap.add_argument("--out", default="/workspace/ab/result.json")
    args = ap.parse_args()
    run_bench(args)


if __name__ == "__main__":
    main()
