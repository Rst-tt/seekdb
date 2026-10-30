/* Standalone CAGRA build/search benchmark for the seekdb <-> hipVS bridge.
 *
 * Links against libseekdb_cuvs_bridge.so so it exercises exactly the code path
 * seekdb uses. Build parameters come from the same env vars the bridge reads.
 *
 * Usage:
 *   ./bench_build --n 250000 --dim 128 --nq 100 --topk 100
 *   ./bench_build --base /data/sift/base.fvecs --query /data/sift/query.fvecs ...
 * Options:
 *   --n, --dim, --nq, --topk, --repeat, --seed
 *   --base  random|<path.fvecs>
 *   --query random|<path.fvecs>     (random queries = base points + noise)
 *   --tag   <string>                echoed in the JSON line
 *   --no-exact                      skip the CPU brute-force recall pass
 *   --debug                         dump the first query's ids
 * Prints one JSON object per repeat, then an AVERAGE line.
 */
#include "seekdb_cuvs_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void* xmalloc(size_t n)
{
  void* p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "oom\n"); exit(2); }
  return p;
}

/* .fvecs: [int32 dim][float x dim] repeated */
static float* load_fvecs(const char* path, long* n_out, long* dim_out)
{
  FILE* f = fopen(path, "rb");
  float* buf = NULL;
  long cap = 0, n = 0, dim = 0;
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  for (;;) {
    int32_t d = 0;
    if (fread(&d, sizeof(d), 1, f) != 1) break;
    if (d <= 0 || d > 8192) { fprintf(stderr, "bad dim %d at row %ld\n", d, n); exit(2); }
    if (dim == 0) dim = d;
    if (d != dim) { fprintf(stderr, "ragged fvecs\n"); exit(2); }
    if ((n + 1) * dim > cap) {
      cap = (cap ? cap * 2 : (long)dim * 1024);
      while ((n + 1) * dim > cap) cap *= 2;
      buf = (float*)realloc(buf, (size_t)cap * sizeof(float));
      if (!buf) { fprintf(stderr, "oom\n"); exit(2); }
    }
    if (fread(buf + (size_t)n * dim, sizeof(float), (size_t)dim, f) != (size_t)dim) {
      fprintf(stderr, "truncated fvecs\n"); exit(2);
    }
    n++;
  }
  fclose(f);
  *n_out = n; *dim_out = dim;
  return buf;
}

static float frand(unsigned* s)
{
  *s = (*s) * 1103515245u + 12345u;
  return (float)((*s >> 8) & 0xffffff) / (float)0xffffff * 2.0f - 1.0f;
}

static void heap_sift_down(float* hd, unsigned* hi, long cnt, long c)
{
  for (;;) {
    long l = c * 2 + 1, r = l + 1, m = c;
    if (l < cnt && hd[l] > hd[m]) m = l;
    if (r < cnt && hd[r] > hd[m]) m = r;
    if (m == c) break;
    { float td = hd[m]; hd[m] = hd[c]; hd[c] = td; }
    { unsigned ti = hi[m]; hi[m] = hi[c]; hi[c] = ti; }
    c = m;
  }
}

static void heap_push(float* hd, unsigned* hi, long k, long* cnt, float d, unsigned id)
{
  long c, p;
  if (*cnt < k) {
    c = (*cnt)++;
    hd[c] = d; hi[c] = id;
    while (c > 0) {
      p = (c - 1) / 2;
      if (hd[p] >= hd[c]) break;
      { float td = hd[p]; hd[p] = hd[c]; hd[c] = td; }
      { unsigned ti = hi[p]; hi[p] = hi[c]; hi[c] = ti; }
      c = p;
    }
  } else if (d < hd[0]) {
    hd[0] = d; hi[0] = id;
    heap_sift_down(hd, hi, *cnt, 0);
  }
}

