/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * Contest version (seekdb x AMD hipVS). The exported ABI is a superset of the
 * V5 baseline ABI, so src/oblib/lib/vector/ob_vsag_adaptor.cpp keeps working
 * unchanged and the win is entirely on the cuVS side:
 *
 *  1) CAGRA build/search knobs are overridable at runtime through
 *     SEEKDB_CUVS_* environment variables; the defaults are the values that
 *     were measured to be the best trade-off on gfx1100 (ROCm 7.2.4,
 *     hipVS f96a663). Defaults therefore reproduce the tuned behaviour even
 *     when the evaluation harness runs the bridge with a clean environment.
 *
 *  2) The dataset is uploaded by this bridge through a pinned staging buffer
 *     and handed to cuVS as a *device* DLPack tensor, so cuVS does not copy
 *     the pageable host buffer a second time (122MB pageable H2D = 610ms,
 *     pinned = 5ms on this box).
 *
 *  3) Search buffers, pinned host staging areas and the cuVS search params
 *     object are allocated once per index and reused across calls; the
 *     baseline allocated and freed three device buffers plus a params object
 *     on every single query.
 *
 *  4) The small-batch search kernel is selected automatically: cuVS' AUTO
 *     choice is very slow for the one-row-at-a-time access pattern used by
 *     the evaluation queries (a single CTA per query), while SINGLE_CTA
 *     answers the same query ~6x faster with the same recall. Large batches
 *     keep the library default.
 *
 *  5) Each build prints one timing line so the DDL -> first-query window can
 *     be attributed to phases (upload vs CAGRA build).
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SK_TAG "v4-tuned"

/* Tuned defaults (gfx1100 / ROCm 7.2.4 / hipVS f96a663).
 * Build: NN-descent with the baseline degrees but far fewer iterations.
 * Search: SINGLE_CTA for small batches, baseline itopk/search_width rules. */
#define SK_DEF_BUILD_ALGO   NN_DESCENT
#define SK_DEF_GRAPH_DEGREE 128
#define SK_DEF_INTER_DEGREE 128
#define SK_DEF_NN_NITER     10
#define SK_DEF_SEARCH_ALGO  SINGLE_CTA
#define SK_DEF_SEARCH_ALGO_LARGE MULTI_KERNEL /* only kernel without an itopk cap */
#define SK_DEF_SMALL_NQ     64     /* <= this many queries -> SINGLE_CTA      */
#define SK_DEF_SEARCH_WIDTH 8      /* baseline used 4, 8 buys extra recall     */
#define SK_ITOPK_FACTOR     4      /* itopk = max(512, topk * 4) (baseline)    */
#define SK_ITOPK_CAP        4096   /* hard cap                                  */

/* ------------------------------------------------------------------ config */

typedef struct {
  int    build_algo;        /* cuvsCagraGraphBuildAlgo                     */
  size_t graph_degree;      /* final graph degree                          */
  size_t inter_degree;      /* intermediate graph degree                   */
  size_t nn_niter;          /* NN-descent iterations                       */
  size_t itopk;             /* 0 => keep baseline rule max(512, topk*4)    */
  size_t search_width;      /* 0 => keep baseline rule (4)                 */
  int    algo;              /* cuvsCagraSearchAlgo, 0 => auto by batch size */
  size_t team_size;         /* 0 => auto                                   */
  size_t max_queries;       /* 0 => auto                                   */
  int    persistent;        /* use the persistent search kernel            */
  double persistent_usage;  /* persistent_device_usage                     */
  double persistent_life;   /* persistent_lifetime (seconds)               */
  int    timing;            /* 1 => build lines, 2 => also per-search lines */
  int    device_dataset;    /* 1 => upload the dataset ourselves           */
  size_t max_iterations;    /* 0 => library default                         */
  size_t min_iterations;    /* 0 => library default                         */
  size_t num_random;        /* number of random samplings, 0 => default    */
  int    hashmap_mode;      /* cuvsCagraHashMode, 0 => default              */
  int    unsafe_itopk;      /* 1 => skip the per-kernel itopk clamp         */
} sk_cuvs_cfg;

static sk_cuvs_cfg g_cfg;

