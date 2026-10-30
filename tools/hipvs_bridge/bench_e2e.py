#!/usr/bin/env python3
"""End-to-end DDL timing for the AMD CAGRA contest, measured through SQL.

The official harness loads the raw vector table (no index, no cache), then times

    CREATE VECTOR INDEX idx_items ON items(v)
      WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);

up to the first APPROXIMATE query returning a valid result.  This script does the
same through pymysql so a candidate bridge .so can be measured without the contest
harness, and it can sweep build parameters by rewriting the dev-only conf file
(SEEKDB_CUVS_CONF) that the bridge reads on every build.

Usage on a GPU node:

    # one fresh server, data loaded once, then one timed DDL per config
    python3 bench_e2e.py --port 2881 --n 250000 --dim 128 \
        --base /data/sift/base.fvecs \
        --conf "fp16=0" --conf "fp16=1,intermediate_degree=128,degree=96,niter=60"

Recall is measured only with --recall: approximate top-100 vs exact top-100 for
--nq queries, i.e. the same comparison the graders make.
"""

import argparse
import os
import random
import struct
import sys
import time

try:
    import pymysql
except ImportError:  # pragma: no cover - node bootstrap
    sys.exit("need pymysql: pip3 install --user pymysql")


DDL = (
    "CREATE VECTOR INDEX idx_items ON items(v) "
    "WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64)"
)


def ddl_for(args):
    """The graded DDL is lib=cuvs/hnsw; --lib vsag is only for local dry runs."""
    if args.lib == "cuvs" and args.index_type == "hnsw":
        return DDL
    return (
        "CREATE VECTOR INDEX idx_items ON items(v) "
        "WITH (distance=l2, type=%s, lib=%s, m=16, ef_construction=200, ef_search=64)"
        % (args.index_type, args.lib)
    )
DROP_DDL = "DROP INDEX idx_items ON items"


def vec_literal(vec, prec=4):
    return "'[" + ",".join(("%." + str(prec) + "f") % x for x in vec) + "]'"


def read_fvecs(path, limit):
    """SIFT .fvecs is [int32 dim][float dim] per vector."""
    out = []
    with open(path, "rb") as f:
        while len(out) < limit:
            head = f.read(4)
            if len(head) < 4:
                break
            (dim,) = struct.unpack("i", head)
            body = f.read(4 * dim)
            if len(body) < 4 * dim:
                break
            out.append(struct.unpack("%df" % dim, body))
    return out


def random_vectors(n, dim, seed=1234):
    rnd = random.Random(seed)
    return [[rnd.uniform(0.0, 1.0) for _ in range(dim)] for _ in range(n)]


def connect(args):
    return pymysql.connect(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        database=args.database,
        autocommit=True,
        local_infile=True,
        charset="utf8mb4",
    )


def wait_ready(args, timeout=180):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            conn = connect(args)
            with conn.cursor() as cur:
                cur.execute("select 1")
            conn.close()
            return True
        except Exception as exc:  # noqa: BLE001 - server may still be booting
            sys.stderr.write("waiting for server: %s\n" % exc)
            time.sleep(3)
    return False


def load_rows(cur, args, vectors, dim):
    """Load once.  LOAD DATA INFILE keeps the load phase comparable to the
    official harness; executemany is the fallback when the server refuses it."""
    path = args.tsv
    if not os.path.exists(path) or args.rewrite_tsv:
        with open(path, "w", encoding="utf-8") as f:
            for i, vec in enumerate(vectors, 1):
                f.write(str(i))
                f.write("\t")
                f.write("[")
                f.write(",".join("%.6f" % x for x in vec))
                f.write("]\n")
    try:
        cur.execute("LOAD DATA INFILE %s INTO TABLE items", (path,))
        return "load_data_infile"
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write("LOAD DATA failed (%s), falling back to batched INSERT\n" % exc)
    rows = [(i, "[" + ",".join("%.6f" % x for x in vec) + "]") for i, vec in enumerate(vectors, 1)]
    for off in range(0, len(rows), 5000):
        chunk = rows[off:off + 5000]
        cur.executemany("INSERT INTO items (id, v) VALUES (%s, %s)", chunk)
    return "insert"


def ensure_table(cur, args, dim):
    cur.execute("DROP TABLE IF EXISTS items")
    cur.execute("CREATE TABLE items (id INT PRIMARY KEY, v VECTOR(%d))" % dim)


def write_conf(args, spec):
    if not spec:
        if os.path.exists(args.conf_file):
            os.remove(args.conf_file)
        return
    with open(args.conf_file, "w", encoding="utf-8") as f:
        for part in spec.split(","):
            if not part.strip():
                continue
            key, _, val = part.partition("=")
            f.write("%s=%s\n" % (key.strip(), val.strip()))