static void heap_pop(float* hd, unsigned* hi, long* cnt)
{
  long n = --(*cnt), c = 0;
  hd[0] = hd[n]; hi[0] = hi[n];
  for (;;) {
    long l = c * 2 + 1, r = l + 1, m = c;
    if (l < n && hd[l] > hd[m]) m = l;
    if (r < n && hd[r] > hd[m]) m = r;
    if (m == c) break;
    { float td = hd[m]; hd[m] = hd[c]; hd[c] = td; }
    { unsigned ti = hi[m]; hi[m] = hi[c]; hi[c] = ti; }
    c = m;
  }
}

/* brute-force L2 top-k, ascending by distance */
static void exact_topk(const float* base, long n, long dim, const float* q, long nq,
                       long k, unsigned* out)
{
  long qi;
  float* hd = (float*)xmalloc(sizeof(float) * (size_t)k);
  unsigned* hi = (unsigned*)xmalloc(sizeof(unsigned) * (size_t)k);
  for (qi = 0; qi < nq; ++qi) {
    const float* qv = q + (size_t)qi * dim;
    long i, j, cnt = 0;
    for (i = 0; i < n; ++i) {
      const float* bv = base + (size_t)i * dim;
      float s = 0.0f;
      for (j = 0; j < dim; ++j) {
        float d = qv[j] - bv[j];
        s += d * d;
      }
      heap_push(hd, hi, k, &cnt, s, (unsigned)i);
    }
    for (i = cnt - 1; i >= 0; --i) {
      out[(size_t)qi * k + i] = hi[0];
      heap_pop(hd, hi, &cnt);
    }
  }
  free(hd); free(hi);
}

