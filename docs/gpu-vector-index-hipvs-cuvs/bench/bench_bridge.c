/* bench_bridge.c - direct benchmark of the seekdb<->hipVS bridge.
 *
 * Bypasses SQL/observer entirely so that the measured numbers are the ones
 * that the CAGRA build and the nq=1 search path actually cost.
 *
 * usage:
 *   bench_bridge --base F --query F --gt F --n N --dim D --nq NQ
 *                [--topk 10 100] [--reps R] [--skip-build]
 *
 * prints one JSON object on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "seekdb_cuvs_bridge.h"

extern char **environ;

static void dump_env(void) {
  const char *pfx = "SEEKDB_CUVS_";
  for (char **e = environ; e && *e; e++) {
    if (strncmp(*e, pfx, strlen(pfx)) == 0) {
      printf("[bench_bridge] env %s\n", *e);
    }
  }
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static float *read_f32(const char *path, long *count) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "open %s failed\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);
  float *p = (float *)malloc(bytes ? bytes : 1);
  if (fread(p, 1, bytes, f) != (size_t)bytes) { fprintf(stderr, "read %s failed\n", path); exit(2); }
  fclose(f);
  *count = bytes / 4;
  return p;
}

static int *read_i32(const char *path, long *count) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "open %s failed\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);
  int *p = (int *)malloc(bytes ? bytes : 1);
  if (fread(p, 1, bytes, f) != (size_t)bytes) { fprintf(stderr, "read %s failed\n", path); exit(2); }
  fclose(f);
  *count = bytes / 4;
  return p;
}

int main(int argc, char **argv) {
  const char *base_path = 0, *query_path = 0, *gt_path = 0;
  long n = 0, dim = 0, nq = 0, reps = 1, build_reps = 1;
  int topks[4], ntopk = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--base") && i + 1 < argc) base_path = argv[++i];
    else if (!strcmp(argv[i], "--query") && i + 1 < argc) query_path = argv[++i];
    else if (!strcmp(argv[i], "--gt") && i + 1 < argc) gt_path = argv[++i];
    else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atol(argv[++i]);
    else if (!strcmp(argv[i], "--dim") && i + 1 < argc) dim = atol(argv[++i]);
    else if (!strcmp(argv[i], "--nq") && i + 1 < argc) nq = atol(argv[++i]);
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atol(argv[++i]);
    else if (!strcmp(argv[i], "--build-reps") && i + 1 < argc) build_reps = atol(argv[++i]);
    else if (!strcmp(argv[i], "--topk") && i + 1 < argc) {
      /* stop at the next option token: "--build-reps" must not be eaten as a topk */
      while (i + 1 < argc && ntopk < 4 && argv[i + 1][0] != '-') topks[ntopk++] = atoi(argv[++i]);
    }
  }
  if (!base_path || !query_path || !n || !dim) {
    fprintf(stderr, "missing args\n");
    return 2;
  }
  if (!ntopk) { topks[0] = 10; topks[1] = 100; ntopk = 2; }

  long bn = 0, qn = 0;
  float *base = read_f32(base_path, &bn);
  float *query = read_f32(query_path, &qn);
  if (qn / dim < nq) nq = qn / dim;
  printf("[bench_bridge] base=%ld q=%ld nq=%ld dim=%ld\n", bn, qn, nq, dim);
  printf("[bench_bridge] bridge_tag=%s\n", seekdb_cuvs_tag());
  dump_env();
  fflush(stdout);

  void *h = 0;
  double build_ms = 0, build_ms_first = 0;
  for (int b = 0; b < build_reps; b++) {
    void *prev = h;
    double tb0 = now_ms();
    h = seekdb_cuvs_build(base, n, dim);
    build_ms = now_ms() - tb0;
    if (!h) { fprintf(stderr, "build failed\n"); return 3; }
    if (b == 0) build_ms_first = build_ms;
    printf("[bench_bridge] build #%d ms=%.1f\n", b, build_ms);
    fflush(stdout);
    if (prev) seekdb_cuvs_free(prev);
  }
  printf("[bench_bridge] build_ms=%.1f (cold=%.1f)\n", build_ms, build_ms_first);

  int *gt = 0;
  long gtn = 0;
  if (gt_path) gt = read_i32(gt_path, &gtn);

  unsigned *ids = (unsigned *)malloc(sizeof(unsigned) * (size_t)nq * 100);
  float *dist = (float *)malloc(sizeof(float) * (size_t)nq * 100);

  printf("{\n");
  printf("  \"build_ms\": %.1f,\n", build_ms);

  for (int t = 0; t < ntopk; t++) {
    int k = topks[t];
    unsigned *best = (unsigned *)malloc(sizeof(unsigned) * (size_t)nq * (size_t)k);
    /* warm up once (not timed) */
    seekdb_cuvs_search(h, query, 1, k, best, dist);

    long hit = 0, tot = 0;
    double q0 = now_ms();
    for (int r = 0; r < reps; r++) {
      for (long i = 0; i < nq; i++) {
        seekdb_cuvs_search(h, query + i * dim, 1, k, best + i * k, dist);
      }
    }
    double dt = now_ms() - q0;
    double per_q = dt / (double)(nq * reps);

    if (gt) {
      long gk = gtn / nq;
      if (gk > 100) gk = 100;
      if (gk < k) gk = k;
      for (long i = 0; i < nq; i++) {
        const int *g = gt + i * gk;
        for (int j = 0; j < k; j++) {
          unsigned id = best[i * k + j];
          int found = 0;
          for (long z = 0; z < gk; z++) if ((unsigned)g[z] == id) { found = 1; break; }
          hit += found;
          tot++;
        }
      }
    }
    printf("  \"k%d\": {\"qps\": %.2f, \"latency_ms\": %.3f, \"recall\": %.5f},\n",
           k, 1000.0 / per_q, per_q, tot ? (double)hit / (double)tot : 0.0);
    fflush(stdout);
    free(best);
  }
  printf("  \"ok\": true\n}\n");

  seekdb_cuvs_free(h);
  free(base);
  free(query);
  free(gt);
  free(ids);
  free(dist);
  return 0;
}