static long env_long(const char *key, long dflt) {
  const char *v = getenv(key);
  if (!v || !*v) return dflt;
  char *end = 0;
  long r = strtol(v, &end, 10);
  if (end == v) return dflt;   /* not a number -> keep the default */
  return r;
}

static double env_double(const char *key, double dflt) {
  const char *v = getenv(key);
  if (!v || !*v) return dflt;
  char *end = 0;
  double r = strtod(v, &end);
  if (end == v) return dflt;
  return r;
}

static void sk_load_cfg(void) {
  static int loaded = 0;
  if (loaded) return;
  loaded = 1;
  g_cfg.build_algo       = (int)env_long("SEEKDB_CUVS_BUILD_ALGO", SK_DEF_BUILD_ALGO);
  g_cfg.graph_degree     = (size_t)env_long("SEEKDB_CUVS_GRAPH_DEGREE", SK_DEF_GRAPH_DEGREE);
  g_cfg.inter_degree     = (size_t)env_long("SEEKDB_CUVS_INTER_DEGREE", SK_DEF_INTER_DEGREE);
  g_cfg.nn_niter         = (size_t)env_long("SEEKDB_CUVS_NN_NITER", SK_DEF_NN_NITER);
  g_cfg.itopk            = (size_t)env_long("SEEKDB_CUVS_ITOPK", 0);
  g_cfg.search_width     = (size_t)env_long("SEEKDB_CUVS_SEARCH_WIDTH", 0);
  g_cfg.algo             = (int)env_long("SEEKDB_CUVS_ALGO", 0);
  g_cfg.team_size        = (size_t)env_long("SEEKDB_CUVS_TEAM_SIZE", 0);
  g_cfg.max_queries      = (size_t)env_long("SEEKDB_CUVS_MAX_QUERIES", 0);
  g_cfg.persistent       = (int)env_long("SEEKDB_CUVS_PERSISTENT", 0);
  g_cfg.persistent_usage = env_double("SEEKDB_CUVS_PERSISTENT_USAGE", 0.9);
  g_cfg.persistent_life  = env_double("SEEKDB_CUVS_PERSISTENT_LIFETIME", 60.0);
  g_cfg.timing           = (int)env_long("SEEKDB_CUVS_TIMING", 1);
  g_cfg.device_dataset   = (int)env_long("SEEKDB_CUVS_DEVICE_DATASET", 1);
  g_cfg.max_iterations   = (size_t)env_long("SEEKDB_CUVS_MAX_ITERATIONS", 0);
  g_cfg.min_iterations   = (size_t)env_long("SEEKDB_CUVS_MIN_ITERATIONS", 0);
  g_cfg.num_random       = (size_t)env_long("SEEKDB_CUVS_NUM_RANDOM", 0);
  g_cfg.hashmap_mode     = (int)env_long("SEEKDB_CUVS_HASHMODE", 0);
  g_cfg.unsafe_itopk     = (int)env_long("SEEKDB_CUVS_UNSAFE_ITOPK", 0);
}

/* ------------------------------------------------------------------- index */

typedef struct {
  cuvsResources_t         res;
  cuvsCagraIndexParams_t  ip;
  cuvsCagraSearchParams_t sp;
  cuvsCagraIndex_t        idx;
  long                    n, dim;
  int                     sp_ready;
  /* cached device buffers (reused across searches) */
  float    *d_q;    size_t d_q_cap;
  unsigned *d_ids;  size_t d_ids_cap;
  float    *d_dist; size_t d_dist_cap;
  /* cached pinned host staging (reused across searches) */
  float    *h_q;    size_t h_q_cap;
  unsigned *h_ids;  size_t h_ids_cap;
  float    *h_dist; size_t h_dist_cap;
  /* device copy of the dataset (owned by the index, see sk_upload_dataset) */
  float    *d_base; size_t d_base_bytes;
  /* DLPack shapes must outlive the tensors that point at them */
  int64_t                   shape[2];
  int64_t                   q_shape[2];
  int64_t                   r_shape[2];
  int64_t                   d_shape[2];
} sk_cuvs_index;

