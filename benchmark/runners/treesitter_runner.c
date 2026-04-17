// treesitter_runner.c — timing harness for tree-sitter parsers.
// Build: clang -O2 treesitter_runner.c src/parser.c [src/scanner.c] \
//        -I<ts_prefix>/include -L<ts_prefix>/lib -ltree-sitter \
//        -DLANG_FN=tree_sitter_json -o runner
//
// Usage: runner <input_file> <iterations>
// Output: CSV line to stdout: parse_us,size_bytes

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tree_sitter/api.h>

#ifndef LANG_FN
#error "Define LANG_FN (e.g. -DLANG_FN=tree_sitter_json)"
#endif

extern const TSLanguage* LANG_FN(void);

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

    // Read input file
    FILE* f = fopen(input_path, "rb");
    if (!f) { perror(input_path); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = malloc((size_t)file_size + 1);
    fread(src, 1, (size_t)file_size, f);
    src[file_size] = '\0';
    fclose(f);

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, LANG_FN());

    // Warm up: 3 iterations
    for (int i = 0; i < 3; i++) {
        TSTree* tree = ts_parser_parse_string(parser, NULL, src, (uint32_t)file_size);
        ts_tree_delete(tree);
    }

    // Timed iterations
    double* times = malloc(sizeof(double) * (size_t)iterations);
    for (int i = 0; i < iterations; i++) {
        double t0 = now_sec();
        TSTree* tree = ts_parser_parse_string(parser, NULL, src, (uint32_t)file_size);
        double t1 = now_sec();
        times[i] = (t1 - t0) * 1e6;
        ts_tree_delete(tree);
    }

    // Compute median
    qsort(times, (size_t)iterations, sizeof(double), cmp_double);
    double median_us = times[iterations / 2];

    // CSV: parse_us,size_bytes
    printf("%.1f,%ld\n", median_us, file_size);

    free(times);
    ts_parser_delete(parser);
    free(src);
    return 0;
}
