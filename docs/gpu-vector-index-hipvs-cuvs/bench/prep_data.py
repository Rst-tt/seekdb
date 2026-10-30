#!/usr/bin/env python3
"""Prepare A/B datasets for the seekdb x hipVS CAGRA contest workload.

Inputs (downloaded once from HuggingFace, qbo-odp/sift1m):
  sift_base.fvecs        500000 x 128 float32 (fvecs: int32 dim + floats)
  sift_query.fvecs        10000 x 128 float32
  sift_groundtruth.ivecs  10000 x 100 int32 (neighbour ids of sift1m_base)

Outputs (raw row-major float32 / int32):
  sift250k_base.f32    250000 x 128   (rows 0..249999 of sift1m_base)
  sift250k_query.f32     4096 x 128   (first 4096 queries)
  sift250k_gt100.i32     4096 x 100   (brute-force ground truth over sift250k_base)
  hid250k_base.f32     250000 x 256   (proxy of the hidden 250K x 256 set:
                                      [base_A | base_B] with A=rows 0..249999,
                                      B=rows 250000..499999 of sift1m_base)
  hid250k_query.f32      4096 x 256   ([query_A | query_B])
  hid250k_gt100.i32      4096 x 100
"""
import os
import struct
import sys
import numpy as np

SRC = sys.argv[1] if len(sys.argv) > 1 else "/e/AMD/data"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/e/AMD/data/prepared"
N_BASE = 250000
N_QUERY = 4096
TOPK = 100
DIM128 = 128


def read_fvecs(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    dim = struct.unpack("<i", raw[:4])[0]
    n = len(raw) // (4 + 4 * dim)
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(n, 4 + 4 * dim)
    dims = arr[:, :4].copy().view(np.int32).ravel()
    assert (dims == dim).all(), f"mixed dims in {path}"
    return arr[:, 4:].copy().view(np.float32).reshape(n, dim)


def read_ivecs(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    dim = struct.unpack("<i", raw[:4])[0]
    n = len(raw) // (4 + 4 * dim)
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(n, 4 + 4 * dim)
    return arr[:, 4:].copy().view(np.int32).reshape(n, dim)


def brute_force_gt(base, queries, k):
    """Exact L2 top-k ids for every query (chunked, float32 BLAS)."""
    b2 = (base.astype(np.float32) ** 2).sum(axis=1)
    out = np.empty((queries.shape[0], k), dtype=np.int32)
    chunk = 64
    for s in range(0, queries.shape[0], chunk):
        q = queries[s:s + chunk]
        d = b2[None, :] - 2.0 * (q @ base.T)
        idx = np.argpartition(d, k, axis=1)[:, :k]
        rows = np.arange(idx.shape[0])[:, None]
        order = np.argsort(d[rows, idx], axis=1)
        out[s:s + chunk] = idx[rows, order].astype(np.int32)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    base = read_fvecs(os.path.join(SRC, "sift_base.fvecs"))
    queries = read_fvecs(os.path.join(SRC, "sift_query.fvecs"))
    print("sift1m base", base.shape, "queries", queries.shape, flush=True)

    a = np.ascontiguousarray(base[:N_BASE])
    qa = np.ascontiguousarray(queries[:N_QUERY])
    b = np.ascontiguousarray(base[N_BASE:N_BASE + N_BASE])
    qb = np.ascontiguousarray(queries[N_QUERY:N_QUERY + N_QUERY])
    if qb.shape[0] < N_QUERY:  # fall back: tile the query set
        qb = np.ascontiguousarray(np.tile(queries[:N_QUERY], (2, 1))[:N_QUERY])
    hid = np.ascontiguousarray(np.hstack([a, b]))
    hidq = np.ascontiguousarray(np.hstack([qa, qb]))

    np.save(os.path.join(OUT, "sift250k_base.npy"), a)
    np.save(os.path.join(OUT, "sift250k_query.npy"), qa)
    np.save(os.path.join(OUT, "hid250k_base.npy"), hid)
    np.save(os.path.join(OUT, "hid250k_query.npy"), hidq)
    print("saved arrays", flush=True)

    for name, base_arr, q_arr in (("sift250k", a, qa), ("hid250k", hid, hidq)):
        gt = brute_force_gt(base_arr, q_arr, TOPK)
        base_arr.tofile(os.path.join(OUT, f"{name}_base.f32"))
        q_arr.tofile(os.path.join(OUT, f"{name}_query.f32"))
        gt.tofile(os.path.join(OUT, f"{name}_gt100.i32"))
        print(f"{name}: base{base_arr.shape} query{q_arr.shape} gt{gt.shape}", flush=True)


if __name__ == "__main__":
    main()
