Use kisSAT to do graph coloring.

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

## Symmetry Breaking

Very simple: find one max clique (with helper from graph.c), assign different colors to it.

## AMO (at most one) Encoding: Heule's Product Encoding

Instead of comparing every variable to every other variable, we map our $n$ variables into a rectangular grid of size $p \times q$, where $p \times q \ge n$. 

1.  **Introduce Auxiliary Variables:** We create $p$ variables to represent the **rows** ($r_1, r_2, \dots, r_p$) and $q$ variables to represent the **columns** ($c_1, c_2, \dots, c_q$).
2.  **The Mapping:** For each variable $x_{i,j}$ in the grid (representing one of your vertex-color pairs), we add two clauses:
    * $x_{i,j} \implies r_i$ (If the variable is true, its row must be "active")
    * $x_{i,j} \implies c_j$ (If the variable is true, its column must be "active")
3.  **The Constraints:**
    * Apply an AMO constraint to the **rows** ($r$).
    * Apply an AMO constraint to the **columns** ($c$).

## Binary Searching

We have LB (max clique) and UB (DSatur) in the beginning, we wan't to find out the a good coloring allocation.

We do a binary search for `LB <= k <= UB` to find the minimal coloring number that: SAT can give an answer in our limited time step settings.

## Graph helpers (src/graph.c)

For testing, src/graph.c provides:
- `Graph* graph_new(int32_t n_vertices)` - create empty graph
- `void graph_add_edge(Graph* g, int32_t u, int32_t v)` - add edge
- `Graph* graph_random_erdos_renyi(uint32_t n, double p)` - generate random graph (edge probability p)
- `int32_t* graph_edges(Graph* g)` - get edge array for coloring_solve
- `max_clique()` - get the max clique number for lower bound detection

## Windows fallback

kissat crashes in windows, we use DSatur algorithm instead. So in windows build we don't need to download/build kissat either.
