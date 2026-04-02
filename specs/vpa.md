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

### Usage interface for generated code

generated header defines interface to interact with the LLVM-IR defined parser.

- user must define `.` hooks for lexing

### Header generating

Resulting parser needs:

- common runtime
  - copy `build/nest_rt.inc`, which is string buf from header amalgamated from `src/nest_rt.h.in`
    - includes: input stream handling: use `ustr`
    - includes: token tree representation: use `token_tree`
- token ids
- vpa parser main function, wrapped inside `NEST_RT_IMPLEMENTATION`
