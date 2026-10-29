/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * Contest build: adds (a) env-tunable CAGRA build parameters, (b) an optional
 * FP16 dataset build path, (c) phase timers, so the same .so can be benchmarked
 * without recompiling seekdb.
 *
 *   SEEKDB_CUVS_FP16=1                 build/search with a float16 dataset
 *   SEEKDB_CUVS_GRAPH_DEGREE=128       final graph degree
 *   SEEKDB_CUVS_INTERMEDIATE_DEGREE=256 intermediate graph degree
 *   SEEKDB_CUVS_NITER=100              nn-descent iterations
 *   SEEKDB_CUVS_TIME=1                 print phase timings to stderr
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
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  int  fp16;
  /* search-side caches: device buffers are per thread, params are shared and
   * only read after creation, so a plain mutex around creation is enough. */
  pthread_mutex_t        mu;
  cuvsCagraSearchParams_t sp;
  int                    sp_topk;
} sk_cuvs_index;

static int sk_env_int(const char* name, int defv)
{
  const char* s = getenv(name);
  return (s && *s) ? atoi(s) : defv;
}

/* Dev-only override file: when SEEKDB_CUVS_CONF points at an existing file whose
 * lines look like "graph_degree=96", that value wins over the compiled default
 * but loses to an explicit environment variable. It lets a single running seekdb
 * sweep build parameters by rewriting the file, instead of restarting the server
 * and re-loading 250K rows for every candidate. */
static int sk_conf_int(const char* name, const char* key, int defv)
{
  const char* s = getenv(name);
  const char* path;
  FILE* f;
  char line[256];
  int v = defv;
  size_t klen;
  if (s && *s) {
    return atoi(s);
  }
  path = getenv("SEEKDB_CUVS_CONF");
  if (!path || !*path) {
    return defv;
  }
  f = fopen(path, "r");
  if (!f) {
    return defv;
  }
  klen = strlen(key);
  while (fgets(line, (int)sizeof(line), f)) {
    char* p = line;
    char* e;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (strncmp(p, key, klen) != 0 || p[klen] != '=') {
      continue;
    }
    e = p + klen + 1;
    while (*e == ' ' || *e == '\t') {
      ++e;
    }
    v = atoi(e);
  }
  fclose(f);
  return v;
}

static double sk_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void sk_log(const char* what, double t0)
{
  if (!sk_env_int("SEEKDB_CUVS_TIME", 0)) {
    return;
  }
  fprintf(stderr, "[seekdb_cuvs] %s %.1f ms\n", what, sk_now_ms() - t0);
}

/* ---- search scratch: one small set of device buffers per (thread, index) ----
 *
 * A single-row approximate query is a few hundred microseconds of GPU work, so
 * allocating and freeing the query / id / distance tensors on every call costs
 * more than the search itself.  Keep the buffers in thread-local storage and
 * grow them on demand; the search params object is cached on the index.
 */
typedef struct {
  void*     qd;
  size_t    qcap;
  unsigned* nd;
  size_t    ncap;
  float*    dd;
  size_t    dcap;
} sk_scratch;

typedef struct sk_tls_node {
  void*              handle;
  sk_scratch         s;
  struct sk_tls_node* next;
} sk_tls_node;

static __thread sk_tls_node* g_tls_scratch = NULL;

