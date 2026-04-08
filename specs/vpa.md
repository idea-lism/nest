# Visibly pushdown generator

Create `src/vpa.c` (`void vpa_gen(VpaGenInput* input, HeaderWriter* hw, IrWriter* w, const char* prefix)`):
- `prefix` comes from `nest` command arg `-p`.
- define functions to generates visibly pushdown automata in LLVM IR, using Parser's processed-data
- generate helpers for result C header
  1. token id definitions
  2. util functions that the final LLVM IR may need

It iterates the parsed & desugared AST, utilize src/re.h to generate DFA, utilize src/irwriter.h to generate the upper level visibly pushdown machine.

### Input & input processing

```c
typedef enum {
  VPA_RE, // for literals, we have VpaUnit.re built from string, and action_units = { literal_tok_id }
  VPA_CALL,
  VPA_MACRO_REF,
} VpaUnitKind;

typedef int32_t* VpaActionUnits;

typedef struct {
  VpaUnitKind kind;

  // kind = VPA_RE
  ReIr re;           // a flattened regexp representation
  bool binary_mode;  // true if tagged with 'b' mode

  // kind = VPA_CALL
  int32_t call_scope_id;

  // kind = VPA_MACRO_REF
  // expanded in post_process
  char* macro_name;

  // see the `action` rule in bootstrap.nest and numbering in parse.md
  // action_unit_id > 0: maps to token_id
  // action_unit_id <= 0: maps to -hook_id
  VpaActionUnits action_units;
} VpaUnit;

// use `VpaUnits` when we mean a darray
typedef VpaUnit* VpaUnits;

typedef struct {
  int32_t scope_id; // only non-macro scopes have scope_id, incremental
  char* name;       // (owned)
  VpaUnit leader;
  VpaUnits children;
  bool is_macro; // after pp_inline_macros no macro scope is left
  bool has_parser;
} VpaScope;

typedef struct {
  int32_t hook_id;
  VpaActionUnits effects;
} EffectDecl;
typedef EffectDecl* EffectDecls;

typedef struct {
  VpaScope* scopes;
  EffectDecls effect_decls; // owned by ParseState
  Symtab tokens; // owned by ParseState, can be used to lookup token name, start from 1
  Symtab hooks;  // owned by ParseState, can be used to lookup hook name, start from 0
} VpaGenInput;
```

A first pre-processing is to create actions table:

```c
typedef struct {
  int32_t action_id; // map to index in Acitons array, start from 1
  VpaActionUnits action_units; // from VpaUnit, not owned
} Action;

typedef Action* Actions; // darray, prefill actions[0] with an empty entry
```

### Scope handling

Every scope, except the `main`, has a starting regex. After the starting regex is matched, following hooks or tokens can be emitted, or get into child scope looping.

For all scopes, the built-in hook `.begin` / `.end` will push / pop the VPA stack.

For scopes with `.has_parser`, the built-in hook `.begin` / `.end` also denotes when to push / pop a TokenChunk too.
When popping a chunk (scope), invoke the PEG parsing -- this is known in compile time, so generated code will hard-code the parsing funciton call for the `.end` action.

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

The automata's `action_id` is the index in actions array, codegen will create basic blocks in LLVM IR for all actions, invoking the action units in sequence.
- With the label array and computed-goto extension, we can dispatch by action_id.
- if a user hook have `%effect` definition, check if returned value is within definition:
  - if yes, emit token or execute primitive hook.
  - if no, add parse error and terminate parsing (TODO: continuable parsing)
- if a user hook doesn't have `%effect` definition, the returned value is ignored.

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

Assume there are 2 hooks `.foo` and `.bar` in lexer definition, generated header should have this interface

```c
typedef int32_t (*LexHook)(void* userdata, Token* token, const char* token_str_start);

typedef struct {
  void* userdata;
  LexHook foo;
  LexHook bar;
} ParseContext;

typedef enum {
  PARSE_ERROR_INVALID_HOOK,
  PARSE_ERROR_REQUIRE_MORE_INPUT, // met eof while the scope stack size is > 1
  PARSE_ERROR_TOKEN_ERR, // expects one of pattern matches in a scope
  PARSE_ERROR_INVALID_SYNTAX, // peg parse fails
} ParseErrorType;

typedef struct {
  const char* message;
  ParseErrorType type;
  int32_t cp_offset;
  int32_t cp_size;
} ParseError;
typedef ParseError* ParseErrors;

typedef struct {
  PegRef main; // root node after parse
  TokenTree* tt;
  ParseErrors errors;
} ParseResult;

extern ParseResult {prefix}_parse(ParseContext l, UStr src);
// TODO: incremental `edit` interface
```

If a user hook emits some token or some action, user should define it like this:

```c
// %effect .my_foo_hook = @bax | .fail
// the return value is in the same numbering system as action_unit_id
int32_t my_foo_hook(void* userdata, Token* token, const char* token_str_start) {
  ...
  if (...) {
    return TOKEN_BAZ;
  } else {
    return HOOK_FAIL;
  }
}
```

A typical usage example (the load_xxx functions are defined by [peg](peg.md)):

```c
struct ParseContext l = { NULL, .foo = my_foo_hook, .bar = my_bar_hook };
struct ParseResult res = {prefix}_parse(l, src_ustr);
if (res.error) {
  fprintf(stderr, "%s", res.error);
} else {
  MainNode n = {prefix}_load_main(res.main);
  ...
}
{prefix}_cleanup(res);
```

### Interaction with peg parsers

In LLVM-IR: define the main function `ParseResult {prefix}_parse(l, src_ustr)`.

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
- token ids: `TOK_XXX` numbered in the system of action_unit_id.
- primitive hook ids: `HOOK_XXX` numbered in the system of action_unit_id.
- declare entrance and cleanup functions (impl in LLVM-IR)
  - `ParseResult {prefix}_parse(ParseContext lc, UStr src)`
  - `void {prefix}_cleanup(ParseResult r)`