def timed_ddl(conn, args, spec):
    write_conf(args, spec)
    with conn.cursor() as cur:
        try:
            cur.execute(DROP_DDL)
        except Exception:  # noqa: BLE001 - index may not exist yet
            pass
        cur.execute("select 1")
        t0 = time.time()
        cur.execute(ddl_for(args))
        ddl_ms = (time.time() - t0) * 1000.0
        t1 = time.time()
        cur.execute(
            "SELECT id FROM items ORDER BY l2_distance(v, %s) APPROXIMATE LIMIT 10".replace(
                "%s", vec_literal(args.probe_vec)
            )
        )
        ids = [r[0] for r in cur.fetchall()]
        first_ms = (time.time() - t1) * 1000.0
    total_ms = ddl_ms + first_ms
    integrity = len(ids) == 10 and len(set(ids)) == 10 and all(1 <= i <= args.n for i in ids)
    return ddl_ms, first_ms, total_ms, integrity


def recall_of(conn, args, queries, k=100):
    hits10 = 0
    hits100 = 0
    total = 0
    gt_total = 0
    with conn.cursor() as cur:
        for q in queries:
            lit = vec_literal(q)
            cur.execute(
                "SELECT id FROM items ORDER BY l2_distance(v, %s) APPROXIMATE LIMIT ".replace(
                    "%s", lit
                )
                + str(k)
            )
            approx = [r[0] for r in cur.fetchall()]
            cur.execute(
                "SELECT id FROM items ORDER BY l2_distance(v, %s) LIMIT ".replace("%s", lit)
                + str(k)
            )
            exact = [r[0] for r in cur.fetchall()]
            if not exact:
                continue
            gt_total += 1
            top10 = set(exact[:10])
            all100 = set(exact[:k])
            hits10 += len(top10.intersection(approx[:10])) / 10.0
            hits100 += len(all100.intersection(approx[:k])) / float(k)
            total += 1
    if not total:
        return 0.0, 0.0
    return hits10 / total, hits100 / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2881)
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="")
    ap.add_argument("--database", default="oceanbase")
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--base", default="", help=".fvecs file with base vectors")
    ap.add_argument("--queries", default="", help=".fvecs file with query vectors")
    ap.add_argument("--nq", type=int, default=20, help="queries used for --recall")
    ap.add_argument("--repeat", type=int, default=1, help="timed DDL rounds per config")
    ap.add_argument("--tag", default="")
    ap.add_argument("--tsv", default="/tmp/seekdb_items.tsv")
    ap.add_argument("--rewrite-tsv", action="store_true")
    ap.add_argument("--conf-file", default="/tmp/seekdb_cuvs.conf")
    ap.add_argument("--conf", action="append", default=[], help="e.g. 'fp16=1,niter=20'")
    ap.add_argument("--recall", action="store_true")
    ap.add_argument("--lib", default="cuvs", choices=["cuvs", "vsag"])
    ap.add_argument("--index-type", default="hnsw", dest="index_type")
    ap.add_argument("--skip-load", action="store_true")
    ap.add_argument("--keep", action="store_true", help="do not drop the table at the end")
    args = ap.parse_args()

    if not wait_ready(args):
        sys.exit("server not ready on port %d" % args.port)

    conn = connect(args)
    if args.base:
        base = read_fvecs(args.base, args.n)
        args.dim = len(base[0])
    else:
        base = random_vectors(args.n, args.dim)
    if args.queries:
        query_pool = read_fvecs(args.queries, args.nq)
    else:
        rnd = random.Random(7)
        query_pool = []
        for _ in range(max(args.nq, 1)):
            src = base[rnd.randrange(len(base))]
            query_pool.append([x + rnd.uniform(-0.01, 0.01) for x in src])
    args.probe_vec = query_pool[0]

    with conn.cursor() as cur:
        if not args.skip_load:
            t0 = time.time()
            ensure_table(cur, args, args.dim)
            how = load_rows(cur, args, base, args.dim)
            load_s = time.time() - t0
            cur.execute("SELECT count(*) FROM items")
            rows = cur.fetchone()[0]
            print("# loaded %d rows (%s) in %.1fs" % (rows, how, load_s), file=sys.stderr)
        else:
            cur.execute("SELECT count(*) FROM items")
            rows = cur.fetchone()[0]
            print("# reusing existing table with %d rows" % rows, file=sys.stderr)

    specs = args.conf or [""]
    for spec in specs:
        for rep in range(args.repeat):
            ddl_ms, first_ms, total_ms, integrity = timed_ddl(conn, args, spec)
            rec10 = rec100 = -1.0
            if args.recall:
                rec10, rec100 = recall_of(conn, args, query_pool)
            print(
                '{"tag":"%s","conf":"%s","rep":%d,"n":%d,"dim":%d,'
                '"ddl_ms":%.1f,"first_query_ms":%.1f,"total_ms":%.1f,'
                '"result_integrity":%s,"recall@10":%.4f,"recall@100":%.4f}'
                % (
                    args.tag,
                    spec,
                    rep,
                    args.n,
                    args.dim,
                    ddl_ms,
                    first_ms,
                    total_ms,
                    "true" if integrity else "false",
                    rec10,
                    rec100,
                )
            )
            sys.stdout.flush()

    with conn.cursor() as cur:
        try:
            cur.execute(DROP_DDL)
        except Exception:  # noqa: BLE001
            pass
        if not args.keep:
            cur.execute("DROP TABLE IF EXISTS items")
    conn.close()


if __name__ == "__main__":
    main()
