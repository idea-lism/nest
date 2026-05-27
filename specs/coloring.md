Use DSatur to do graph coloring.

create src/coloring.c

Interface:

```c
ColoringResult* coloring_solve(int32_t n_vertices, int32_t* edges, int32_t n_edges, int32_t k, int32_t max_steps,
                               int32_t seed, bool use_product_encoding);
```

Formalism: simple coloring, with simple symmetry-breaking.

After coloring there are groups of vertexes with the same color. If a group has more than 64 elements, break the group into segments (in a greedy way) such that each segment contains less or equal to 64 elements. Arrange the segments into seg_groups so that:
- every segment belongs to a seg_group
- each `seg_group.size <= 64`

With this optimization - arangement, we can
- assign each vertex a bit in a bitset (`uint64_t seg_groups[sg_size]`)
- given a vertex id, we can find out its `sg_id` in the seg_group and the `seg_mask` that represents all the vertexes in the segment

## Graph helpers (src/graph.c)

For testing, src/graph.c provides:
- `Graph* graph_new(int32_t n_vertices)` - create empty graph
- `void graph_add_edge(Graph* g, int32_t u, int32_t v)` - add edge
- `Graph* graph_random_erdos_renyi(uint32_t n, double p)` - generate random graph (edge probability p)
- `int32_t* graph_edges(Graph* g)` - get edge array for coloring_solve
