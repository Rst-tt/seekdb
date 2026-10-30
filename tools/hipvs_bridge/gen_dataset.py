#!/usr/bin/env python3
"""Generate synthetic .fvecs datasets for the CAGRA bridge benchmarks.

On the contest node the real SIFT250K / Hidden250K files are provided, so this is
only a fallback for smoke tests and for the small local sanity runs.

    python3 gen_dataset.py --n 250000 --dim 128 --out /data/synth/base.fvecs \
        --queries 200 --queries-out /data/synth/query.fvecs

Queries are base vectors perturbed by --noise, which keeps recall measurements
meaningful without shipping a separate ground-truth file.
"""

import argparse
import random
import struct
import sys


def write_fvecs(path, vectors):
    with open(path, "wb") as f:
        for vec in vectors:
            f.write(struct.pack("i", len(vec)))
            f.write(struct.pack("%df" % len(vec), *vec))


def gen(n, dim, seed, lo=0.0, hi=1.0):
    rnd = random.Random(seed)
    for _ in range(n):
        yield [rnd.uniform(lo, hi) for _ in range(dim)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--out", required=True)
    ap.add_argument("--queries", type=int, default=0)
    ap.add_argument("--queries-out", default="")
    ap.add_argument("--noise", type=float, default=0.01)
    args = ap.parse_args()

    try:
        import numpy as np  # noqa: F401 - optional fast path
    except ImportError:
        np = None

    if np is not None:
        rng = np.random.default_rng(args.seed)
        base = rng.random((args.n, args.dim), dtype=np.float32)
        # .fvecs stores the dimension as int32, so write it separately: packing it
        # as float32 makes readers see a 1124073472-dimension vector.
        hdr = np.int32(args.dim).tobytes()
        with open(args.out, "wb") as f:
            for vec in base:
                f.write(hdr)
                f.write(vec.tobytes())
        if args.queries and args.queries_out:
            pick = rng.integers(0, args.n, size=args.queries)
            noise = rng.normal(0.0, args.noise, size=(args.queries, args.dim)).astype(np.float32)
            qs = base[pick] + noise
            with open(args.queries_out, "wb") as f:
                for vec in qs:
                    f.write(hdr)
                    f.write(vec.astype(np.float32).tobytes())
        print("wrote %d x %d to %s" % (args.n, args.dim, args.out), file=sys.stderr)
        return

    write_fvecs(args.out, gen(args.n, args.dim, args.seed))
    if args.queries and args.queries_out:
        rnd = random.Random(args.seed + 1)
        qs = []
        for _ in range(args.queries):
            qs.append([rnd.uniform(0.0, 1.0) + rnd.uniform(-args.noise, args.noise) for _ in range(args.dim)])
        write_fvecs(args.queries_out, qs)
    print("wrote %d x %d to %s" % (args.n, args.dim, args.out), file=sys.stderr)


if __name__ == "__main__":
    main()
