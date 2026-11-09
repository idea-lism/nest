# Refactor plan: remove → re-spec → re-implement

Read [status.md](status.md) for the pre-refactor audit. Specs are user area: propose spec changes, don't rewrite unilaterally.

## Goals

1. **LL(1) parser** replaces the PEG backend. No packrat memoization, no coloring, no token-array materialization for parsing.
2. **Bytecode VM** replaces LLVM IR emission + clang. `nest` output = bytecode + C header; interpreter lives in the runtime lib. No clang anywhere, including nest's own bootstrap.
3. **Specs slimmed** to key ideas, data structures, interfaces.
4. **Syntax tweaks** so `.nest` grammars fit LL(1).

## Governing spec rules

Spec: key idea; key data structures & interfaces (a code sketch when it clarifies). Don't spec: anything inferable from the requirement; constraints nobody asked for; noise with no implementation effect. No C pseudocode of whole loops; no enum tables that duplicate headers 1:1; no transformation case-law (that belongs in code comments).

## Road

Each subsystem goes through the same three steps, never skipping:

1. **Remove** — delete the old code, spec, docs and tests. Don't port; old code is reference only.
2. **Update spec** — rewrite the spec to the target design, slim per the rules above. Decisions the spec can't settle go to the user, not into code.
3. **Re-implement** — build against the spec; tests assert spec invariants, not emitted text.

## Current state (verified)

- **Removed already:** PEG backend, LLVM IR backend, token tree, `nest` CLI, coloring, memtab, header_writer, dead specs.
- **Built and green:** `ustr`, `darray`, `bitset`, `symtab`, `xmalloc`, `re`, `re_ir`, `aut`, plus their tests. `ninja` succeeds.
- **Dormant, kept as reference:** `src/g4.c`, `src/g4.nest`, `benchmark/`, `examples/`, `test/examples/`.
- **Stale:** `specs/cli.md`, `specs/example_test.md`, `specs/g4.md`, `specs/vpa.md` (IR/codegen halves), `doc/nest_syntax.md`, `doc/lex_tricks.md`. Old binaries in `build/*/` still get picked up by `scripts/test` and abort.
- **Missing:** `ll1.md`, `vm.md`, runtime/VM source, generator source, CLI.

## Target architecture (end state)

```
.nest src → parse_nest (LL(1), self-hosted eventually) → ParseState
         → post_process (macro inline, VPA validation)
         → LL(1) analysis (FIRST/FOLLOW, conflict errors)
         → vpa_gen: per-scope DFA (NFA→DFA→minimize) → bytecode/tables
         → codegen: bytecode (lexer tables + LL(1) parse program) + C header
         → runtime: libnest_rt interpreter (token streaming, parse, errors)
```

Survive: ustr/darray/bitset/symtab/xmalloc, re + re_ir, aut minus codegen, vpa core, parse + post_process cores, g4 (retargeted).

## Phases

State legend: `[ ] todo · [~] in progress · [x] done · [!] blocked (design decision needed)`. Update this table and append one-line notes under the phase as you work.

### Stage 1 — Remove

#### P0 — Clean the tree `[ ]`
- Delete stale build output (`rm -rf build`) so `scripts/test` stops running removed binaries; stale `.ll` / generated headers go with it.
- Delete stale specs: `cli.md`, `example_test.md`, `g4.md` (18K of old case-law; rewrite from design), `vpa.md` generator/IR sections (rewrite from types).
- Delete stale docs: `doc/nest_syntax.md`, `doc/lex_tricks.md` — they describe the PEG/clang world. Keep copies under `notes/` if useful as reference.
- Delete `scripts/kotlin_*.rb` helpers; they predate the retarget.
- Keep `src/g4.c` + `g4.nest`, `examples/`, `test/examples/`, `benchmark/` as dormant fixtures for later phases. Do not build or repair them.
- Keep `.tmp/reports/parser_stack.md` and `codegen_stack.md` until their phases land.

Accept: no live spec/doc mentions `irwriter`, `peg_*`, `clang`, `token_tree`, `memtab`, `coloring`; `ruby config.rb debug && ninja && scripts/test debug` green from a clean tree.

