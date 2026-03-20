#include "../src/ustr.h"
#include "../src/ustr_intern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void fill_ascii(char* buf, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    buf[i] = 'A' + (i % 26);
  }
}

static void fill_mixed(char* buf, size_t sz) {
  const char unit[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  size_t ulen = sizeof(unit) - 1;
  for (size_t i = 0; i < sz;) {
    size_t n = ulen;
    if (i + n > sz) {
      n = sz - i;
    }
    memcpy(buf + i, unit, n);
    i += n;
  }
}

static void fill_cjk(char* buf, size_t sz) {
  for (size_t i = 0; i + 3 <= sz; i += 3) {
    buf[i + 0] = '\xE4';
    buf[i + 1] = '\xB8';
    buf[i + 2] = '\x80';
  }
}

typedef int (*validate_fn)(const uint8_t*, size_t, uint8_t*);

static double bench_validate(validate_fn fn, const char* data, size_t sz, int iters) {
  size_t mark_sz = (sz + 7) / 8;
  uint8_t* marks = (uint8_t*)calloc(1, mark_sz);

  double start = now_sec();
  for (int i = 0; i < iters; i++) {
    memset(marks, 0, mark_sz);
    fn((const uint8_t*)data, sz, marks);
  }
  double elapsed = now_sec() - start;
  free(marks);
  return elapsed;
}

static void run_validate_bench(const char* label, const char* data, size_t sz) {
  int iters = 1000;
  bench_validate(ustr_validate_scalar, data, sz, 10);
  bench_validate(ustr_validate, data, sz, 10);

  double t_scalar = bench_validate(ustr_validate_scalar, data, sz, iters);
  double t_neon = bench_validate(ustr_validate, data, sz, iters);

  double scalar_ns = (t_scalar / iters) / sz * 1e9;
  double neon_ns = (t_neon / iters) / sz * 1e9;
  double speedup = t_scalar / t_neon;

  printf("%-20s  %8zu bytes  scalar: %6.2f ns/byte  neon: %6.2f ns/byte  speedup: %.2fx\n", label, sz, scalar_ns,
         neon_ns, speedup);
}

// --- cplen benchmark ---

static void bench_cplen(const char* label, size_t sz, char* (*make)(size_t)) {
  char* s = make(sz);
  if (!s) {
    printf("%-20s  SKIP (ustr_new failed)\n", label);
    return;
  }

  int iters = 10000;
  volatile int32_t sink;

  for (int i = 0; i < 100; i++) {
    sink = ustr_size(s);
  }
  for (int i = 0; i < 100; i++) {
    sink = ustr_size_naive(s);
  }

  double t0 = now_sec();
  for (int i = 0; i < iters; i++) {
    sink = ustr_size(s);
  }
  double t_popcnt = now_sec() - t0;

  t0 = now_sec();
  for (int i = 0; i < iters; i++) {
    sink = ustr_size_naive(s);
  }
  double t_naive = now_sec() - t0;

  (void)sink;
  double speedup = t_naive / t_popcnt;
  printf("%-20s  %8zu bytes  naive: %8.1f ns  popcnt: %8.1f ns  speedup: %.2fx\n", label, sz, t_naive / iters * 1e9,
         t_popcnt / iters * 1e9, speedup);

  ustr_del(s);
}

// --- slice benchmark ---

static void bench_slice(const char* label, size_t sz, char* (*make)(size_t)) {
  char* s = make(sz);
  if (!s) {
    printf("%-20s  SKIP\n", label);
    return;
  }

  int32_t cplen = ustr_size(s);
  int32_t mid = cplen / 2;
  int32_t quarter = cplen / 4;

  int iters = 5000;

  for (int i = 0; i < 50; i++) {
    char* t = ustr_slice(s, quarter, mid);
    ustr_del(t);
  }
  for (int i = 0; i < 50; i++) {
    char* t = ustr_slice_naive(s, quarter, mid);
    ustr_del(t);
  }

  double t0 = now_sec();
  for (int i = 0; i < iters; i++) {
    char* t = ustr_slice(s, quarter, mid);
    ustr_del(t);
  }
  double t_popcnt = now_sec() - t0;

  t0 = now_sec();
  for (int i = 0; i < iters; i++) {
    char* t = ustr_slice_naive(s, quarter, mid);
    ustr_del(t);
  }
  double t_naive = now_sec() - t0;

  double speedup = t_naive / t_popcnt;
  printf("%-20s  %8zu bytes  naive: %8.1f ns  popcnt: %8.1f ns  speedup: %.2fx\n", label, sz, t_naive / iters * 1e9,
         t_popcnt / iters * 1e9, speedup);

  ustr_del(s);
}

// --- Factories ---

static char* make_ascii(size_t sz) {
  char* buf = (char*)malloc(sz);
  fill_ascii(buf, sz);
  char* s = ustr_new(sz, buf);
  free(buf);
  return s;
}

static char* make_mixed(size_t sz) {
  char* buf = (char*)malloc(sz);
  fill_mixed(buf, sz);
  size_t msz = (sz / 10) * 10;
  char* s = ustr_new(msz, buf);
  free(buf);
  return s;
}

static char* make_cjk(size_t sz) {
  char* buf = (char*)calloc(1, sz);
  fill_cjk(buf, sz);
  size_t csz = (sz / 3) * 3;
  char* s = ustr_new(csz, buf);
  free(buf);
  return s;
}

int main(void) {
  size_t sizes[] = {64, 256, 1024, 4096, 65536, 1048576};
  int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

  printf("=== Validation benchmark (scalar vs NEON) ===\n\n");

  for (int si = 0; si < nsizes; si++) {
    size_t sz = sizes[si];
    char* buf = (char*)malloc(sz);

    fill_ascii(buf, sz);
    run_validate_bench("ascii", buf, sz);

    fill_mixed(buf, sz);
    size_t msz = (sz / 10) * 10;
    if (msz > 0) {
      run_validate_bench("mixed", buf, msz);
    }

    fill_cjk(buf, sz);
    size_t csz = (sz / 3) * 3;
    if (csz > 0) {
      run_validate_bench("cjk", buf, csz);
    }

    printf("\n");
    free(buf);
  }

  printf("=== ustr_size benchmark (naive vs popcnt) ===\n\n");

  for (int si = 0; si < nsizes; si++) {
    size_t sz = sizes[si];
    bench_cplen("ascii", sz, make_ascii);
    bench_cplen("mixed", sz, make_mixed);
    bench_cplen("cjk", sz, make_cjk);
    printf("\n");
  }

  printf("=== ustr_slice benchmark (naive vs popcnt) ===\n\n");

  for (int si = 0; si < nsizes; si++) {
    size_t sz = sizes[si];
    bench_slice("ascii", sz, make_ascii);
    bench_slice("mixed", sz, make_mixed);
    bench_slice("cjk", sz, make_cjk);
    printf("\n");
  }

  return 0;
}
