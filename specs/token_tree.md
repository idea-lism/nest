Scoped token stream, for visibly pushdown guided scoped parsing.

Files include
- src/token_tree.h
- src/token_tree.c

Both bootstrap parser and resulting parser use this tree representation.

### From byte stream to codepoint stream to token stream

The input is byte stream, with [ustr](ustr.md), we already have a bitmap to index the starting positions of codepoints.

By feeding the chars one by one, the lexer constructs another bitmap to index the newlines: each bit represents a codepoint, `1` represents the codepoint is a newline. With this index, given a cp_offset, `tt_locate` quickly computes 1-based line and column by popcnt instruction.

So the data structures are:

```c
// 16 bytes a token
typedef struct {
  int32_t term_id; // token_id or scope_id (in the same numbering system), parse analysis should give a universal numbering to all of them
  // with cp_start (absolute offset relative to input string):
  // - we can locate the line & column with newline_map
  // - we can locate the byte offset by ustr
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id; // when tok_id is a scope, it can be expanded to a TokenChunk
} Token;

// add typedef so we don't mess darray fat pointers with normal pointers
typedef Token* Tokens;

typedef struct { // matches a scope
  int32_t scope_id;
  int32_t parent_id; // parent chunk_id, -1 for root chunk, can be used by "pop"
  void* value;       // parser associate a value to it after parse, `struct ScopeXxx`
  void* aux_value;   // parser associate another value to it
  Tokens tokens;
} TokenChunk;

// darray
typedef TokenChunk* TokenChunks;

typedef struct {
  const char* src; // = ustr
  uint64_t newline_map[]; // bitmap = (ustr_size(ustr) + 63) / 64
  TokenChunk* root; // pointer to the root chunk
  TokenChunk* current; // pointer to the current chunk
  TokenChunks table; // indexed by chunk_id
} TokenTree;
```

current is always non-null.

### Helper functions

```c
TokenTree* tt_tree_new(ustr);
void tt_tree_del(TokenTree*, bool free_values);
struct {line, col} tt_locate(TokenTree* tree, int32_t cp_offset); // 1-based line and col
tt_add(TokenTree* tree, int32_t tok_id, int32_t cp_start, int32_t cp_size, int32_t chunk_id);
TokenChunk* tt_push(TokenTree* tree, int32_t scope_id);
// generated parser only: also set the token_chunk.aux_value = token_tree
TokenChunk* tt_push_assoc(TokenTree* tree, int32_t scope_id);
// Pop current chunk. Add scope-ref token to parent chunk.
// cp_end = cp_start + cp_size of the closing token.
// scope-ref: term_id = child scope_id, chunk_id = child index in table,
//   cp_start = child's first token's cp_start, cp_size = cp_end - cp_start.
TokenChunk* tt_pop(TokenTree* tree, int32_t cp_end);

// current chunk token count
int64_t tt_current_size(TokenTree* tree);
// get current chunk
TokenChunk* tt_current(TokenTree* tree);
// alloc sizeof_col * (token_count + 1) to value, +1 for sentinel column
// sizeof_col must be multiple of 8, returned address must be 64-bit aligned
void* tt_alloc_memoize_table(TokenChunk* chunk, int64_t sizeof_col);
```

With `tc = tt_current(tree)`, we can check `tc->scope_id`, `tc->tokens[col]`

### Sentinel token

Since all valid token_id and scope_id are non-zero (see [parse.md](parse.md)), a zero-initialized Token acts as a natural sentinel.

**tt_add**: not use `darray_push` directly, instead call `darray_grow` with `size+2` then set the ptr value, so there's always at least a sentinel token with id=0 at the end.

**tt_alloc_memoize_table**: allocates `sizeof_col * (token_count + 1)` bytes. Naturally the end slot has `-1`

This eliminates bounds checks in:
- **PEG IR term matching** (`_emit_term`): reading `tokens[col].term_id` at end-of-stream returns 0, which never equals any valid term_id → natural mismatch, branches to fail. No explicit `col < token_size` check needed.
- **Generated `has_elem`**: No explicit `col >= darray_size(tokens)` check needed.
- **Generated `peg_size`**: sentinel column slot reads as 0, return 0 (no match). No explicit bounds check needed.
- **Generated loader cursor advance**: sentinel column naturally stops advancement.
