#include "coloring.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  int32_t sg_id;
  int32_t seg_mask;
} VertexInfo;

struct ColoringResult {
  int32_t n_vertices;
  int32_t sg_size;
  VertexInfo* vertex_info;
};

typedef struct kissat kissat;
extern kissat* kissat_init(void);
extern void kissat_add(kissat* solver, int lit);
extern int kissat_solve(kissat* solver);
extern int kissat_value(kissat* solver, int lit);
extern void kissat_release(kissat* solver);
extern void kissat_set_conflict_limit(kissat* solver, unsigned limit);
extern void kissat_reserve(kissat* solver, int max_var);

static int32_t _var(int32_t v, int32_t c, int32_t k) {
  return v * k + c + 1;
}

static int32_t* _solve_sat(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k, int32_t max_steps, int32_t seed) {
  (void)seed;
  kissat* solver = kissat_init();
  
  int32_t max_var = n_vertices * k;
  kissat_reserve(solver, max_var);
  
  if (max_steps > 0) {
    kissat_set_conflict_limit(solver, (unsigned)max_steps);
  }

  for (int32_t v = 0; v < n_vertices; v++) {
    for (int32_t c = 0; c < k; c++) {
      kissat_add(solver, _var(v, c, k));
    }
    kissat_add(solver, 0);
  }

  for (int32_t v = 0; v < n_vertices; v++) {
    for (int32_t c1 = 0; c1 < k; c1++) {
      for (int32_t c2 = c1 + 1; c2 < k; c2++) {
        kissat_add(solver, -_var(v, c1, k));
        kissat_add(solver, -_var(v, c2, k));
        kissat_add(solver, 0);
      }
    }
  }

  for (int32_t i = 0; i < n_edges; i++) {
    int32_t u = edges[i * 2];
    int32_t v = edges[i * 2 + 1];
    for (int32_t c = 0; c < k; c++) {
      kissat_add(solver, -_var(u, c, k));
      kissat_add(solver, -_var(v, c, k));
      kissat_add(solver, 0);
    }
  }

  if (n_vertices > 0) {
    kissat_add(solver, _var(0, 0, k));
    kissat_add(solver, 0);
  }

  int result = kissat_solve(solver);
  int32_t* colors = NULL;
  if (result == 10) {
    colors = malloc(n_vertices * sizeof(int32_t));
    for (int32_t v = 0; v < n_vertices; v++) {
      for (int32_t c = 0; c < k; c++) {
        if (kissat_value(solver, _var(v, c, k)) > 0) {
          colors[v] = c;
          break;
        }
      }
    }
  }

  kissat_release(solver);
  return colors;
}

static void _build_segments(ColoringResult* cr, int32_t* colors, int32_t k) {
  int32_t* color_counts = calloc(k, sizeof(int32_t));
  for (int32_t i = 0; i < cr->n_vertices; i++) {
    color_counts[colors[i]]++;
  }

  int32_t sg_id = 0;
  int32_t* color_sg_base = malloc(k * sizeof(int32_t));
  
  for (int32_t c = 0; c < k; c++) {
    color_sg_base[c] = sg_id;
    int32_t count = color_counts[c];
    sg_id += (count + 31) / 32;
  }
  cr->sg_size = sg_id;

  int32_t* color_pos = calloc(k, sizeof(int32_t));
  for (int32_t v = 0; v < cr->n_vertices; v++) {
    int32_t c = colors[v];
    int32_t pos = color_pos[c]++;
    int32_t seg_idx = pos / 32;
    int32_t bit_idx = pos % 32;
    cr->vertex_info[v].sg_id = color_sg_base[c] + seg_idx;
    cr->vertex_info[v].seg_mask = 1 << bit_idx;
  }

  free(color_counts);
  free(color_sg_base);
  free(color_pos);
}

ColoringResult* coloring_solve(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k, int32_t max_steps, int32_t seed) {
  ColoringResult* cr = malloc(sizeof(ColoringResult));
  cr->n_vertices = n_vertices;
  cr->vertex_info = malloc(n_vertices * sizeof(VertexInfo));

  int32_t* colors = _solve_sat(n_vertices, edges, n_edges, k, max_steps, seed);
  if (!colors) {
    free(cr->vertex_info);
    free(cr);
    return NULL;
  }
  
  _build_segments(cr, colors, k);
  free(colors);

  return cr;
}

void coloring_result_del(ColoringResult* cr) {
  if (!cr) return;
  free(cr->vertex_info);
  free(cr);
}

void coloring_get_segment_info(ColoringResult* cr, int32_t vertex_id, int32_t* out_sg_id, int32_t* out_seg_mask) {
  *out_sg_id = cr->vertex_info[vertex_id].sg_id;
  *out_seg_mask = cr->vertex_info[vertex_id].seg_mask;
}

int32_t coloring_get_sg_size(ColoringResult* cr) {
  return cr->sg_size;
}