static int sk_scratch_get(sk_cuvs_index* h, long nq, long topk, sk_scratch** out)
{
  size_t q_bytes = (h->fp16 ? sizeof(uint16_t) : sizeof(float)) * (size_t)nq * (size_t)h->dim;
  size_t n_bytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  size_t d_bytes = sizeof(float) * (size_t)nq * (size_t)topk;
  sk_tls_node* e = NULL;
  for (e = g_tls_scratch; e != NULL; e = e->next) {
    if (e->handle == (void*)h) {
      break;
    }
  }
  if (e == NULL) {
    e = (sk_tls_node*)calloc(1, sizeof(*e));
    if (e == NULL) {
      return 1;
    }
    e->handle = (void*)h;
    e->next = g_tls_scratch;
    g_tls_scratch = e;
  }
  if (e->s.qcap < q_bytes) {
    if (e->s.qd) { cuvsRMMFree(h->res, e->s.qd, e->s.qcap); e->s.qd = NULL; e->s.qcap = 0; }
    if (cuvsRMMAlloc(h->res, &e->s.qd, q_bytes) != CUVS_SUCCESS) { return 2; }
    e->s.qcap = q_bytes;
  }
  if (e->s.ncap < n_bytes) {
    if (e->s.nd) { cuvsRMMFree(h->res, e->s.nd, e->s.ncap); e->s.nd = NULL; e->s.ncap = 0; }
    if (cuvsRMMAlloc(h->res, (void**)&e->s.nd, n_bytes) != CUVS_SUCCESS) { return 3; }
    e->s.ncap = n_bytes;
  }
  if (e->s.dcap < d_bytes) {
    if (e->s.dd) { cuvsRMMFree(h->res, e->s.dd, e->s.dcap); e->s.dd = NULL; e->s.dcap = 0; }
    if (cuvsRMMAlloc(h->res, (void**)&e->s.dd, d_bytes) != CUVS_SUCCESS) { return 4; }
    e->s.dcap = d_bytes;
  }
  *out = &e->s;
  return 0;
}

static cuvsCagraSearchParams_t sk_search_params(sk_cuvs_index* h, long topk)
{
  size_t required_itopk = (size_t)topk * 4;
  cuvsCagraSearchParams_t sp = NULL;
  const char* ev = NULL;
  int width = 4;
  int itopk_env = 0;
  if (required_itopk < 512) {
    required_itopk = 512;
  }
  /* Search side knobs: a graph built with fewer NN-descent iterations loses recall at
   * the top of the list; widening the beam at query time wins most of it back.  These
   * are process-wide (env) so that a deployment can trade query latency for recall. */
  if ((ev = getenv("SEEKDB_CUVS_SEARCH_WIDTH")) != NULL && ev[0] != '\0') {
    int w = atoi(ev);
    if (w >= 1 && w <= 64) {
      width = w;
    }
  }
  if ((ev = getenv("SEEKDB_CUVS_ITOPK")) != NULL && ev[0] != '\0') {
    int it = atoi(ev);
    if (it >= 1 && it <= 65536) {
      itopk_env = it;
    }
  }
  pthread_mutex_lock(&h->mu);
  if (h->sp == NULL || h->sp_topk != (int)topk) {
    if (h->sp) {
      cuvsCagraSearchParamsDestroy(h->sp);
      h->sp = NULL;
    }
    if (cuvsCagraSearchParamsCreate(&h->sp) == CUVS_SUCCESS) {
      h->sp->search_width = width;
      if (itopk_env > 0) {
        h->sp->itopk_size = itopk_env;
      } else if ((size_t)h->sp->itopk_size < required_itopk) {
        h->sp->itopk_size = (int)required_itopk;
      }
      h->sp_topk = (int)topk;
    }
  }
  sp = h->sp;
  pthread_mutex_unlock(&h->mu);
  return sp;
}

/* round-to-nearest-even float -> IEEE half, portable C */
static uint16_t sk_f2h(float f)
{
  union { float f; uint32_t u; } v;
  uint32_t x, sign, mant;
  int32_t exp;
  v.f = f;
  x = v.u;
  sign = (x >> 16) & 0x8000u;
  mant = x & 0x7fffffu;
  if (((x >> 23) & 0xffu) == 0xffu) {
    return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0));
  }
  exp = (int32_t)((x >> 23) & 0xffu) - 127 + 15;
  if (exp >= 0x1f) {
    return (uint16_t)(sign | 0x7c00u);
  }
  if (exp <= 0) {
    uint32_t shift, r, rem, half;
    if (exp < -10) {
      return (uint16_t)sign;
    }
    mant |= 0x800000u;
    shift = (uint32_t)(14 - exp);
    r = mant >> shift;
    rem = mant & ((1u << shift) - 1u);
    half = 1u << (shift - 1u);
    if (rem > half || (rem == half && (r & 1u))) {
      r++;
    }
    return (uint16_t)(sign | r);
  }
  {
    uint32_t r = sign | ((uint32_t)exp << 10) | (mant >> 13);
    uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) {
      r++;
    }
    return (uint16_t)r;
  }
}

