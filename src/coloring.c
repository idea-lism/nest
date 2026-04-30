#include "coloring.h"
#include "graph.h"
#include "xmalloc.h"
#include <string.h>

typedef struct {
  int32_t sg_id;
  int64_t seg_mask;
} VertexInfo;

struct ColoringResult {
  int32_t n_vertices;
  int32_t sg_size;
  VertexInfo* vertex_info;
  int32_t* colors;
};

// DSatur: pick uncolored vertex with max saturation (ties broken by degree), assign smallest feasible color.
// Returns colors or NULL if k is insufficient.
static int32_t* _solve_dsatur(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k) {
  int32_t* adj = XCALLOC(n_vertices * n_vertices, sizeof(int32_t));
  int32_t* degree = XCALLOC(n_vertices, sizeof(int32_t));
  for (int32_t i = 0; i < n_edges; i++) {
    int32_t u = edges[i * 2], v = edges[i * 2 + 1];
    adj[u * n_vertices + v] = 1;
    adj[v * n_vertices + u] = 1;
    degree[u]++;
    degree[v]++;
  }

  int32_t* colors = XMALLOC(n_vertices * sizeof(int32_t));
  memset(colors, -1, n_vertices * sizeof(int32_t));

  // sat[v] = number of distinct colors among v's colored neighbors
  int32_t* sat = XCALLOC(n_vertices, sizeof(int32_t));
  // neighbor_colors[v * k + c] = 1 if v has a neighbor colored c
  int32_t* neighbor_colors = XCALLOC(n_vertices * k, sizeof(int32_t));

  for (int32_t step = 0; step < n_vertices; step++) {
    // pick vertex with max saturation, break ties by degree
    int32_t best = -1;
    for (int32_t v = 0; v < n_vertices; v++) {
      if (colors[v] >= 0) {
        continue;
      }
      if (best < 0 || sat[v] > sat[best] || (sat[v] == sat[best] && degree[v] > degree[best])) {
        best = v;
      }
    }

    // assign smallest color not used by neighbors
    int32_t c;
    for (c = 0; c < k; c++) {
      if (!neighbor_colors[best * k + c]) {
        break;
      }
    }
    if (c >= k) {
      XFREE(adj);
      XFREE(degree);
      XFREE(sat);
      XFREE(neighbor_colors);
      XFREE(colors);
      return NULL;
    }
    colors[best] = c;

    // update saturation of uncolored neighbors
    for (int32_t v = 0; v < n_vertices; v++) {
      if (adj[best * n_vertices + v] && colors[v] < 0) {
        if (!neighbor_colors[v * k + c]) {
          neighbor_colors[v * k + c] = 1;
          sat[v]++;
        }
      }
    }
  }

  XFREE(adj);
  XFREE(degree);
  XFREE(sat);
  XFREE(neighbor_colors);
  return colors;
}

static void _build_segments(ColoringResult* cr, int32_t* colors, int32_t k) {
  int32_t* color_counts = XCALLOC(k, sizeof(int32_t));
  for (int32_t i = 0; i < cr->n_vertices; i++) {
    color_counts[colors[i]]++;
  }

  int32_t sg_id = 0;
  int32_t* color_sg_base = XMALLOC(k * sizeof(int32_t));

  for (int32_t c = 0; c < k; c++) {
    color_sg_base[c] = sg_id;
    int32_t count = color_counts[c];
    sg_id += (count + 63) / 64;
  }
  cr->sg_size = sg_id;

  int32_t* color_pos = XCALLOC(k, sizeof(int32_t));
  for (int32_t v = 0; v < cr->n_vertices; v++) {
    int32_t c = colors[v];
    int32_t pos = color_pos[c]++;
    int32_t seg_idx = pos / 64;
    int32_t bit_idx = pos % 64;
    cr->vertex_info[v].sg_id = color_sg_base[c] + seg_idx;
    cr->vertex_info[v].seg_mask = (int64_t)(1ULL << bit_idx);
  }

  XFREE(color_counts);
  XFREE(color_sg_base);
  XFREE(color_pos);
}

#ifdef _WIN32

// Windows fallback: DSatur only, no SAT solver.

ColoringResult* coloring_solve(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t max_steps, int32_t seed,
                               bool use_product_encoding, FILE* log) {
  (void)max_steps;
  (void)seed;
  (void)use_product_encoding;
  (void)log;

  // DSatur always succeeds with n_vertices colors.
  int32_t* colors = _solve_dsatur(n_vertices, edges, n_edges, n_vertices);
  if (!colors) {
    return NULL;
  }

  ColoringResult* cr = XMALLOC(sizeof(ColoringResult));
  cr->n_vertices = n_vertices;
  cr->vertex_info = XMALLOC(n_vertices * sizeof(VertexInfo));
  _build_segments(cr, colors, n_vertices);
  cr->colors = XMALLOC(n_vertices * sizeof(int32_t));
  memcpy(cr->colors, colors, n_vertices * sizeof(int32_t));
  XFREE(colors);
  return cr;
}

