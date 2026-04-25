#pragma once

#include <stdint.h>

typedef struct ColoringResult ColoringResult;

ColoringResult* coloring_solve(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k, int32_t max_steps,
                               int32_t seed, bool use_product_encoding);
void coloring_result_del(ColoringResult* cr);
void coloring_get_segment_info(ColoringResult* cr, int32_t vertex_id, int32_t* out_sg_id, int64_t* out_seg_mask);
int32_t coloring_get_sg_size(ColoringResult* cr);
int32_t coloring_get_n_vertices(ColoringResult* cr);
int32_t coloring_get_color(ColoringResult* cr, int32_t vertex_id);