const char *seekdb_cuvs_tag(void) { return SK_TAG; }

static double sk_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int sk_ensure_device(void **p, size_t *cap, size_t need, cuvsResources_t res) {
  if (*p && *cap >= need) return 0;
  if (*p) { cuvsRMMFree(res, *p, *cap); *p = 0; *cap = 0; }
  void *buf = 0;
  if (cuvsRMMAlloc(res, &buf, need) != CUVS_SUCCESS) return -1;
  *p = buf;
  *cap = need;
  return 0;
}

static int sk_ensure_pinned(void **p, size_t *cap, size_t need) {
  if (*p && *cap >= need) return 0;
  if (*p) { cudaFreeHost(*p); *p = 0; *cap = 0; }
  void *buf = 0;
#ifdef __HIP_PLATFORM_AMD__
  if (cudaMallocHost(&buf, need, hipHostMallocDefault) != cudaSuccess) return -1;
#else
  if (cudaMallocHost(&buf, need) != cudaSuccess) return -1;
#endif
  *p = buf;
  *cap = need;
  return 0;
}

/* Upload the dataset through pinned memory and return a device DLPack tensor.
 * Returns 0 on success, -1 when the caller should fall back to the host path. */
static int sk_upload_dataset(sk_cuvs_index *h, const float *base, long n, long dim,
                             DLManagedTensor *out) {
  size_t bytes = sizeof(float) * (size_t)n * (size_t)dim;
  void *pinned = 0;
  if (cudaMallocHost(&pinned, bytes
#ifdef __HIP_PLATFORM_AMD__
                     , hipHostMallocDefault
#endif
                     ) != cudaSuccess) {
    return -1;
  }
  memcpy(pinned, base, bytes);
  float *d_base = 0;
  if (cuvsRMMAlloc(h->res, (void **)&d_base, bytes) != CUVS_SUCCESS) {
    cudaFreeHost(pinned);
    return -1;
  }
  if (cudaMemcpy(d_base, pinned, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
    cudaFreeHost(pinned);
    cuvsRMMFree(h->res, d_base, bytes);
    return -1;
  }
  cudaFreeHost(pinned);

  h->shape[0] = (int64_t)n;
  h->shape[1] = (int64_t)dim;
  memset(out, 0, sizeof(*out));
  out->dl_tensor.data = d_base;
  out->dl_tensor.device.device_type = kDLCUDA;
  out->dl_tensor.device.device_id = 0;
  out->dl_tensor.ndim = 2;
  out->dl_tensor.dtype.code = kDLFloat;
  out->dl_tensor.dtype.bits = 32;
  out->dl_tensor.dtype.lanes = 1;
  out->dl_tensor.shape = h->shape;
  out->dl_tensor.strides = 0;
  out->dl_tensor.byte_offset = 0;
  h->d_base = d_base;
  h->d_base_bytes = bytes;
  return 0;
}

static void sk_fill_host_tensor(sk_cuvs_index *h, const float *base, long n, long dim,
                                DLManagedTensor *out) {
  h->shape[0] = (int64_t)n;
  h->shape[1] = (int64_t)dim;
  memset(out, 0, sizeof(*out));
  out->dl_tensor.data = (void *)base;
  out->dl_tensor.device.device_type = kDLCPU;
  out->dl_tensor.device.device_id = 0;
  out->dl_tensor.ndim = 2;
  out->dl_tensor.dtype.code = kDLFloat;
  out->dl_tensor.dtype.bits = 32;
  out->dl_tensor.dtype.lanes = 1;
  out->dl_tensor.shape = h->shape;
  out->dl_tensor.strides = 0;
  out->dl_tensor.byte_offset = 0;
}

/* ------------------------------------------------------------------- build */

void *seekdb_cuvs_build(const float *base, long n, long dim) {
  sk_load_cfg();
  sk_cuvs_index *h = (sk_cuvs_index *)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset;
  sk_fill_host_tensor(h, base, n, dim, &dset);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = (enum cuvsCagraGraphBuildAlgo)g_cfg.build_algo;
  h->ip->graph_degree = g_cfg.graph_degree;
  h->ip->intermediate_graph_degree = g_cfg.inter_degree;
  h->ip->nn_descent_niter = g_cfg.nn_niter;

  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;

  double upload_ms = 0, t0 = 0, t1 = 0;
  int used_device = 0;

  if (g_cfg.device_dataset) {
    double u0 = sk_now_ms();
    DLManagedTensor dev;
    if (sk_upload_dataset(h, base, n, dim, &dev) == 0) {
      upload_ms = sk_now_ms() - u0;
      t0 = sk_now_ms();
      if (cuvsCagraBuild(h->res, h->ip, &dev, h->idx) == CUVS_SUCCESS) {
        used_device = 1;
      } else {
        /* device tensor rejected: fall back to the host tensor */
        fprintf(stderr, "[seekdb_cuvs] device dataset build failed, retrying on host: %s\n",
                cuvsGetLastErrorText());
        if (h->d_base) { cuvsRMMFree(h->res, h->d_base, h->d_base_bytes); h->d_base = 0; }
        h->d_base_bytes = 0;
      }
      t1 = sk_now_ms();
    }
  }

  if (!used_device) {
    if (h->d_base) { cuvsRMMFree(h->res, h->d_base, h->d_base_bytes); h->d_base = 0; h->d_base_bytes = 0; }
    t0 = sk_now_ms();
    if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
    t1 = sk_now_ms();
  }

  if (g_cfg.timing >= 1) {
    fprintf(stdout,
            "[seekdb_cuvs] build n=%ld dim=%ld algo=%d gdeg=%zu ideg=%zu niter=%zu "
            "device=%d upload_ms=%.1f ms=%.1f\n",
            n, dim, g_cfg.build_algo, g_cfg.graph_degree, g_cfg.inter_degree,
            g_cfg.nn_niter, used_device, upload_ms, t1 - t0);
    fflush(stdout);
  }
  h->n = n;
  h->dim = dim;
  return h;

fail_idx:
  cuvsCagraIndexDestroy(h->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(h->ip);
fail_res:
  cuvsResourcesDestroy(h->res);
  free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* ------------------------------------------------------------------ search */

int seekdb_cuvs_search(void *handle, const float *query, long nq, long topk,
                       unsigned int *out_ids, float *out_dist) {
  sk_load_cfg();
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0) return 1;

  size_t q_bytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  size_t r_bytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  size_t d_bytes = sizeof(float) * (size_t)nq * (size_t)topk;
  int rc = 0;
  double t0 = sk_now_ms();

  if (sk_ensure_device((void **)&h->d_q, &h->d_q_cap, q_bytes, h->res) != 0) { rc = 10; goto done; }
  if (sk_ensure_device((void **)&h->d_ids, &h->d_ids_cap, r_bytes, h->res) != 0) { rc = 11; goto done; }
  if (sk_ensure_device((void **)&h->d_dist, &h->d_dist_cap, d_bytes, h->res) != 0) { rc = 12; goto done; }
  if (sk_ensure_pinned((void **)&h->h_q, &h->h_q_cap, q_bytes) != 0) { rc = 14; goto done; }
  if (sk_ensure_pinned((void **)&h->h_ids, &h->h_ids_cap, r_bytes) != 0) { rc = 15; goto done; }
  if (sk_ensure_pinned((void **)&h->h_dist, &h->h_dist_cap, d_bytes) != 0) { rc = 16; goto done; }

  memcpy(h->h_q, query, q_bytes);
  if (cudaMemcpy(h->d_q, h->h_q, q_bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = 13; goto done; }

  {
    DLManagedTensor qt, nt, dt;
    h->q_shape[0] = nq; h->q_shape[1] = h->dim;
    h->r_shape[0] = nq; h->r_shape[1] = topk;
    h->d_shape[0] = nq; h->d_shape[1] = topk;
    memset(&qt, 0, sizeof(qt));
    qt.dl_tensor.data = h->d_q;
    qt.dl_tensor.device.device_type = kDLCUDA;
    qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2;
    qt.dl_tensor.dtype.code = kDLFloat;
    qt.dl_tensor.dtype.bits = 32;
    qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = h->q_shape;
    nt = qt;
    nt.dl_tensor.data = h->d_ids;
    nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.shape = h->r_shape;
    dt = qt;
    dt.dl_tensor.data = h->d_dist;
    dt.dl_tensor.dtype.code = kDLFloat;
    dt.dl_tensor.shape = h->d_shape;

    if (!h->sp_ready) {
      if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc = 17; goto done; }
      h->sp_ready = 1;
    }
    {
      /* itopk rule: same as the baseline, max(512, topk*4).  The single-CTA
       * search kernel that the one-row query path uses only supports an
       * internal candidate list of 512 entries (search_single_cta.cuh:
       * max_itopk = 512); asking for more does not raise an error in the
       * release build, it silently returns wrong neighbours.  The extra
       * search budget that buys back the recall of the cheap graph build is
       * therefore spent on search_width / max_iterations instead.  An
       * explicit SEEKDB_CUVS_ITOPK overrides the rule. */
      size_t required_itopk = (size_t)topk * SK_ITOPK_FACTOR;
      if (required_itopk < 512) required_itopk = 512;
      if (required_itopk > SK_ITOPK_CAP) required_itopk = SK_ITOPK_CAP;
      if (g_cfg.itopk) {
        h->sp->itopk_size = g_cfg.itopk;
      } else {
        h->sp->itopk_size = required_itopk;
      }
      /* baseline rule: search_width is pinned to 4 (cuVS default is 1 and is
       * ~2x slower on the nq=1 path); we use 8 for extra recall. */
      h->sp->search_width = g_cfg.search_width ? g_cfg.search_width : (size_t)SK_DEF_SEARCH_WIDTH;
      /* Search kernel selection: the evaluation issues one row at a time.
       * cuVS' AUTO choice spawns a full grid per query, which is ~6x slower
       * than the single-CTA kernel for that access pattern and returns the
       * same neighbours. Large batches keep the library default. */
      /* Kernel selection. cuVS enforces hard limits on the internal top-k
       * buffer (search_single_cta.cuh: itopk <= 512, search_plan.cuh:
       * itopk <= 1024 for AUTO, search_multi_cta.cuh: result buffer <= 256)
       * and a rejected search makes seekdb fall back to the CPU (VSAG)
       * index, which would break the "no CPU fallback" gate. The limits are
       * therefore enforced here, before the search is launched. */
      if (g_cfg.algo) {
        /* Explicit override: honour it verbatim (an explicit override may
         * exceed the cuVS limits, which is what the parameter sweep wants). */
        h->sp->algo = (enum cuvsCagraSearchAlgo)g_cfg.algo;
      } else {
        if (nq <= SK_DEF_SMALL_NQ) {
          /* One row at a time: the single-CTA kernel answers a single query
           * with one CTA and no per-call device allocation, which is what the
           * seekdb one-row query path needs. */
          h->sp->algo = SK_DEF_SEARCH_ALGO;
        } else if (h->sp->itopk_size > 1024) {
          h->sp->algo = SK_DEF_SEARCH_ALGO_LARGE; /* MULTI_KERNEL: no itopk cap */
        } else {
          h->sp->algo = AUTO;
        }
        if (h->sp->algo == AUTO && h->sp->itopk_size > 1024) {
          h->sp->itopk_size = 1024;
        }
      }
      /* Hard limits of the cuVS search kernels.  Violating them does not
       * raise an error in the release build: the kernel reads past its
       * internal buffer and silently returns wrong neighbours (measured on
       * the hard 250K x 256 dataset: recall@100 0.29 instead of 0.90), and
       * a failing search would make seekdb fall back to the CPU (VSAG)
       * index, which breaks the "no CPU fallback" gate. */
      if (!g_cfg.unsafe_itopk) {
        if (h->sp->algo == SINGLE_CTA && h->sp->itopk_size > 512) {
          h->sp->itopk_size = 512;
        }
        if (h->sp->algo == MULTI_CTA && h->sp->itopk_size > 256) {
          h->sp->itopk_size = 256;
        }
      }
      if (g_cfg.max_iterations) h->sp->max_iterations = g_cfg.max_iterations;
      if (g_cfg.min_iterations) h->sp->min_iterations = g_cfg.min_iterations;
      if (g_cfg.num_random) h->sp->num_random_samplings = g_cfg.num_random;
      if (g_cfg.hashmap_mode) h->sp->hashmap_mode = (enum cuvsCagraHashMode)g_cfg.hashmap_mode;
      if (g_cfg.team_size) h->sp->team_size = g_cfg.team_size;
      if (g_cfg.max_queries) h->sp->max_queries = g_cfg.max_queries;
      h->sp->persistent = g_cfg.persistent ? true : false;
      if (g_cfg.persistent) {
        h->sp->persistent_lifetime = (float)g_cfg.persistent_life;
        h->sp->persistent_device_usage = (float)g_cfg.persistent_usage;
      }
    }
    cuvsFilter f;
    f.type = NO_FILTER;
    f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) {
      fprintf(stderr,
              "[seekdb_cuvs] search failed rc=%d nq=%ld topk=%ld itopk=%zu width=%zu algo=%d: %s\n",
              rc, nq, topk, h->sp ? h->sp->itopk_size : (size_t)0,
              h->sp ? h->sp->search_width : (size_t)0, (int)(h->sp ? h->sp->algo : 0),
              cuvsGetLastErrorText());
      rc = 18; goto done;
    }
    if (cudaMemcpy(h->h_ids, h->d_ids, r_bytes, cudaMemcpyDeviceToHost) != cudaSuccess) { rc = 19; goto done; }
    if (out_dist) {
      if (cudaMemcpy(h->h_dist, h->d_dist, d_bytes, cudaMemcpyDeviceToHost) != cudaSuccess) { rc = 20; goto done; }
    }
    memcpy(out_ids, h->h_ids, r_bytes);
    if (out_dist) memcpy(out_dist, h->h_dist, d_bytes);
  }
done:
  if (g_cfg.timing >= 2) {
    fprintf(stdout, "[seekdb_cuvs] search nq=%ld topk=%ld itopk=%zu width=%zu ms=%.3f rc=%d\n",
            nq, topk, h->sp ? h->sp->itopk_size : (size_t)0,
            h->sp ? h->sp->search_width : (size_t)0, sk_now_ms() - t0, rc);
    fflush(stdout);
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

/* -------------------------------------------------------------------- free */

void seekdb_cuvs_free(void *handle) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h) return;
  if (h->d_q) cuvsRMMFree(h->res, h->d_q, h->d_q_cap);
  if (h->d_ids) cuvsRMMFree(h->res, h->d_ids, h->d_ids_cap);
  if (h->d_dist) cuvsRMMFree(h->res, h->d_dist, h->d_dist_cap);
  if (h->d_base) cuvsRMMFree(h->res, h->d_base, h->d_base_bytes);
  if (h->h_q) cudaFreeHost(h->h_q);
  if (h->h_ids) cudaFreeHost(h->h_ids);
  if (h->h_dist) cudaFreeHost(h->h_dist);
  if (h->sp_ready) cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

/* --------------------------------------------------------------- serialize */

int seekdb_cuvs_serialize(void *handle, const char *filename) {
  sk_cuvs_index *index = (sk_cuvs_index *)handle;
  if (!index || !filename) return 1;
  int include_dataset = (int)env_long("SEEKDB_CUVS_SERIALIZE_DATASET", 1);
  if (cuvsCagraSerialize(index->res, filename, index->idx, include_dataset ? true : false) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void *seekdb_cuvs_deserialize(const char *filename, long n, long dim) {
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index *index = (sk_cuvs_index *)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
    index->n = n;
    index->dim = dim;
    return index;
  }
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  free(index);
  return NULL;
}

/* ------------------------------------------------------------- one shot knn
 * Kept for ABI compatibility with the V5 baseline bridge (the seekdb
 * adaptor references this symbol). */
int seekdb_cuvs_cagra_knn(const float *base, long n, long dim,
                           const float *query, long nq, long topk,
                           unsigned int *out_ids) {
  void *handle = seekdb_cuvs_build(base, n, dim);
  if (!handle) return -1;
  int rc = seekdb_cuvs_search(handle, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(handle);
  return rc;
}
