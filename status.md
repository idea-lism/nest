# nest — code quality status (pre-refactor)

Static audit against the project's own bar (`specs/review.md`). HEAD `388e9e8`, which doesn't build: `test_vpa.c` and `test_peg_ir.c` are stale against the last refactor, and the generated `g4_grammar.h` fails `-Werror`.

## Verdict

Utility and regex-range layers are solid. The test suite is wide but shallow. Most suites check that output *looks* right (symbol exists, IR text contains a string) and not that the *constraints* hold. The spec's core invariants are each shown on one or two hand-picked examples and are never exercised as properties.

## Tests: real constraints vs happy path

**Real (keep as templates):**
- `test_ustr`: invalid UTF-8 at every boundary (overlong, surrogate, truncated 2/3/4-byte, >0x10FFFF, F5/FE/FF).
- `test_re` ranges: interval edges (`0`, `0x10FFFF`, negation at start/end, adjacent/overlap/subset merges).
- `test_post_process`: error cases assert on the failure *and* its message (e.g. duplicate tag → error mentions `dup`).
- `test_re_ir` validate: missing, recursive and mutually recursive fragments.

**Happy-path / presence-only:**
- `test_parse_gen`: the `test_has_lex_*` tests only assert that a symbol exists. The `*_recognizes_*` tests assert `action != -2`, so any live transition passes and none checks which token is produced.
- `test_peg_gen`, `test_peg_ir`, `test_irwriter`, `test_vpa`: assert through `strstr` on emitted IR or header text. This pins the shape of the text, not the semantics. It's brittle (any renaming breaks it) and blind (wrong code with the right substrings passes).
- `test_aut`: MIN-RULE, PRESERVING-RULE and "optimize preserves the language" are each checked on one tiny hand-built automaton. Nothing compares the NFA and the minimized DFA on generated inputs, though that is the one property the module exists for. `test_optimize_reduces_states` only asserts that the state count went down, not that it reached the minimum.
- Fake by review.md's definition (an empty impl would pass): the `test_lifecycle` / `test_new_del` / `test_init_free` style tests (`x = new(); assert(x); del(x)`).
- `test_coloring`: an ad-hoc `main` with random graphs and lower-bound asserts (`>= 3`, `>= 2`). It doesn't check that the coloring is valid.
- `test_peg_gen` has a tail of `*_regression` tests. Each one patches a specific crash (out-of-bounds read, alternation crash) and none states the invariant that was broken, which is a sign the memoize layout invariants were never pinned down.

**Example tests (`test/examples`):** these are golden dumps of the token stream plus one error line. The error cases are good: position, scope path and message are all pinned. But each grammar has only one or two of them. Parse *structure* is barely asserted, mostly just which tokens were consumed. The output format is incidental, yet it's pinned byte-exact, so it will resist the refactor for no semantic reason.

## Code

- **Lazy hidden state in `aut`:** every getter repeats `if (!a->optimized) _simple_determinize(a)` (`aut.c:520,527`). Mode is implicit, so callers can't tell which automaton they are reading.
- **NULL-tolerant darray:** every function accepts NULL (`darray.c:32-174`). `darray_size(NULL) = 0` is a deliberate convenience. But `darray_grow2(NULL)` returning NULL silently swallows a misuse, which goes against fail-fast.
- **Dead parameters:** `coloring_solve` ignores 4 params (`coloring.c:118-121`) and the CLI `-s` still feeds one of them. `peg_gen.c:470-579` and `peg_ir.c:272` contain `(void)` casts: the signatures were widened "for later" and never used (a YAGNI violation).
- **Spec honesty:** `aut.md` says "Hopcroft", but the code does subset construction plus naive refinement. The type definitions in `vpa.md` have drifted from `vpa.h`. `nest_rt.h` hand-duplicates `token_tree.h`, and the two already diverge.
- **Duplicated mechanisms:** there are two regex parsers (ReLex and ReIr) with different feature sets (icase and binary only in ReLex). `bootstrap.nest` and `parse_gen.c` are kept in sync by hand.
- **Type punning:** `TokenChunk.value` holds a string buffer in the bootstrap parser but an alias of `memtab->t` in generated parsers.
- **Not deeply audited:** the internals of `parse.c` and `g4.c`, the two largest files. Both are heavy on guard clauses. Whether those are defensive or legitimate input validation is still unverified.

## Lessons for the new backend

- **Carry forward:** boundary tables like the ustr/range tests; error tests that assert the message and position; executing generated code instead of grepping it.
- **Add:** differential/property tests for the invariants the specs name: NFA ≡ min-DFA on random strings; MIN/PRESERVING under optimize; LL(1) conflict detection against a brute-force FIRST/FOLLOW; VM result against a reference interpreter.
- **Ban:** asserting on emitted text; presence-only tests; new/del tests; unused "future" params; implicit lazy modes; regression tests that don't name the invariant.
- **Examples:** pin semantics (tree shape, error position/expected set) and keep the formatting loose.
