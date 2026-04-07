# Visibly pushdown generator

Create `src/vpa.c` (`void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix)`):
- `prefix` comes from `nest` command arg `-p`.
- define functions to generates visibly pushdown automata in LLVM IR, using Parser's processed-data
- generate helpers for result C header
  1. token id definitions
  2. util functions that the final LLVM IR may need

It iterates the parsed & desugared AST, utilize src/re.h to generate DFA, utilize src/irwriter.h to generate the upper level visibly pushdown machine.

### Input

```c
typedef struct {
  char* name;     // (owned)
  VpaUnit* units; // darray
  bool is_scope;
  bool is_macro;
  bool has_parser;
} VpaRule;

typedef struct {
  VpaRule* rules;         // darray
  KeywordEntry* keywords; // darray
  EffectDecl* effects;    // darray
  const char* src;
} VpaGenInput;
```

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

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

Assume there are 2 hooks `.foo` and `.bar` in lexer definition, generated header should have this interface

```c
typedef void (*LexHook)(void* userdata, Token* token, const char* token_str_start);

struct LexerContext {
  void* userdata;
  LexHook foo;
  LexHook bar;
};

struct LexResult {
  TokenTree* tt;
  const char* error;
  void (*cleanup)(LexResult* res); // a static function pointer to release the resources
};

extern ParseResult {prefix}_parse(LexerContext l, UStr src);
// TODO: incremental `edit` interface
```

If a user hook emits some token or some action, user should define it like this:

```c
// %effect .my_foo_hook = @bax | .fail
int32_t my_foo_hook(void* userdata, Token* token, const char* token_str_start) {
  ...
  if (...) {
    return TOKEN_BAZ;
  } else {
    return ACTION_FAIL;
  }
}
```

A typical usage example (the load_xxx functions are defined by [peg](peg.md)):

```c
struct LexerContext l = { NULL, .foo = my_foo_hook, .bar = my_bar_hook };
struct ParseResult res = {prefix}_parse(l, src_ustr);
if (res.error) {
  fprintf(stderr, "%s", res.error);
} else {
  MainNode n = {prefix}_load_main(res.main);
  ...
}
res.cleanup(res);
```

### Interaction with peg parsers

In LLVM-IR: define the main function `bool {prefix}_parse()`.

For each scope, PEG parsers generates a `parse_{scope_name}` function, the visibly pushdown machine just invoke the parsing function when a scope ends (`.end` action).
- Main scope doesn't have `.end` action, so after all input is consumed in {main_parse_fn_name}, call `parse_main()`

Since the VPA and the PEG share a same LLVM-IR writer, in PEG the parsing functions should be defined as internal, in VPA we can call them directly.

### The `{prefix}_parse` loop

Similar to the `_lex_scope` in the [parse spec][parse.md].

### Header generating

Resulting parser needs:

- common runtime
  - copy `build/nest_rt.inc`, which is string buf from header amalgamated from `src/nest_rt.h.in`
    - includes: input stream handling: use `ustr`
    - includes: token tree representation: use `token_tree`
- token ids: `TOK_XXX` numbered from [parse](parse.md)'s analysis.
- primitive action ids: `ACTION_XXX` numbered from [parse](parse.md)'s analysis.
- declare entrance and cleanup functions
  - `static ParseResult {prefix}_parse(LexerContext lc)`
  - `static void {prefix}_cleanup(ParseResult r)`
