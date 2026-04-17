// packcc_runner.c — timing harness for PackCC-generated parsers.
// Build: packcc -o parser grammar.peg
//        clang -O2 -DPACKCC_RUNNER parser.c packcc_runner.c -o runner
// The generated parser.c has its own main() in the %% section.
// We override it by defining PACKCC_RUNNER and providing our own main.
// However, PackCC embeds main() in the generated .c file itself.
// So instead, this file IS the runner: we compile it with the generated
// parser header included, and call the {prefix}_create / _parse / _destroy API.
//
// Usage: runner <input_file> <iterations>
// Output: CSV line to stdout: parse_us,size_bytes

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Forward declarations — filled in by linker from generated parser.c
// The actual prefix is grammar-specific; we use macros set at compile time.
// -DPACKCC_PREFIX=calc  (or json, kotlin)
#ifndef PACKCC_PREFIX
#error "Define PACKCC_PREFIX (e.g. -DPACKCC_PREFIX=calc)"
#endif

#define _CONCAT(a, b) a##b
#define CONCAT(a, b) _CONCAT(a, b)
#define PCC_CREATE  CONCAT(PACKCC_PREFIX, _create)
#define PCC_PARSE   CONCAT(PACKCC_PREFIX, _parse)
#define PCC_DESTROY CONCAT(PACKCC_PREFIX, _destroy)

typedef struct CONCAT(PACKCC_PREFIX, _context_tag) pcc_ctx_t;
extern pcc_ctx_t* PCC_CREATE(void* auxil);
extern int PCC_PARSE(pcc_ctx_t* ctx, int* ret);
extern void PCC_DESTROY(pcc_ctx_t* ctx);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp_double(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input_file> <iterations>\n", argv[0]);
        return 1;
    }

    const char* input_path = argv[1];
    int iterations = atoi(argv[2]);
    if (iterations < 1) iterations = 1;

    // PackCC reads from stdin, so we redirect
    if (freopen(input_path, "r", stdin) == NULL) {
        perror(input_path);
        return 1;
    }

    // Get file size
    FILE* f = fopen(input_path, "rb");
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);

    // Warm up: 3 iterations
    for (int i = 0; i < 3; i++) {
        freopen(input_path, "r", stdin);
        pcc_ctx_t* ctx = PCC_CREATE(NULL);
        while (PCC_PARSE(ctx, NULL));
        PCC_DESTROY(ctx);
    }

    // Timed iterations
    double* times = malloc(sizeof(double) * (size_t)iterations);
    for (int i = 0; i < iterations; i++) {
        freopen(input_path, "r", stdin);
        double t0 = now_sec();
        pcc_ctx_t* ctx = PCC_CREATE(NULL);
        while (PCC_PARSE(ctx, NULL));
        PCC_DESTROY(ctx);
        double t1 = now_sec();
        times[i] = (t1 - t0) * 1e6;
    }

    // Compute median
    qsort(times, (size_t)iterations, sizeof(double), cmp_double);
    double median_us = times[iterations / 2];

    // CSV: parse_us,size_bytes
    printf("%.1f,%ld\n", median_us, file_size);

    free(times);
    return 0;
}
