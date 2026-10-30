#!/usr/bin/env python3
"""Download an ann-benchmarks HDF5 dataset and convert a subset to .fvecs.

The contest only publishes synthetic stand-ins (SIFT250K / Hidden250K), so the
recall sensitivity of a parameter cut has to be calibrated on *real* data:
uniform random vectors are close to the worst case for a CAGRA graph, real
embeddings are far more forgiving.  This script produces .fvecs files that
bench_build.c can consume directly (--base / --query).

Usage:
  python3 real_data.py --name sift-128-euclidean --n 250000
  python3 real_data.py --name nytimes-256-angular --n 250000
"""
import argparse
import os
import struct
import sys
import urllib.request

BASE = "http://ann-benchmarks.com"


def download(name, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 1024:
        print("# cached", dest, os.path.getsize(dest))
        return dest
    url = "%s/%s.hdf5" % (BASE, name)
    tmp = dest + ".part"
    print("# downloading", url)
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            if total:
                pct = 100.0 * got / total
                if got % (32 << 20) < (1 << 20):
                    print("#   %.1f%% %d/%d" % (pct, got, total), flush=True)
    os.replace(tmp, dest)
    print("# downloaded", dest, os.path.getsize(dest))
    return dest


def write_fvecs(path, mat, n):
    import numpy as np
    mat = np.asarray(mat[:n], dtype="<f4")
    n, dim = mat.shape
    with open(path, "wb") as f:
        for row in range(n):
            f.write(struct.pack("<i", dim))
            f.write(mat[row].tobytes())
    print("# wrote", path, os.path.getsize(path), "rows", n, "dim", dim, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--nq", type=int, default=200)
    ap.add_argument("--out-dir", default="/workspace/amd/data")
    args = ap.parse_args()

    import numpy as np
    import h5py

    os.makedirs(args.out_dir, exist_ok=True)
    raw_dir = os.path.join(args.out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)
    h5 = download(args.name, os.path.join(raw_dir, args.name + ".hdf5"))

    with h5py.File(h5, "r") as f:
        keys = list(f.keys())
        print("# keys", keys)
        train = np.array(f["train"] if "train" in keys else f["emb"], dtype=np.float32)
        test = np.array(f["test"] if "test" in keys else train[: args.nq], dtype=np.float32)
    print("# train", train.shape, "test", test.shape)

    tag = args.name.split("-")[0]
    write_fvecs(os.path.join(args.out_dir, tag + "_base.fvecs"), train, args.n)
    write_fvecs(os.path.join(args.out_dir, tag + "_query.fvecs"), test, args.nq)


if __name__ == "__main__":
    sys.exit(main())
