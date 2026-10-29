#!/usr/bin/env python3
"""One-shot bootstrap of the AMD contest workspace on a fresh GPU container.

The Radeon Cloud container is ephemeral: /workspace disappears with the
instance, and raw.githubusercontent.com is blocked from inside it, so every
file is pulled through the GitHub Contents API of the (public) fork
Rst-tt/seekdb instead.

    python3 bootstrap.py              # files + compile + smoke test
    python3 bootstrap.py --data       # also fetch + convert real datasets
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import urllib.request

OWNER, REPO = "Rst-tt", "seekdb"
WORK = "/workspace/amd"
HIPVS = "/opt/hipvs"
ROCM = "/opt/rocm"
HIPCC = ROCM + "/bin/hipcc"

# local name -> (repo path, git ref)
FILES = {
    "seekdb_cuvs_bridge.c": ("tools/hipvs_bridge/seekdb_cuvs_bridge.c", "amd-work"),
    "seekdb_cuvs_bridge.h": ("tools/hipvs_bridge/seekdb_cuvs_bridge.h", "amd-cagra-submit"),
    "build.sh": ("tools/hipvs_bridge/build.sh", "amd-cagra-submit"),
    "bench_build.c": ("tools/hipvs_bridge/bench_build.c", "amd-work"),
    "bench_e2e.py": ("tools/hipvs_bridge/bench_e2e.py", "amd-work"),
    "gen_dataset.py": ("tools/hipvs_bridge/gen_dataset.py", "amd-work"),
    "real_data.py": ("tools/hipvs_bridge/real_data.py", "amd-work"),
    "real_sweep.sh": ("tools/hipvs_bridge/real_sweep.sh", "amd-work"),
    "probe_node.sh": ("tools/hipvs_bridge/probe_node.sh", "amd-work"),
    "start_seekdb_node.sh": ("tools/hipvs_bridge/start_seekdb_node.sh", "amd-work"),
}


def fetch(path, ref, dest):
    url = "https://api.github.com/repos/%s/%s/contents/%s?ref=%s" % (OWNER, REPO, path, ref)
    req = urllib.request.Request(url, headers={"User-Agent": "amd-bootstrap"})
    with urllib.request.urlopen(req, timeout=120) as r:
        meta = json.load(r)
    blob = base64.b64decode(meta["content"])
    with open(dest, "wb") as f:
        f.write(blob)
    print("# %-24s %7d B  (%s)" % (os.path.basename(dest), len(blob), ref))


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.stdout.strip():
        print(p.stdout[-4000:])
    if p.stderr.strip():
        print(p.stderr[-4000:], file=sys.stderr)
    if p.returncode != 0:
        raise SystemExit("FAILED rc=%d: %s" % (p.returncode, " ".join(cmd)))
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", action="store_true", help="also download + convert real datasets")
    ap.add_argument("--skip-compile", action="store_true")
    args = ap.parse_args()

    tools = os.path.join(WORK, "tools", "hipvs_bridge")
    for d in (tools, os.path.join(WORK, "build"), os.path.join(WORK, "data"), os.path.join(WORK, "run")):
        os.makedirs(d, exist_ok=True)

    for name, (path, ref) in FILES.items():
        fetch(path, ref, os.path.join(tools, name))

    if not args.skip_compile:
        run([HIPCC, "-shared", "-fPIC", "-O2", "-D__HIP_PLATFORM_AMD__",
             "-I" + HIPVS + "/include", "-I" + ROCM + "/include", "-I" + tools,
             os.path.join(tools, "seekdb_cuvs_bridge.c"),
             "-o", os.path.join(WORK, "build", "libseekdb_cuvs_bridge.so"),
             "-L" + HIPVS + "/lib", "-Wl,-soname,libseekdb_cuvs_bridge.so", "-lcuvs_c"])
        run([HIPCC, "-O2", "-I" + tools, os.path.join(tools, "bench_build.c"),
             "-o", os.path.join(WORK, "build", "bench_build"),
             "-L" + os.path.join(WORK, "build"), "-L" + HIPVS + "/lib",
             "-lseekdb_cuvs_bridge", "-lcuvs_c",
             "-Wl,-rpath," + os.path.join(WORK, "build") + ":" + HIPVS + "/lib"])
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = os.path.join(WORK, "build") + ":" + HIPVS + "/lib:" + ROCM + "/lib"
        env["SEEKDB_CUVS_TIME"] = "1"
        run([os.path.join(WORK, "build", "bench_build"), "--n", "20000", "--dim", "64",
             "--nq", "20", "--topk", "100", "--tag", "smoke"], env=env)

    if args.data:
        py = "/opt/venv/bin/python"
        try:
            import h5py  # noqa: F401
        except Exception:
            run([py, "-m", "pip", "install", "-q", "h5py", "numpy"])
        for name, tag in (("sift-128-euclidean", "sift"), ("nytimes-256-angular", "nytimes")):
            run([py, os.path.join(tools, "real_data.py"), "--name", name,
                 "--n", "250000", "--nq", "200", "--out-dir", os.path.join(WORK, "data")])

    print("# BOOTSTRAP OK")


if __name__ == "__main__":
    main()
