# Visibly pushdown generator

Create `src/vpa.c` (`vpa_gen()`):
- define functions to generates visibly pushdown automata in LLVM IR, using Parser's processed-data
- generate helpers for result C header
  1. token id definitions
  2. util functions that the final LLVM IR may need

It iterates the parsed & desugared AST, utilize src/re.h to generate DFA, utilize src/irwriter.h to generate the upper level visibly pushdown machine.

### Scope handling

Every scope, except the `main`, has a starting regex. After the starting regex is matched, following hooks or tokens can be emitted, or get into child scope looping.

The built-in hook `.begin` / `.end` denotes when to push / pop a TokenChunk. When popping a chunk (scope), if it is mapped to a PEG rule, invoke the PEG parsing -- this is known in compile time, so generated code will hard-code the parsing funciton call for the `.end` action.

Sub-scope calls inlines the leader regexp as matcher. Assume we have a scope like this:

```c
# braced
s = /regex1/ {
  /regex2/
  a
}

a = /regex3/ .hook1 {
  /regex4/
}
```

The matcher for `s` first matches `/regex1/`, then the braced union. Since `a` is called, the brace in `s` should be compiled to the union of `/regex2/` and `/regex3/` (the leader regexp of `a`). And if `/regex3/` is matched, go into `a`'s following processing: 1. invoke `.hoo1`, 2. loop into `a`'s braced union with `/regex4/`.

Note that the automata's `action_id` is not token_id or scope id. each action_id should map to a sequence of actions like invoke hook / emit token, so we will have a label array, and utilize the computed-goto extension (there should be equivalent one in LLVM IR) to invoke the series of actions quickly.

### Parallel state matching

When a hook invokes state matching like the following example:

```c
main = {
  /regexp1/ # action 1
  $st
  /regexp2/ # action 2
}
```

The generated code will have automata as the union of `/regexp1/` and `/regexp2/`, a user-defined state matcher is also invoked to check if input matches `$st` state. If state matcher matches but `action 1` is produced, `action 1` wins. If state matcher matches and only `action 2` is produced, state matcher wins.

### From byte stream to codepoint stream to token stream

The input is byte stream, with [ustr](ustr.md), we already have a bitmap to index the starting positions of codepoints.

By feeding the chars one by one, the lexer construct another bitmap to index the newlines: each bit represents a codepoint, `1` represents the codepoint is a newline. With this index, given a cp_offset, we can quickly locate the line and column by popcnt instruction.

So the data structures are:

```c
struct TokenTree {
  uint64_t newline_map[];
  uint64_t token_end_map[];
  TokenChunk* root;
  TokenChunk* current;
}

struct TokenChunk { // matches a scope
  int32_t chunk_id;
  int32_t scope_id;
  Token tokens[];
}

// 16 bytes a token
union Token {
  int32_t tok_id; // or scope_id (negative), parse analysis should give a universal numbering to all of them
  // with cp_start (absolute offset relative to input string):
  // - we can locate the line & column with newline_map
  // - we can locate the byte offset by ustr
  int32_t cp_start;
  int32_t cp_size;
  int32_t chunk_id; // when tok_id is a scope, it can be expanded to a TokenChunk
}

struct ChunkTable {
  TokenChunk chunks[]; // indexed by chunk_id
}
```

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

- user must define `%state` and `.` hooks for lexing
- each state is an opaque type `void*`, user have custom interpretation of it (string? number? string array? etc).
  - in hooks user can update states.
  - if states are used in matching, user also need to implement `match_{state_id}(const char*, userdata)` so lexer can do dynamic matching by current state. for example:
    - `foo = $foo` means the matching of the `foo` rule is totally customed by `match_foo(current_src_pointer, userdata)`
