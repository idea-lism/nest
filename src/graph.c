#include "graph.h"
#include <stdlib.h>
#include <string.h>

struct Graph {
  int32_t n_vertices;
  int32_t n_edges;
  int32_t capacity;
  int32_t* edges;
};

Graph* graph_new(int32_t n_vertices) {
  Graph* g = malloc(sizeof(Graph));
  g->n_vertices = n_vertices;
  g->n_edges = 0;
  g->capacity = 16;
  g->edges = malloc(g->capacity * 2 * sizeof(int32_t));
  return g;
}

void graph_add_edge(Graph* g, int32_t u, int32_t v) {
  if (g->n_edges >= g->capacity) {
    g->capacity *= 2;
    g->edges = realloc(g->edges, g->capacity * 2 * sizeof(int32_t));
  }
  g->edges[g->n_edges * 2] = u;
  g->edges[g->n_edges * 2 + 1] = v;
  g->n_edges++;
}

void graph_del(Graph* g) {
  if (!g) return;
  free(g->edges);
  free(g);
}

int32_t graph_n_vertices(Graph* g) {
  return g->n_vertices;
}

int32_t graph_n_edges(Graph* g) {
  return g->n_edges;
}

int32_t* graph_edges(Graph* g) {
  return g->edges;
}

static uint32_t _xorshift32(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

Graph* graph_random_erdos_renyi(uint32_t n, double p) {
  Graph* g = graph_new((int32_t)n);
  uint32_t seed = 12345;
  for (uint32_t u = 0; u < n; u++) {
    for (uint32_t v = u + 1; v < n; v++) {
      double r = (double)_xorshift32(&seed) / (double)0xFFFFFFFF;
      if (r < p) {
        graph_add_edge(g, (int32_t)u, (int32_t)v);
      }
    }
  }
  return g;
}

int32_t* graph_find_max_clique(Graph* g) {
  int32_t n = g->n_vertices;
  if (n == 0) return NULL;
  
  int32_t* adj = calloc(n * n, sizeof(int32_t));
  for (int32_t i = 0; i < g->n_edges; i++) {
    int32_t u = g->edges[i * 2];
    int32_t v = g->edges[i * 2 + 1];
    adj[u * n + v] = 1;
    adj[v * n + u] = 1;
  }
  
  int32_t* clique = malloc(n * sizeof(int32_t));
  int32_t clique_size = 0;
  
  for (int32_t v = 0; v < n; v++) {
    int32_t can_add = 1;
    for (int32_t i = 0; i < clique_size; i++) {
      if (!adj[v * n + clique[i]]) {
        can_add = 0;
        break;
      }
    }
    if (can_add) {
      clique[clique_size++] = v;
    }
  }
  
  free(adj);
  if (clique_size == 0) {
    free(clique);
    return NULL;
  }
  
  int32_t* result = malloc((clique_size + 1) * sizeof(int32_t));
  result[0] = clique_size;
  memcpy(result + 1, clique, clique_size * sizeof(int32_t));
  free(clique);
  return result;
}
