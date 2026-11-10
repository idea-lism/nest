# Scoped lexer (VPA)

`vpa` compiles scoped regexps into per-scope DFAs and drives the character-level
scan. Its output is a stream of events (`token` / `hook`, with a source span),
not a token tree. Each event is fed immediately to the LL(1) parser of the
innermost active scope that has one; the scope stack is the feed path. No token
array or token chunk is materialized for parsing.

This spec fixes the input contract and VPA semantics only. Table encoding,
generation, and runtime output are [vm](vm.md) concerns.

## Input

An action is an ordered list of action units. A unit either emits a term or
runs a builtin/user hook.

```c
typedef enum {
  VPA_RE,        // regexp matcher; action_units normally holds its literal token
  VPA_CALL,      // call another scope
  VPA_MACRO_REF, // expanded by post_process; absent from VPA input
  VPA_EOF,       // matches end of input
} VpaUnitKind;

// > 0: term id (token or scope, see parse.md); < 0: -hook_id
typedef int32_t* VpaActionUnits;

typedef struct {
  VpaUnitKind kind;
  ReIr re;             // kind = VPA_RE
  bool binary_mode;    // 'b' mode
  int32_t call_scope_id;
  char* macro_name;    // kind = VPA_MACRO_REF, before post_process
  VpaActionUnits action_units;
  int32_t source_line; // diagnostics only; 0 = unknown
  int32_t source_col;
} VpaUnit;

typedef VpaUnit* VpaUnits;

typedef struct {
  int32_t scope_id;     // stable id used by the runtime
  char* name;           // owned
  VpaUnit leader;       // every scope except main has one
  VpaUnits children;    // body lines: regexp + actions, or scope call
  bool is_macro;        // removed by pp_inline_macros
  bool has_parser;      // same-name, non-TODO LL(1) rule exists
  int32_t eof_action;   // action for VPA_EOF, 0 = none
  int32_t source_line;
  int32_t source_col;
} VpaScope;

typedef struct {
  int32_t hook_id;
  VpaActionUnits effects; // allowed return values
} EffectDecl;
typedef EffectDecl* EffectDecls;

typedef struct {
  VpaScope* scopes;
  EffectDecls effect_decls; // owned by ParseState
  Symtab tokens;            // owned by ParseState, 1-based
  Symtab hooks;             // owned by ParseState
  Symtab ignore_names;      // tokens suppressed from the feed
  const char* source_file_name;
  ReFrags re_frags;         // owned by ParseState
} VpaGenInput;
```

Preprocess once into an action table. The table is indexed by action id;
entry 0 is the empty action.

```c
typedef struct {
  VpaActionUnits action_units; // not owned
  int32_t begin_scope_id;      // target of .begin, -1 if none
} Action;
```

`end_scope_name` is gone: `.end` pops the runtime scope stack, so no generated
`parse_{name}` callback is needed.

## Actions

Units run in order.

- `id > 0` feeds the term to the active parser; ignored tokens are dropped
  before feeding.
- `id < 0` runs hook `-id`.
- Builtins: `.begin` pushes a scope, `.end` pops and completes it, `.fail`
  aborts lexing, `.unparse` puts the matched input back (no progress), `.noop`
  does nothing.
- A user hook with `%effect` must return a declared effect. Anything else is
  fatal. A hook without `%effect` has its return value ignored.
- An empty action is valid: consume input and commit progress without emitting
  a term or running a hook.
- Greedy match; when several actions accept on the same state, the smallest
  action id wins ([aut](aut.md) MIN-RULE).

## Scopes and leader inlining

`main` is the entry and has no leader. Every other scope has a leader regexp;
once it matches, the scope body is the union of its lines.

A scope call in a body inlines the callee's leader regexp. If that leader
matches, run the callee's leader actions, then continue with the callee's body.
Thus, for

```nest
s = /regex1/ {
  /regex2/
  a
}

a = /regex3/ .hook1 {
  /regex4/
}
```

the matcher for `s` is `/regex1/` followed by `/regex2/ | /regex3/`. Matching
`/regex3/` runs `.hook1` and then continues in `a`'s body.

`.begin` / `.end` push/pop the scope stack for every scope. `has_parser` only
controls whether `.begin` also attaches a new LL(1) parser instance.

## Token feed

- The innermost active parser receives every term. Scopes without a parser are
  transparent: their terms feed the nearest enclosing parser.
- On `.begin` of a scope with a parser, create the child parser. The parent
  parser is suspended; it does not see the child's tokens.
- On `.end`, finalize the child parser, then feed exactly one scope term to the
  parent parser. A parse error is recoverable: record it, finish/close the
  child, and still feed the scope term to the parent.
- This is why no token chunk is needed: the parent sees one term per
  materialized scope, at the point the child finishes.
- `.begin` / `.end` of a scope without a parser only move the scope stack.
- The scope's `eof_action`, if any, fires at end of input before the final
  mismatch.
- Lexer failure is fatal: no transition, `.fail`, an invalid `%effect`, or an
  out-of-range hook result stops scanning immediately.

## Output contract

Each scope yields one DFA (states, transition ranges, accepting action id) and
an EOF action id. The action table above is emitted alongside it. Term ids are
the `action_unit_id` contract; `TOK_*` / `HOOK_*` constants belong to codegen,
not VPA.

## Errors

VPA reports only lexer/hook failures and their source spans. Parse error
records, recovery policy, and message layout live in [ll1](ll1.md) and
[vm](vm.md).

## Tests

- Nested scopes, leader inlining, and a scope call from inside a body.
- Empty-action branches, ignored tokens, and EOF actions.
- Valid and invalid `%effect` returns.
- Feed trace `(scope, term)` for scopes with and without parsers; no token-tree
  assertion.
