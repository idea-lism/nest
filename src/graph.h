#pragma once

#include <stdint.h>

typedef struct Graph Graph;

Graph* graph_new(int32_t n_vertices);
void graph_add_edge(Graph* g, int32_t u, int32_t v);
void graph_del(Graph* g);
int32_t graph_n_vertices(Graph* g);
int32_t graph_n_edges(Graph* g);
int32_t* graph_edges(Graph* g);
Graph* graph_random_erdos_renyi(uint32_t n, double p);