#else

// Non-Windows: use kissat SAT solver with binary search between LB and UB.

// Compute the lower bound: max clique size.
static int32_t _max_clique(int32_t n_vertices, int32_t* edges, int32_t n_edges) {
  Graph* g = graph_new(n_vertices);
  for (int32_t i = 0; i < n_edges; i++) {
    graph_add_edge(g, edges[i * 2], edges[i * 2 + 1]);
  }
  int32_t* clique = graph_find_max_clique(g);
  graph_del(g);
  int32_t lb = clique ? clique[0] : 0;
  if (clique) {
    XFREE(clique);
  }
  return lb;
}

typedef struct kissat kissat;
extern kissat* kissat_init(void);
extern void kissat_add(kissat* solver, int lit);
extern int kissat_solve(kissat* solver);
extern int kissat_value(kissat* solver, int lit);
extern void kissat_release(kissat* solver);
extern void kissat_set_conflict_limit(kissat* solver, unsigned limit);
extern int kissat_set_option(kissat* solver, const char* name, int new_value);
extern void kissat_reserve(kissat* solver, int max_var);

// Variable index: vertex v, color c, k colors total.
static int32_t _var(int32_t v, int32_t c, int32_t k) { return v * k + c + 1; }

// SAT solver for a single k value. Returns colors or NULL on failure.
// *out_timeout is set to true if the solver hit the conflict limit (UNKNOWN result).
static int32_t* _solve_sat(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k, int32_t max_steps,
                           int32_t seed, bool use_product_encoding, bool* out_timeout) {
  *out_timeout = false;
  kissat* solver = kissat_init();
  kissat_set_option(solver, "seed", seed);
  kissat_set_option(solver, "quiet", 1);

  int32_t max_var = n_vertices * k;
  int32_t p = 0, q = 0;
  if (use_product_encoding) {
    p = 1;
    while (p * p < k) {
      p++;
    }
    q = (k + p - 1) / p;
    max_var += n_vertices * (p + q);
  }
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
    if (use_product_encoding) {
      int32_t v_offset = n_vertices * k + v * (p + q);
      for (int32_t c = 0; c < k; c++) {
        int32_t i = c / q;
        int32_t j = c % q;
        // x_{v,c} => r_{v,i}
        kissat_add(solver, -_var(v, c, k));
        kissat_add(solver, v_offset + i + 1);
        kissat_add(solver, 0);
        // x_{v,c} => c_{v,j}
        kissat_add(solver, -_var(v, c, k));
        kissat_add(solver, v_offset + p + j + 1);
        kissat_add(solver, 0);
      }
      // AMO on rows
      for (int32_t i1 = 0; i1 < p; i1++) {
        for (int32_t i2 = i1 + 1; i2 < p; i2++) {
          kissat_add(solver, -(v_offset + i1 + 1));
          kissat_add(solver, -(v_offset + i2 + 1));
          kissat_add(solver, 0);
        }
      }
      // AMO on columns
      for (int32_t j1 = 0; j1 < q; j1++) {
        for (int32_t j2 = j1 + 1; j2 < q; j2++) {
          kissat_add(solver, -(v_offset + p + j1 + 1));
          kissat_add(solver, -(v_offset + p + j2 + 1));
          kissat_add(solver, 0);
        }
      }
    } else {
      for (int32_t c1 = 0; c1 < k; c1++) {
        for (int32_t c2 = c1 + 1; c2 < k; c2++) {
          kissat_add(solver, -_var(v, c1, k));
          kissat_add(solver, -_var(v, c2, k));
          kissat_add(solver, 0);
        }
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

  // Symmetry breaking: assign different colors to max clique vertices.
  Graph* g = graph_new(n_vertices);
  for (int32_t i = 0; i < n_edges; i++) {
    graph_add_edge(g, edges[i * 2], edges[i * 2 + 1]);
  }
  int32_t* clique = graph_find_max_clique(g);
  graph_del(g);

  if (clique) {
    int32_t clique_size = clique[0];
    for (int32_t i = 0; i < clique_size && i < k; i++) {
      kissat_add(solver, _var(clique[i + 1], i, k));
      kissat_add(solver, 0);
    }
    XFREE(clique);
  }

  int result = kissat_solve(solver);
  int32_t* colors = NULL;
  if (result == 0) {
    *out_timeout = true;
  } else if (result == 10) {
    colors = XMALLOC(n_vertices * sizeof(int32_t));
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

// Binary search for the minimal k between lb and ub that SAT can solve.
// initial_best is a known valid coloring (ownership transferred); returned as-is if no better k is found.
static ColoringResult* _binary_search_color(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t lb, int32_t ub,
                                            int32_t max_steps, int32_t seed, bool use_product_encoding, FILE* log,
                                            ColoringResult* best) {
  if (lb > ub) {
    return best;
  }

  int32_t lo = lb, hi = ub;
  int32_t iter = 0;

  while (lo <= hi) {
    // Bias toward UB (likely SAT region). When hi-lo==1 this gives mid=lo, still progresses.
    int32_t mid = lo + (hi - lo) * 2 / 3;
    bool timeout = false;
    int32_t* colors = _solve_sat(n_vertices, edges, n_edges, mid, max_steps, seed, use_product_encoding, &timeout);
    if (log) {
      fprintf(log, "  [coloring] iter %d: lo=%d hi=%d try k=%d => %s\n", iter, lo, hi, mid,
              colors ? "SAT" : (timeout ? "timeout" : "UNSAT"));
    }
    iter++;
    if (colors) {
      // mid works, try smaller
      ColoringResult* cr = XMALLOC(sizeof(ColoringResult));
      cr->n_vertices = n_vertices;
      cr->vertex_info = XMALLOC(n_vertices * sizeof(VertexInfo));
      _build_segments(cr, colors, mid);
      cr->colors = XMALLOC(n_vertices * sizeof(int32_t));
      memcpy(cr->colors, colors, n_vertices * sizeof(int32_t));
      XFREE(colors);
      coloring_result_del(best);
      best = cr;
      hi = mid - 1;
    } else {
      // mid doesn't work, need more colors
      lo = mid + 1;
    }
  }

  return best;
}

ColoringResult* coloring_solve(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t max_steps, int32_t seed,
                               bool use_product_encoding, FILE* log) {
  // Compute LB (max clique) and UB (DSatur).
  int32_t lb = _max_clique(n_vertices, edges, n_edges);

  int32_t* dsatur_colors = _solve_dsatur(n_vertices, edges, n_edges, n_vertices);
  int32_t ub;
  ColoringResult* dsatur_cr = NULL;
  if (dsatur_colors) {
    // Find the max color used by DSatur to get a tight UB.
    ub = 0;
    for (int32_t i = 0; i < n_vertices; i++) {
      if (dsatur_colors[i] > ub) {
        ub = dsatur_colors[i];
      }
    }
    ub++;
    // Build the DSatur result now — it is a valid ub-coloring and serves as the
    // initial best so SAT never needs to probe k = ub.
    dsatur_cr = XMALLOC(sizeof(ColoringResult));
    dsatur_cr->n_vertices = n_vertices;
    dsatur_cr->vertex_info = XMALLOC(n_vertices * sizeof(VertexInfo));
    _build_segments(dsatur_cr, dsatur_colors, ub);
    dsatur_cr->colors = dsatur_colors; // transfer ownership
  } else {
    // DSatur failed (shouldn't happen with n_vertices colors, but be safe).
    ub = n_vertices;
  }

  if (log) {
    fprintf(log, "  [coloring] n=%d edges=%d lb=%d (max clique) ub=%d (DSatur)\n", n_vertices, n_edges, lb, ub);
  }

  // Binary search between lb and ub-1: DSatur already covers ub.
  // dsatur_cr ownership is transferred to _binary_search_color.
  ColoringResult* cr =
      _binary_search_color(n_vertices, edges, n_edges, lb, ub - 1, max_steps, seed, use_product_encoding, log, dsatur_cr);
  if (log && cr) {
    fprintf(log, "  [coloring] result: %d colors\n", coloring_get_sg_size(cr));
  }
  return cr;
}

#endif

void coloring_result_del(ColoringResult* cr) {
  if (!cr) {
    return;
  }
  XFREE(cr->vertex_info);
  XFREE(cr->colors);
  XFREE(cr);
}

int32_t coloring_get_color(ColoringResult* cr, int32_t vertex_id) { return cr->colors[vertex_id]; }

void coloring_get_segment_info(ColoringResult* cr, int32_t vertex_id, int32_t* out_sg_id, int64_t* out_seg_mask) {
  *out_sg_id = cr->vertex_info[vertex_id].sg_id;
  *out_seg_mask = cr->vertex_info[vertex_id].seg_mask;
}

int32_t coloring_get_sg_size(ColoringResult* cr) { return cr->sg_size; }

int32_t coloring_get_n_vertices(ColoringResult* cr) { return cr->n_vertices; }
