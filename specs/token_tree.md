Scoped token stream, for visibly pushdown guided scoped parsing.

Files include
- src/token_tree.h
- src/token_tree.c

Both bootstrap parser and resulting parser use this tree representation.

### From byte stream to codepoint stream to token stream

The input is byte stream, with [ustr](ustr.md), we already have a bitmap to index the starting positions of codepoints.

By feeding the chars one by one, the lexer construct another bitmap to index the newlines: each bit represents a codepoint, `1` represents the codepoint is a newline. With this index, given a cp_offset, we can quickly locate the line and column by popcnt instruction.

So the data structures are:

```c
// 16 bytes a token
typedef struct {
  int32_t tok_id; // or scope_id (is scope id when < SCOPE_COUNT), parse analysis should give a universal numbering to all of them
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
  int32_t parent_id; // parent chunk_id, -1 for root chunk
  void* value;       // parser associate a value to it after parse, `struct ScopeXxx`
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

### Helper functions

```c
TokenTree* tc_tree_new(ustr);
void tc_tree_del(TokenTree*);
struct {line, col} tc_locate(TokenTree* tree, int32_t cp_offset);
tc_add(TokenChunk* c, Token t);
TokenChunk* tc_push(TokenTree* tree); // updates current
TokenChunk* tc_pop(TokenTree* tree);  // returns new current
```
