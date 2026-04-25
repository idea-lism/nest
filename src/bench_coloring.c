#include "coloring.h"
#include "graph.h"
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

static double run_solve(Graph* g, int32_t k, bool use_product, bool* solved, bool* valid, int32_t* colors_used) {
  int32_t n = graph_n_vertices(g);
  int32_t* edges = graph_edges(g);
  int32_t n_edges = graph_n_edges(g);

  clock_t start = clock();
  ColoringResult* res = coloring_solve(n, edges, n_edges, k, 10000, 42, use_product);
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

static void benchmark_density(int32_t n, double p, int32_t k, int32_t iterations) {
  printf("Testing density p=%.3f (n=%d, k=%d)\n", p, n, k);
  double t_naive = 0, t_product = 0;

  for (int i = 0; i < iterations; i++) {
    Graph* g = graph_random_erdos_renyi(n, p);
    bool s_n = false, s_p = false, v_n = false, v_p = false;
    int32_t c_n = 0, c_p = 0;

    double dt_n = run_solve(g, k, false, &s_n, &v_n, &c_n);
    double dt_p = run_solve(g, k, true, &s_p, &v_p, &c_p);

    if (s_n != s_p || v_n != v_p || (s_n && c_n != c_p)) {
      printf("  Mismatch at iter %d: Naive(s=%d,v=%d,c=%d) Product(s=%d,v=%d,c=%d)\n", i, s_n, v_n, c_n, s_p, v_p, c_p);
    } else {
      t_naive += dt_n;
      t_product += dt_p;
    }
    graph_del(g);
  }
  printf("  Avg Time: Naive=%.4fs, Product=%.4fs, Speedup=%.2fx\n\n", t_naive / iterations, t_product / iterations,
         t_naive / t_product);
}

int main() {
  int32_t n = 256;
  int32_t k = 32;
  int32_t iterations = 5;

  double densities[] = {0.05, 0.15, 0.30};

  printf("Benchmarking coloring for k=%d\n", k);
  for (int i = 0; i < 3; i++) {
    benchmark_density(n, densities[i], k, iterations);
  }

  return 0;
}
