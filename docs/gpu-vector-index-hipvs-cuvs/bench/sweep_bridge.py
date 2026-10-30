#!/usr/bin/env python3
"""Sweep build/search parameters directly against the bridge (no SQL, no observer).

Every configuration is a set of SEEKDB_CUVS_* environment variables; the same
bridge binary implements all of them, so no relinking is needed between runs.

The harness is deliberately paranoid: the previous sweep silently measured the
*baseline* bridge for every configuration (both bridges export the same SONAME,
so the loader picked whichever came first).  Now it

  * pins LD_LIBRARY_PATH to the bridge directory of the configuration,
  * prints and records the bridge version tag (seekdb_cuvs_tag()),
  * parses the "[seekdb_cuvs] build ..." line the bridge prints and compares the
    *applied* parameters against the *requested* ones, and
  * marks a configuration as MISMATCH when they differ.

usage: sweep_bridge.py configs.json [--out summary.csv]
"""
import argparse
import json
import os
import re
import subprocess
import sys

P = "/workspace/data/prepared"
BRIDGE_BIN = "/workspace/harness/bench_bridge_v3"
BASE_BIN = "/workspace/harness/bench_bridge_base"

# requested env var -> key inside the bridge's build line
BUILD_ENV_TO_FIELD = {
    "SEEKDB_CUVS_BUILD_ALGO": "algo",
    "SEEKDB_CUVS_GRAPH_DEGREE": "gdeg",
    "SEEKDB_CUVS_INTER_DEGREE": "ideg",
    "SEEKDB_CUVS_NN_NITER": "niter",
}
BUILD_FIELD_ORDER = ["algo", "gdeg", "ideg", "niter"]


def parse_build_line(out):
    """-> dict of the parameters the bridge actually used, or None."""
    m = re.search(
        r"\[seekdb_cuvs\] build n=(\d+) dim=(\d+) algo=(\d+) gdeg=(\d+) ideg=(\d+) niter=(\d+)",
        out)
    if not m:
        return None
    keys = ["n", "dim", "algo", "gdeg", "ideg", "niter"]
    vals = dict(zip(keys, m.groups()))
    for k in ("n", "dim", "algo", "gdeg", "ideg", "niter"):
        vals[k] = int(vals[k])
    return vals


def check_match(cfg, applied):
    """Compare requested env knobs against the applied build parameters."""
    if applied is None:
        return "no-build-line"
    problems = []
    for env_key, field in BUILD_ENV_TO_FIELD.items():
        if env_key in cfg.get("env", {}) and str(applied[field]) != str(cfg["env"][env_key]):
            problems.append("%s: want %s got %s" % (field, cfg["env"][env_key], applied[field]))
    return ";".join(problems) if problems else "ok"


def run_one(cfg):
    name = cfg["name"]
    binary = cfg.get("binary", BRIDGE_BIN)
    ds = cfg.get("dataset", "sift250k")
    dim = cfg.get("dim", 128)
    n = cfg.get("n", 250000)
    nq = cfg.get("nq", 256)
    topk = cfg.get("topk", [10, 100])
    build_reps = cfg.get("build_reps", 1)
    env = dict(os.environ)
    env.update({k: str(v) for k, v in cfg.get("env", {}).items()})
    bridge_dir = cfg.get("bridge_dir")
    if bridge_dir:
        env["LD_LIBRARY_PATH"] = bridge_dir + ":" + env.get("LD_LIBRARY_PATH", "")
    cmd = [binary,
           "--base", "%s/%s_base.f32" % (P, ds),
           "--query", "%s/%s_query.f32" % (P, ds),
           "--gt", "%s/%s_gt100.i32" % (P, ds),
           "--n", str(n), "--dim", str(dim), "--nq", str(nq),
           "--topk", *[str(k) for k in topk],
           "--build-reps", str(build_reps)]
    print("=== [%s] %s" % (name, json.dumps(cfg.get("env", {}))), flush=True)
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           timeout=cfg.get("timeout", 600))
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        res = {"name": name, "env": cfg.get("env", {}), "rc": -9, "timeout": True,
               "stdout_tail": out[-800:]}
        print("   " + json.dumps({k: v for k, v in res.items() if k != "env"}), flush=True)
        return res
    out = p.stdout
    m = re.search(r"^\{\n.*?^\}\n", out, re.S | re.M)
    res = {"name": name, "env": cfg.get("env", {}), "rc": p.returncode}
    if m:
        try:
            res.update(json.loads(m.group(0)))
        except Exception as e:
            res["parse_error"] = repr(e)
    else:
        res["stdout_tail"] = out[-800:]
        res["stderr_tail"] = p.stderr[-800:]
    builds = re.findall(r"build #\d+ ms=([\d.]+)", out)
    if builds:
        res["builds_ms"] = [float(x) for x in builds]
    tag = re.search(r"bridge_tag=(\S+)", out)
    res["tag"] = tag.group(1) if tag else ""
    applied = parse_build_line(out)
    res["applied"] = applied
    res["match"] = check_match(cfg, applied)
    if res["match"] != "ok":
        print("   !! PARAMETER MISMATCH: %s (applied=%s)" % (res["match"], applied), flush=True)
    print("   " + json.dumps({k: v for k, v in res.items() if k != "env"}), flush=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("configs")
    ap.add_argument("--out", default="/workspace/ab/bsweep.csv")
    args = ap.parse_args()
    cfgs = json.load(open(args.configs))
    rows = []
    for c in cfgs:
        rows.append(run_one(c))
    cols = ["name", "build_ms", "build_ms_cold", "k10_qps", "k10_latency_ms",
            "k10_recall", "k100_qps", "k100_latency_ms", "k100_recall", "rc",
            "tag", "match", "applied"]
    with open(args.out, "w") as fh:
        fh.write(",".join(cols) + "\n")
        for r in rows:
            fh.write(",".join(str(r.get(c, "")).replace(",", ";") for c in cols) + "\n")
    print("\n=== SUMMARY ===")
    for r in rows:
        k10 = r.get("k10", {})
        k100 = r.get("k100", {})
        print("%-18s build=%8s  k10: %8s q/s rec=%s   k100: %8s q/s rec=%s   [%s] %s" % (
            r["name"], r.get("build_ms", ""), k10.get("qps", ""), k10.get("recall", ""),
            k100.get("qps", ""), k100.get("recall", ""), r.get("tag", ""), r.get("match", "")))


if __name__ == "__main__":
    main()
