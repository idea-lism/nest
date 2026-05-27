#include "coloring.h"
#include "xmalloc.h"
#include <string.h>

typedef struct {
  int32_t seg_group_id;
  int64_t seg_mask;
} VertexInfo;

struct ColoringResult {
  int32_t vertex_size;
  int32_t seg_group_size;
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
  for (int32_t i = 0; i < cr->vertex_size; i++) {
    color_counts[colors[i]]++;
  }

  int32_t seg_group_id = 0;
  int32_t* color_seg_group_base = XMALLOC(k * sizeof(int32_t));

  for (int32_t c = 0; c < k; c++) {
    color_seg_group_base[c] = seg_group_id;
    int32_t count = color_counts[c];
    seg_group_id += (count + 63) / 64;
  }
  cr->seg_group_size = seg_group_id;

  int32_t* color_pos = XCALLOC(k, sizeof(int32_t));
  for (int32_t v = 0; v < cr->vertex_size; v++) {
    int32_t c = colors[v];
    int32_t pos = color_pos[c]++;
    int32_t seg_idx = pos / 64;
    int32_t bit_idx = pos % 64;
    cr->vertex_info[v].seg_group_id = color_seg_group_base[c] + seg_idx;
    cr->vertex_info[v].seg_mask = (int64_t)(1ULL << bit_idx);
  }

  XFREE(color_counts);
  XFREE(color_seg_group_base);
  XFREE(color_pos);
}

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
  cr->vertex_size = n_vertices;
  cr->vertex_info = XMALLOC(n_vertices * sizeof(VertexInfo));
  _build_segments(cr, colors, n_vertices);
  cr->colors = XMALLOC(n_vertices * sizeof(int32_t));
  memcpy(cr->colors, colors, n_vertices * sizeof(int32_t));
  XFREE(colors);
  return cr;
}

void coloring_result_del(ColoringResult* cr) {
  if (!cr) {
    return;
  }
  XFREE(cr->vertex_info);
  XFREE(cr->colors);
  XFREE(cr);
}

int32_t coloring_get_color(ColoringResult* cr, int32_t vertex_id) { return cr->colors[vertex_id]; }

void coloring_get_segment_info(ColoringResult* cr, int32_t vertex_id, int32_t* out_seg_group_id,
                               int64_t* out_seg_mask) {
  *out_seg_group_id = cr->vertex_info[vertex_id].seg_group_id;
  *out_seg_mask = cr->vertex_info[vertex_id].seg_mask;
}

int32_t coloring_get_seg_group_size(ColoringResult* cr) { return cr->seg_group_size; }

int32_t coloring_get_n_vertices(ColoringResult* cr) { return cr->vertex_size; }