#### P1 — Verify survivor specs `[ ]`
Mechanical, no design: check each survivor still matches its implementation and fits the governing rules. Fix only drift, no behavior change.
- utils: `ustr.md`, `darray.md`, `bitset.md`, `symtab.md`, `xmalloc.md`
- regex: `re.md`, `re_ir.md`, `aut.md`
- front half: `parse.md`, `post_process.md` (drop PEG-only checks if any remain)

Accept: every built module has a spec that names its invariants; impl unchanged.

### Stage 2 — Update specs

#### P2 — New backend specs `[!]` (needs design decisions, see below)
Write, from the target design, not from old code:
- `vpa.md` — rewrite: VPA types, scope/action table, leader inlining; generator half only as the interface `vpa_gen` consumes.
- `vm.md` — bytecode format, lexer table format, runtime interface, error reporting.
- `ll1.md` — FIRST/FOLLOW analysis, conflict = compile error, parse program / token streaming, recovery.
- `cli.md` — subcommands, outputs, no clang/CC flags, no memoize modes.
- `example_test.md` — output contract on semantics (token stream, tree/errors), formatting loose.
- `g4.md` — slim to the mapping key ideas to the LL(1) form.
- `doc/nest_syntax.md` — rewritten for the LL(1) syntax.

Blocked until the user settles: bytecode/table encoding, LL(1) token streaming model, error/recovery contract, syntax tweaks, g4 emission target. Record answers in the spec itself; do not guess.

Accept: spec set complete; a reader can implement the backend from specs alone; no references to removed machinery.

### Stage 3 — Re-implement

#### P3 — Lexer codegen + VM `[!]` (blocked: P2 `vm.md`, `vpa.md`)
`vpa_gen`: scoped regex → NFA → DFA → minimize → tables; runtime VM executes lexer bytecode. Implement `nest l`.

Accept: `nest l` on calc/json emits tables + header; the runtime stepper produces the expected token stream; differential test vs a reference matcher.

#### P4 — LL(1) parser `[!]` (blocked: P2 `ll1.md`)
FIRST/FOLLOW with conflict errors carrying rule/line; parse program emitted as bytecode; runtime parses the token stream. Implement `nest c`.

Accept: calc + clike + json parse via VM; conflict grammars fail with rule/line; brute-force FIRST/FOLLOW cross-check.

#### P5 — Bootstrap `[ ]`
Rebuild nest itself with its own output: no clang, `nest_lex` generated at build time, then the full grammar. Sync `specs/bootstrap.nest` ↔ the implemented grammar.

Accept: nest builds from source with only cc; bootstrap grammar parses itself.

#### P6 — g4 retarget + kotlin `[!]` (blocked: P4, g4 emission target)
Emission half of `g4.c` targets the LL(1) form; regenerate the kotlin grammar. Compile and run it, or consciously drop it.

Accept: kotlin `.g4` → `.nest` → VM in bounded time.

#### P7 — Fixtures, tests, benchmarks `[ ]`
Restore `examples/`, `test/examples/`, `benchmark/` against the new pipeline. Test bar from status.md "Lessons for the new backend": property/differential tests on spec invariants; execute generated code; no text-grep, presence-only, new/del tests. Fix stale link lines in the example/bench runners; decide the comparison set.

Accept: `scripts/test`, `scripts/test_examples`, `scripts/benchmark` green in debug and release, ASAN clean, coverage reviewed.

#### P8 — Close-out `[ ]`
Regenerate AGENTS.md (fix drift noted in status.md), rewrite README (drop GVN/clang story), final spec review pass, close status.md.

Accept: docs describe only the shipped pipeline; status.md closed.

## Working method

- Read status.md → plan.md → the phase's specs before editing. Update the phase state and notes as you go.
- Hit an unspecified behavior? Ask the user; specs are user area. Don't infer design from deleted code.
- Big investigations: spawn a sub-agent (`pi -p --no-session "<read-only task>"`) writing a report under `.tmp/reports/`, then build on it.
- Tests for surviving code (ustr/re/aut/...) are the template for the new backend: boundary tables, error messages + position, property checks.
