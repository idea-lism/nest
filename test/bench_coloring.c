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

static double run_solve(Graph* g, bool* solved, bool* valid, int32_t* colors_used) {
  int32_t n = graph_n_vertices(g);
  int32_t* edges = graph_edges(g);
  int32_t n_edges = graph_n_edges(g);

  clock_t start = clock();
  ColoringResult* res = coloring_solve(n, edges, n_edges, 10000, 42, false, NULL);
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

static void benchmark_density(int32_t n, double p, int32_t iterations) {
  printf("Testing density p=%.3f (n=%d)\n", p, n);
  double total_time = 0;
  int32_t total_colors = 0;
  int32_t valid_runs = 0;

  for (int i = 0; i < iterations; i++) {
    Graph* g = graph_random_erdos_renyi(n, p);
    bool solved = false, valid = false;
    int32_t colors = 0;

    double dt = run_solve(g, &solved, &valid, &colors);

    if (solved && valid) {
      total_time += dt;
      total_colors += colors;
      valid_runs++;
    } else {
      printf("  Invalid coloring at iter %d: solved=%d valid=%d colors=%d\n", i, solved, valid, colors);
    }
    graph_del(g);
  }
  if (valid_runs > 0) {
    printf("  Avg Colors: %.1f\n", (double)total_colors / valid_runs);
    printf("  Avg Time:   %.4fs\n\n", total_time / valid_runs);
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