static void sk_set_dset(DLManagedTensor* d, void* data, int bits, int dev,
                        long* shape)
{
  d->dl_tensor.data = data;
  d->dl_tensor.device.device_type = dev;
  d->dl_tensor.device.device_id = 0;
  d->dl_tensor.ndim = 2;
  d->dl_tensor.dtype.code = kDLFloat;
  d->dl_tensor.dtype.bits = (uint8_t)bits;
  d->dl_tensor.dtype.lanes = 1;
  d->dl_tensor.shape = shape;
  d->dl_tensor.strides = NULL;
  d->dl_tensor.byte_offset = 0;
}

/* Dim aware build defaults.
 *
 * The stock bridge pins NN_DESCENT / graph_degree 128 / intermediate 256 /
 * niter 100 for every dataset, which is far more work than the graph needs.
 * Measured on 250K points (see CONTEST_REPORT.md):
 *   dim 128: 128/192/35 builds 2.49x faster with recall@10 0.9980 and
 *            recall@100 0.9855 against stock 1.0000 / 0.9887, i.e. the drop
 *            stays inside the 0.005 gate even on the hardest proxy data.
 *   dim 256: every cut we measured (niter 30/50/70, intermediate 192/224)
 *            loses 0.010-0.045 recall@10, so wide vectors keep the stock
 *            parameters and only benefit from the bridge's other changes.
 */
static void sk_tune_defaults(long dim, int* deg, int* inter, int* niter)
{
  if (dim <= 160) {
    *deg = 128;
    *inter = 192;
    *niter = 35;
  } else {
    *deg = 128;
    *inter = 256;
    *niter = 100;
  }
}

static void sk_apply_params(cuvsCagraIndexParams_t ip, const sk_cuvs_index* h)
{
  int deg = 0, inter = 0, niter = 0;
  sk_tune_defaults(h->dim, &deg, &inter, &niter);
  ip->graph_degree = sk_conf_int("SEEKDB_CUVS_GRAPH_DEGREE", "graph_degree", deg);
  ip->intermediate_graph_degree = sk_conf_int("SEEKDB_CUVS_INTERMEDIATE_DEGREE", "intermediate_degree", inter);
  ip->nn_descent_niter = sk_conf_int("SEEKDB_CUVS_NITER", "niter", niter);
  /* Build algorithm: default NN_DESCENT (stock behaviour).  Overridable so the
   * IVF_PQ route (usually a much cheaper graph initialisation) can be measured
   * on a real GPU without recompiling.  The numeric value is forwarded as-is;
   * cuVS rejects unknown values at build time. */
  ip->build_algo = (enum cuvsCagraGraphBuildAlgo)sk_conf_int("SEEKDB_CUVS_BUILD_ALGO", "build_algo", (int)NN_DESCENT);
  if (sk_conf_int("SEEKDB_CUVS_TIME", "time", 0) == 1) {
    fprintf(stderr, "[seekdb_cuvs] params build_algo=%d graph_degree=%lu intermediate=%lu niter=%lu\n",
            (int)ip->build_algo, (unsigned long)ip->graph_degree,
            (unsigned long)ip->intermediate_graph_degree,
            (unsigned long)ip->nn_descent_niter);
  }
}

/* One dataset upload + one CAGRA build; leaves h->idx/h->ip NULL on failure. */
static int sk_build_once(sk_cuvs_index* h, DLManagedTensor* dset)
{
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) {
    return 1;
  }
  sk_apply_params(h->ip, h);
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) {
    cuvsCagraIndexParamsDestroy(h->ip);
    h->ip = NULL;
    return 1;
  }
  if (cuvsCagraBuild(h->res, h->ip, dset, h->idx) != CUVS_SUCCESS) {
    cuvsCagraIndexDestroy(h->idx);
    h->idx = NULL;
    cuvsCagraIndexParamsDestroy(h->ip);
    h->ip = NULL;
    return 1;
  }
  return 0;
}

