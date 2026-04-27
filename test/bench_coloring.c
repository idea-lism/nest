#include "../src/coloring.h"
#include "../src/graph.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static bool validate_coloring(Graph* g, ColoringResult* cr) {
  if (!cr) {
    return false;
  }
  int32_t* edges = graph_edges(g);
  int32_t n_edges = graph_n_edges(g);

  for (int32_t i = 0; i < n_edges; i++) {
    int32_t u = edges[i * 2];
    int32_t v = edges[i * 2 + 1];
    if (coloring_get_color(cr, u) == coloring_get_color(cr, v)) {
      return false;
    }
  }
  return true;
}

static int32_t get_max_color(ColoringResult* cr) {
  int32_t max_c = -1;
  for (int32_t v = 0; v < coloring_get_n_vertices(cr); v++) {
    int32_t c = coloring_get_color(cr, v);
    if (c > max_c) {
      max_c = c;
    }
  }
  return max_c + 1; // return number of colors used
}

static double run_solve(Graph* g, bool use_product, bool* solved, bool* valid, int32_t* colors_used) {
  int32_t n = graph_n_vertices(g);
  int32_t* edges = graph_edges(g);
  int32_t n_edges = graph_n_edges(g);

  clock_t start = clock();
  ColoringResult* res = coloring_solve(n, edges, n_edges, 10000, 42, use_product, NULL);
  clock_t end = clock();

  if (res) {
    *solved = true;
    *valid = validate_coloring(g, res);
    *colors_used = get_max_color(res);
    coloring_result_del(res);
  } else {
    *solved = false;
    *valid = false;
    *colors_used = 0;
  }

  return (double)(end - start) / CLOCKS_PER_SEC;
}

// Benchmark naive (pairwise) vs product (Heule grid) AMO encoding at a given
// graph density.  For each random Erdős–Rényi graph we binary-search for the
// minimal chromatic number under the SAT conflict limit, then report:
//   Avg Colors  – average k found (lower is better; encodings may differ
//                 because one can prove a smaller k within the conflict budget)
//   Avg Time    – wall-clock seconds per solve
//   Speedup     – naive_time / product_time  (>1 means product is faster)
static void benchmark_density(int32_t n, double p, int32_t iterations) {
  printf("Testing density p=%.3f (n=%d)\n", p, n);
  double t_naive = 0, t_product = 0;
  int32_t total_c_naive = 0, total_c_product = 0;
  int32_t valid_runs = 0;

  for (int i = 0; i < iterations; i++) {
    Graph* g = graph_random_erdos_renyi(n, p);
    bool s_n = false, s_p = false, v_n = false, v_p = false;
    int32_t c_n = 0, c_p = 0;

    double dt_n = run_solve(g, false, &s_n, &v_n, &c_n);
    double dt_p = run_solve(g, true, &s_p, &v_p, &c_p);

    if ((s_n && !v_n) || (s_p && !v_p)) {
      // solved but adjacency constraint violated — SAT bug
      printf("  INVALID coloring at iter %d: Naive(solved=%d,valid=%d,colors=%d)"
             " Product(solved=%d,valid=%d,colors=%d)\n",
             i, s_n, v_n, c_n, s_p, v_p, c_p);
    } else if (s_n && s_p) {
      t_naive += dt_n;
      t_product += dt_p;
      total_c_naive += c_n;
      total_c_product += c_p;
      valid_runs++;
    } else {
      // at least one encoding failed to find any coloring within the conflict limit
      printf("  Unsolved at iter %d: Naive(solved=%d,colors=%d) Product(solved=%d,colors=%d)\n", i, s_n, c_n, s_p, c_p);
    }
    graph_del(g);
  }
  if (valid_runs > 0) {
    printf("  Avg Colors: Naive=%.1f, Product=%.1f\n", (double)total_c_naive / valid_runs,
           (double)total_c_product / valid_runs);
    printf("  Avg Time:   Naive=%.4fs, Product=%.4fs, Speedup=%.2fx\n\n", t_naive / valid_runs, t_product / valid_runs,
           t_naive / t_product);
  } else {
    printf("  No valid runs\n\n");
  }
}

int main() {
  int32_t iterations = 3;

  struct {
    int32_t n;
    double p;
  } cases[] = {
      {256, 0.30}, {256, 0.50}, {128, 0.65}, {128, 0.78}, {64, 0.90},
  };
  int32_t n_cases = (int32_t)(sizeof(cases) / sizeof(cases[0]));

  for (int32_t i = 0; i < n_cases; i++) {
    benchmark_density(cases[i].n, cases[i].p, iterations);
  }

  return 0;
}
