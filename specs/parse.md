### Overview

Parser generator combining visibly pushdown automata (VPA) and an LL(1) parser.

- VPA is a set of minimized DFAs with a scope stack
  - it emits a nested token stream
  - it can be extended by hook predicates
- LL(1) works on the token level, not character level
  - whenever a token is created, it is fed to the LL(1) parser for that scope
  - scope isolation lets the parser recover and produce more than one error

Input is handled in two passes:

1. a manual pushdown automaton in `src/parse.c` lexes the source into a scoped token stream
2. a recursive descent parser in `src/parse.c` parses each scope's token stream

`specs/bootstrap.nest` is the full syntax definition, in its own syntax.

### The nested word `[[vpa]]`

Visibly pushdown (nested word) definition, served as lexer.

Acts as a low-level ambiguity resolver; prefer the first-declared action when possible.

The lexer stays simple: it only solves the state and macro problem.

Hooks receive the current position and parsed range in `Token tok`, and can return a token id or a primitive hook (details in [vpa](vpa.md)).

Semantics (also validated in [post process](post_process.md)):

- `main` is the entrance.
- User hooks fire during lexing for semantic actions (tracking definitions, resolving names, etc.); they affect the token stream only when combined with `%effect`.
- A scope is a union of `vpa_rules`.
  - if an LL(1) rule is defined with the same name, the scope takes up one token in the parent scope
  - otherwise the scope is flattened into the parent scope
- Greedy match: when no further transition is possible, the last minimum action wins (see MIN-RULE in [aut](aut.md)).
- When a VPA scope ends, if an `ll_rule` has the same name, invoke LL(1) parsing on the scope tokens.
  - both LL(1) & VPA: create a sub-stream and parse it
  - non-LL(1) VPA: no sub-stream
  - non-VPA LL(1): a normal LL(1) sub-rule
  - LL(1) parse failure is recoverable: record a syntax `ParseError`, close the sub-stream, emit the scope token in the parent stream, and continue VPA lexing. Lexer failures remain fatal.
- In the `re` scope, `#{UPPER_CASED_ID}` interpolates a predefined regexp fragment.

Token stream at runtime, when `scope1` has an LL(1) rule and `scope2` does not:

```
tok1 tok2 scope1 tok7 tok8
            |
         tok3 tok4 (scope2 begins) tok5 tok6 (scope2 ends)
```

### The LL(1) parser `[[ll]]`

Define an LL(1) grammar. The concrete syntax is in `bootstrap.nest`.

Semantics (also validated in [post process](post_process.md)):

- There must be exactly one entrance rule named `main`.
- Branches must be distinguishable by their first token; conflicts are compile errors.
- Besides sequence and alternation, `join/interlace` interlaces the "operator" inside angle brackets with the base multi-unit.
- Each branch may be tagged with `: the_tag`; untagged branches are auto-tagged by [post process](post_process.md).

### Scope entry and exit

The scope lexer runs the DFA for one scope.

- An `end_token` returns and pops the stream.
- A token id that names a scope is a scope call: recurse with that scope id.
- A control action id is lexer control (ignore / begin / end / unparse / fail and their composites); a token-producing id emits a token.
- At EOF, the scope's EOF action (if any) fires before the final mismatch.
- A sub-lexer error sets the lexing error and returns false; callers propagate false.
- Materializable scopes (same-name LL(1) rule) create a child stream; other scopes are flattened into the current stream.
- A completed scope invokes its LL(1) parser on the child stream.

### Parsing model

- Parsers work on the scope level: the input is a scoped token stream. A sub-scope is matched as a single token, then parsed as a separate stream.
- Non-scope rule calls are ordinary recursive-descent calls.
- Regexps are converted to [IR](re_ir.md) before VPA; `frag_ref` is resolved when VPA executes the IR.

### Token, hook, and action numbering

- Produced token ids and scope ids are non-zero and share one numbering; 0 is the sentinel.
- Builtin hook ids are fixed (see [vpa](vpa.md)); user-defined hooks are numbered after them.
- Unified `action_unit_id`: > 0 maps to a token id; < 0 maps to `-hook_id`.

### What is FORBIDDEN, a no-go

- Re-inventing regexp/lexer machine in parse.c
- Stub implementations

### Test

Should at least check these cases:

- `/[a-]/`, `/[-a]/` regexp with bad range, should report syntax error