int main(int argc, char** argv)
{
  long n = 250000, dim = 128, nq = 100, topk = 100;
  int repeat = 1, seed = 42, want_exact = 1, debug = 0;
  const char* base_path = "random";
  const char* query_path = "random";
  const char* tag = "";
  float* base = NULL;
  float* query = NULL;
  double best_build = 1e18, sum_build = 0;
  double best_search = 1e18, sum_search = 0;
  double recall_at_10 = -1, recall_at_100 = -1;
  unsigned* approx = NULL;
  int r, i;

  for (i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* v = (i + 1 < argc) ? argv[i + 1] : "";
    if (!strcmp(a, "--n")) { n = atol(v); i++; }
    else if (!strcmp(a, "--dim")) { dim = atol(v); i++; }
    else if (!strcmp(a, "--nq")) { nq = atol(v); i++; }
    else if (!strcmp(a, "--topk")) { topk = atol(v); i++; }
    else if (!strcmp(a, "--repeat")) { repeat = atoi(v); i++; }
    else if (!strcmp(a, "--seed")) { seed = atoi(v); i++; }
    else if (!strcmp(a, "--base")) { base_path = v; i++; }
    else if (!strcmp(a, "--query")) { query_path = v; i++; }
    else if (!strcmp(a, "--tag")) { tag = v; i++; }
    else if (!strcmp(a, "--no-exact")) { want_exact = 0; i++; }
    else if (!strcmp(a, "--debug")) { debug = 1; i++; }
    else { fprintf(stderr, "unknown arg %s\n", a); return 2; }
  }
  if (getenv("SEEKDB_BENCH_DEBUG")) {
    debug = 1;
  }

  if (!strcmp(base_path, "random")) {
    unsigned s = (unsigned)seed;
    long j;
    base = (float*)xmalloc(sizeof(float) * (size_t)n * (size_t)dim);
    for (j = 0; j < n * dim; ++j) base[j] = frand(&s);
  } else {
    base = load_fvecs(base_path, &n, &dim);
  }

  if (!strcmp(query_path, "random")) {
    unsigned s = (unsigned)seed * 7919u + 17u;
    long qi, j;
    query = (float*)xmalloc(sizeof(float) * (size_t)nq * (size_t)dim);
    for (qi = 0; qi < nq; ++qi) {
      long pick = (long)(s % (unsigned)n);
      for (j = 0; j < dim; ++j) {
        query[(size_t)qi * dim + j] = base[pick * dim + j] + 0.02f * frand(&s);
      }
    }
  } else {
    long qn = 0, qd = 0;
    query = load_fvecs(query_path, &qn, &qd);
    if (qd != dim) { fprintf(stderr, "query dim %ld != base dim %ld\n", qd, dim); return 2; }
    if (qn < nq) nq = qn;
  }

  approx = (unsigned*)xmalloc(sizeof(unsigned) * (size_t)nq * (size_t)topk);

  for (r = 0; r < repeat; ++r) {
    double t0 = now_ms();
    void* h = seekdb_cuvs_build(base, n, dim);
    double build_ms = now_ms() - t0;
    double search_ms = 0.0;
    if (!h) { fprintf(stderr, "build failed\n"); return 3; }
    t0 = now_ms();
    if (seekdb_cuvs_search(h, query, nq, topk, approx, NULL) != 0) {
      fprintf(stderr, "search failed\n"); return 4;
    }
    search_ms = now_ms() - t0;
    seekdb_cuvs_free(h);

    sum_build += build_ms; sum_search += search_ms;
    if (build_ms < best_build) best_build = build_ms;
    if (search_ms < best_search) best_search = search_ms;

    if (want_exact && r == 0) {
      long k100 = 100, qi, a, b, hit10 = 0, hit100 = 0;
      unsigned* exact;
      if (k100 > n) k100 = n;
      if (k100 > topk) k100 = topk;
      exact = (unsigned*)xmalloc(sizeof(unsigned) * (size_t)nq * (size_t)k100);
      exact_topk(base, n, dim, query, nq, k100, exact);
      if (debug) {
        printf("DBG approx[0..9]:");
        for (a = 0; a < 10 && a < topk; ++a) printf(" %u", approx[a]);
        printf("\nDBG exact [0..9]:");
        for (a = 0; a < 10 && a < k100; ++a) printf(" %u", exact[a]);
        printf("\n");
      }
      for (qi = 0; qi < nq; ++qi) {
        long n10 = (10 < topk ? 10 : topk);
        long n100 = (100 < topk ? 100 : topk);
        if (n10 > k100) n10 = k100;
        if (n100 > k100) n100 = k100;
        for (a = 0; a < n10; ++a) {
          for (b = 0; b < n10; ++b) {
            if (approx[(size_t)qi * topk + a] == exact[(size_t)qi * k100 + b]) { hit10++; break; }
          }
        }
        for (a = 0; a < n100; ++a) {
          for (b = 0; b < n100; ++b) {
            if (approx[(size_t)qi * topk + a] == exact[(size_t)qi * k100 + b]) { hit100++; break; }
          }
        }
        recall_at_10 = (double)hit10 / (double)(nq * n10);
        recall_at_100 = (double)hit100 / (double)(nq * n100);
      }
      if (debug) {
        printf("DBG hit10=%ld hit100=%ld nq=%ld k100=%ld recall@10=%.4f recall@100=%.4f\n",
               hit10, hit100, nq, k100, recall_at_10, recall_at_100);
      }
      free(exact);
    }
    printf("{\"tag\":\"%s\",\"rep\":%d,\"n\":%ld,\"dim\":%ld,\"nq\":%ld,\"topk\":%ld,"
           "\"build_ms\":%.1f,\"search_ms\":%.1f,\"recall@10\":%.4f,\"recall@100\":%.4f}\n",
           tag, r, n, dim, nq, topk, build_ms, search_ms, recall_at_10, recall_at_100);
    fflush(stdout);
  }
  printf("{\"tag\":\"%s\",\"AVERAGE\":true,\"rep\":%d,\"build_ms\":%.1f,\"best_build_ms\":%.1f,"
         "\"search_ms\":%.1f,\"best_search_ms\":%.1f,\"recall@10\":%.4f,\"recall@100\":%.4f}\n",
         tag, repeat, sum_build / repeat, best_build, sum_search / repeat, best_search,
         recall_at_10, recall_at_100);
  free(base); free(query); free(approx);
  return 0;
}