/* Copy the f32 dataset to the device as fp16; returns 0 on success. */
static int sk_prepare_fp16(sk_cuvs_index* h, const float* base, long n, long dim,
                           void** dev_data, size_t* dev_bytes,
                           DLManagedTensor* dset, long* shape)
{
  uint16_t* hbuf = (uint16_t*)malloc((size_t)n * (size_t)dim * sizeof(uint16_t));
  double t0 = sk_now_ms();
  long i;
  if (!hbuf) {
    return 1;
  }
  for (i = 0; i < n * dim; ++i) {
    hbuf[i] = sk_f2h(base[i]);
  }
  *dev_bytes = (size_t)n * (size_t)dim * sizeof(uint16_t);
  if (cuvsRMMAlloc(h->res, dev_data, *dev_bytes) != CUVS_SUCCESS) {
    free(hbuf);
    return 1;
  }
  if (cudaMemcpy(*dev_data, hbuf, *dev_bytes, cudaMemcpyDefault) != cudaSuccess) {
    free(hbuf);
    cuvsRMMFree(h->res, *dev_data, *dev_bytes);
    *dev_data = NULL;
    return 1;
  }
  free(hbuf);
  sk_log("fp16_dataset_upload", t0);
  sk_set_dset(dset, *dev_data, 16, kDLCUDA, shape);
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  double t_all, t0;
  void*  dev_data = NULL;
  size_t dev_bytes = 0;
  int    use_fp16 = sk_conf_int("SEEKDB_CUVS_FP16", "fp16", 0) ? 1 : 0;
  DLManagedTensor dset;
  long ds[2];

  if (!h) {
    return NULL;
  }
  pthread_mutex_init(&h->mu, NULL);
  memset(&dset, 0, sizeof(dset));
  ds[0] = n;
  ds[1] = dim;
  t_all = sk_now_ms();
  t0 = t_all;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    free(h);
    return NULL;
  }
  sk_log("resources_create", t0);

  if (use_fp16 && sk_prepare_fp16(h, base, n, dim, &dev_data, &dev_bytes, &dset, ds) != 0) {
    use_fp16 = 0;
    h->fp16 = 0;
  }
  if (!use_fp16) {
    sk_set_dset(&dset, (void*)base, 32, kDLCPU, ds);
  }

  t0 = sk_now_ms();
  if (sk_build_once(h, &dset) != 0) {
    fprintf(stderr, "[seekdb_cuvs] build failed (fp16=%d): %s\n", use_fp16, cuvsGetLastErrorText());
    if (!use_fp16) {
      if (dev_data) cuvsRMMFree(h->res, dev_data, dev_bytes);
      cuvsResourcesDestroy(h->res);
      free(h);
      return NULL;
    }
    /* cuVS rejected the half dataset: retry once with f32. */
    if (dev_data) {
      cuvsRMMFree(h->res, dev_data, dev_bytes);
      dev_data = NULL;
    }
    use_fp16 = 0;
    sk_set_dset(&dset, (void*)base, 32, kDLCPU, ds);
    if (sk_build_once(h, &dset) != 0) {
      fprintf(stderr, "[seekdb_cuvs] f32 fallback build failed: %s\n", cuvsGetLastErrorText());
      cuvsResourcesDestroy(h->res);
      free(h);
      return NULL;
    }
  }
  sk_log(use_fp16 ? "cagra_build_fp16" : "cagra_build_fp32", t0);
  if (dev_data) {
    cuvsRMMFree(h->res, dev_data, dev_bytes);
    dev_data = NULL;
  }
  h->n = n;
  h->dim = dim;
  h->fp16 = use_fp16;
  sk_log("build_total", t_all);
  return h;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  int rc = 0;
  int use_cache;
  int own_sp = 0;
  void* qd = NULL;
  unsigned* nd = NULL;
  float* dd = NULL;
  cuvsCagraSearchParams_t sp = NULL;
  sk_scratch* s = NULL;
  size_t q_bytes;
  size_t n_bytes;
  size_t d_bytes;
  if (!h) {
    return 1;
  }
  q_bytes = h->fp16 ? sizeof(uint16_t) * (size_t)nq * (size_t)h->dim
                    : sizeof(float) * (size_t)nq * (size_t)h->dim;
  n_bytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  d_bytes = sizeof(float) * (size_t)nq * (size_t)topk;
  use_cache = sk_env_int("SEEKDB_CUVS_SEARCH_CACHE", 1) ? 1 : 0;
  if (use_cache) {
    if (sk_scratch_get(h, nq, topk, &s) != 0) { rc = 10; goto done; }
    qd = s->qd;
    nd = s->nd;
    dd = s->dd;
    sp = sk_search_params(h, topk);
    if (sp == NULL) { rc = 14; goto done; }
  } else {
    if (cuvsRMMAlloc(h->res, &qd, q_bytes) != CUVS_SUCCESS) { rc = 10; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&nd, n_bytes) != CUVS_SUCCESS) { rc = 11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&dd, d_bytes) != CUVS_SUCCESS) { rc = 12; goto done; }
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc = 14; goto done; }
    own_sp = 1;
    sp->search_width = 4;
    if ((size_t)sp->itopk_size < (size_t)(topk * 4 < 512 ? 512 : topk * 4)) {
      sp->itopk_size = (int)(topk * 4 < 512 ? 512 : topk * 4);
    }
  }
  if (!h->fp16) {
    if (cudaMemcpy(qd, query, q_bytes, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  } else {
    uint16_t* qh = (uint16_t*)malloc(q_bytes);
    long i;
    if (!qh) { rc = 13; goto done; }
    for (i = 0; i < nq * h->dim; ++i) {
      qh[i] = sk_f2h(query[i]);
    }
    if (cudaMemcpy(qd, qh, q_bytes, cudaMemcpyDefault) != cudaSuccess) {
      free(qh);
      rc = 13;
      goto done;
    }
    free(qh);
  }
  {
    DLManagedTensor qt;
    DLManagedTensor nt;
    DLManagedTensor dt;
    long qs[2];
    long nsh[2];
    long dsh[2];
    cuvsFilter filter;
    qs[0] = nq; qs[1] = h->dim;
    nsh[0] = nq; nsh[1] = topk;
    dsh[0] = nq; dsh[1] = topk;
    memset(&qt, 0, sizeof(qt));
    sk_set_dset(&qt, qd, h->fp16 ? 16 : 32, kDLCUDA, qs);
    nt = qt;
    nt.dl_tensor.data = nd;
    nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.shape = nsh;
    dt = qt;
    dt.dl_tensor.data = dd;
    dt.dl_tensor.shape = dsh;
    filter.type = NO_FILTER;
    filter.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, nd, n_bytes, cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) {
      cudaMemcpy(out_dist, dd, d_bytes, cudaMemcpyDefault);
    }
  }
done:
  if (own_sp && sp) cuvsCagraSearchParamsDestroy(sp);
  if (!use_cache) {
    if (dd) cuvsRMMFree(h->res, dd, d_bytes);
    if (nd) cuvsRMMFree(h->res, nd, n_bytes);
    if (qd) cuvsRMMFree(h->res, qd, q_bytes);
  }
  if (rc) {
    fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  }
  return rc;
}
void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) {
    return;
  }
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mu);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  double t0;
  if (!index || !filename) {
    return 1;
  }
  t0 = sk_now_ms();
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  sk_log("serialize", t0);
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  sk_cuvs_index* index;
  int64_t actual_n = 0;
  int64_t actual_dim = 0;
  if (!filename || n <= 0 || dim <= 0) {
    return NULL;
  }
  index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) {
    return NULL;
  }
  pthread_mutex_init(&index->mu, NULL);
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) {
    free(index);
    return NULL;
  }
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) {
    cuvsResourcesDestroy(index->res);
    free(index);
    return NULL;
  }
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) {
    cuvsCagraIndexParamsDestroy(index->ip);
    cuvsResourcesDestroy(index->res);
    free(index);
    return NULL;
  }
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
    cuvsCagraIndexDestroy(index->idx);
    cuvsCagraIndexParamsDestroy(index->ip);
    cuvsResourcesDestroy(index->res);
    free(index);
    return NULL;
  }
  if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
      cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
      actual_n != n || actual_dim != dim) {
    fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
            (long)actual_n, (long)actual_dim, n, dim);
    cuvsCagraIndexDestroy(index->idx);
    cuvsCagraIndexParamsDestroy(index->ip);
    cuvsResourcesDestroy(index->res);
    free(index);
    return NULL;
  }
  index->n = n;
  index->dim = dim;
  index->fp16 = 0;
  return index;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  int rc;
  if (!h) {
    return 10;
  }
  rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
