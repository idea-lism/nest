Use kisSAT to do graph coloring.

create src/coloring.c

Formalism: simple coloring, with simple symmetry-breaking.

After coloring there are groups of vertexes with the same color. If a group has more than 32 elements, break the group into segments (in a greedy way) such that each segment contains less or equal to 32 elements. Arrange the segments into seg_groups so that:
- every segment belongs to a seg_group
- each `seg_group.size <= 32`

With this optimization - arangement, we can
- assign each vertex a bit in a bitset (`int32_t seg_groups[sg_size]`)
- given a vertex id, we can find out its `sg_id` in the seg_group and the `seg_mask` that represents all the vertexes in the segment

## Symmetry breaking

Very simple: find one max clique, assign different colors to it.

## Graph helpers (src/graph.c)

For testing, src/graph.c provides:
- `Graph* graph_new(int32_t n_vertices)` - create empty graph
- `void graph_add_edge(Graph* g, int32_t u, int32_t v)` - add edge
- `Graph* graph_random_erdos_renyi(uint32_t n, double p)` - generate random graph (edge probability p)
- `int32_t* graph_edges(Graph* g)` - get edge array for coloring_solve
