#include "../src/coloring.h"
#include "../src/graph.h"
#include "../src/xmalloc.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool _verify_coloring(Graph* g, ColoringResult* cr) {
  int32_t n = graph_n_vertices(g);
  int32_t* edges = graph_edges(g);
  int32_t n_edges = graph_n_edges(g);

  int32_t* colors = malloc(n * sizeof(int32_t));
  for (int32_t v = 0; v < n; v++) {
    int32_t sg_id;
    int64_t seg_mask;
    coloring_get_segment_info(cr, v, &sg_id, &seg_mask);
    colors[v] = sg_id;
  }

  for (int32_t i = 0; i < n_edges; i++) {
    int32_t u = edges[i * 2];
    int32_t v = edges[i * 2 + 1];
    if (colors[u] == colors[v]) {
      free(colors);
      return false;
    }
  }

  free(colors);
  return true;
}

static void _test_max_clique(void) {
  Graph* g = graph_new(5);
  graph_add_edge(g, 0, 1);
  graph_add_edge(g, 0, 2);
  graph_add_edge(g, 1, 2);
  graph_add_edge(g, 2, 3);

  int32_t* clique = graph_find_max_clique(g);
  assert(clique != NULL);
  assert(clique[0] >= 3);
  XFREE(clique);
  graph_del(g);

  g = graph_new(4);
  graph_add_edge(g, 0, 1);
  graph_add_edge(g, 0, 2);
  graph_add_edge(g, 0, 3);
  graph_add_edge(g, 1, 2);
  graph_add_edge(g, 1, 3);
  graph_add_edge(g, 2, 3);

  clique = graph_find_max_clique(g);
  assert(clique != NULL);
  assert(clique[0] == 4);
  XFREE(clique);
  graph_del(g);
}

static void _test_segmenting(void) {
  Graph* g = graph_new(100);
  for (int32_t i = 0; i < 50; i++) {
    for (int32_t j = 50; j < 100; j++) {
      graph_add_edge(g, i, j);
    }
  }

  ColoringResult* cr = coloring_solve(100, graph_edges(g), graph_n_edges(g), 2, 10000, 42);
  assert(cr != NULL);

  int32_t sg_size = coloring_get_sg_size(cr);
  assert(sg_size >= 2);

  int32_t sg0, sg49;
  int64_t mask0, mask49;
  coloring_get_segment_info(cr, 0, &sg0, &mask0);
  coloring_get_segment_info(cr, 49, &sg49, &mask49);

  assert(sg0 != sg49 || mask0 != mask49);

  coloring_result_del(cr);
  graph_del(g);
}

int main(void) {
  _test_max_clique();
  _test_segmenting();

  int32_t edges[] = {0, 1, 1, 2, 2, 0};
  ColoringResult* cr = coloring_solve(3, edges, 3, 3, 1000, 42);
  assert(cr != NULL);
  assert(coloring_get_sg_size(cr) >= 3);
  coloring_result_del(cr);

  Graph* g = graph_random_erdos_renyi(10, 0.3);
  cr = coloring_solve(graph_n_vertices(g), graph_edges(g), graph_n_edges(g), 4, 10000, 42);
  assert(cr != NULL);
  assert(_verify_coloring(g, cr));
  coloring_result_del(cr);
  graph_del(g);

  g = graph_random_erdos_renyi(50, 0.2);
  cr = coloring_solve(graph_n_vertices(g), graph_edges(g), graph_n_edges(g), 10, 10000, 42);
  assert(cr != NULL);
  assert(_verify_coloring(g, cr));
  coloring_result_del(cr);
  graph_del(g);

  printf("test_coloring: OK\n");
  return 0;
}
