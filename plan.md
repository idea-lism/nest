# Refactor plan: PEG→LL(1), LLVM→bytecode VM, spec slimming

Read [status.md](status.md) first. Open design decisions live in [questions.md](questions.md) — phases marked **(blocked)** wait for those answers; do not guess them.

## Goals

1. **LL(1) parser** replaces the PEG backend. No packrat memoization, no coloring, no token-array materialization for parsing.
2. **Bytecode VM** replaces LLVM IR emission + clang. `nest` output = bytecode + C header; runtime interpreter lives in the runtime lib. No clang dependency anywhere (including nest's own bootstrap).
3. **Specs slimmed** to key ideas, data structures, interfaces.
4. **Syntax tweaks** so `.nest` grammars fit LL(1).

## Governing spec rules (apply to every spec rewrite)

Spec: key idea; key data structures & interfaces (a code sketch when it clarifies). Don't spec: anything inferable from the requirement; constraints nobody asked for; noise with no implementation effect. No C pseudocode of whole loops; no enum tables that duplicate headers 1:1; no transformation case-law (that belongs in code comments).

## Target architecture (end state)

```
.nest src → parse_nest (bootstrap, LL(1) itself once retargeted) → ParseState
         → post_process (macro inline, VPA validation, LL(1) FIRST/FOLLOW conflict check)
         → LL(1) analysis (FIRST/FOLLOW, conflict errors)
         → vpa_gen: per-scope DFA (NFA→DFA→minimize) → transition tables
         → codegen: bytecode (lexer tables + LL(1) parse program) + C header
         → runtime: libnest_rt interpreter (token streaming, parse, errors)
```

Gone: peg_analyze/peg_alloc/peg_gen/peg_ir, coloring, graph, memtab, memoize machinery in token_tree, irwriter*, llir_parse_gen, aut_gen_dfa, clang subprocesses, `-m` memoize modes, GVN workaround, `.ll` outputs.
Survive: ustr/darray/bitset/symtab/xmalloc/header_writer, re + re_ir, aut minus codegen, vpa core, parse + post_process cores, g4 (retargeted), token_tree chunk basics (pending questions on streaming), `nest l`.

## Target spec set

| new / rewritten spec | from | fate of old |
|---|---|---|
| `parse.md` (slim) | parse.md + parse_gen.md merged | drop PEG-section parsing, pseudocode; keep numbering + lex-loop key idea |
| `ll1.md` (new) | — | replaces peg_analyze/peg_alloc/peg_gen/peg_ir (all deleted) |
| `vm.md` (new, blocked) | — | replaces irwriter.md, irwriter_gen_rt.md, aut.md's codegen section |
| `aut.md` (slim) | aut.md | keep NFA/DFA/minimize, MIN/PRESERVING action rules; honest about naive refinement; drop aut_gen_dfa |
| `re.md`, `re_ir.md` (slim) | same | re.md gains a ReLex paragraph (two entry paths); re_ir survives near-verbatim |
| `vpa.md` (slim) | same | keep types, action table, leader-inlining; trim hook/example sections |
| `token_tree.md` (rewrite, blocked on Q2/Q3) | same | keep chunk/newline/sentinel; delete memoize + best-try halves |
| `post_process.md` (slim) | same | keep macro inline + VPA validity; PEG checks → LL(1) conflict check |
| `cli.md` (rewrite, blocked) | same | bytecode outputs, no clang/CC flags, no memoize modes |
| `example_test.md` (update contract) | same | output-format contract is golden; runner steps change |
| `g4.md` (slim, blocked on Q8) | same | keep mapping key ideas; drop case-law |
| utils specs (symtab/bitset/darray/ustr/memtab-delete/xmalloc/header_writer) | same | keep as-is; xmalloc drops LLVM section; memtab.md deleted |
| deleted outright | | peg_analyze/peg_alloc/peg_gen/peg_ir/peg_err_report/coloring/memtab/irwriter/irwriter_gen_rt/syntax_harness |
| user docs | doc/nest_syntax.md rewritten for LL(1) syntax (it is user-visible: `nest h` prints it); doc/lex_tricks.md small fix |

## Phases

State legend: `[ ] todo · [~] in progress · [x] done · [!] blocked (see questions.md)`. Agents: update this table and add one-line notes under phase notes as you work.

### P0 — Triage the red build `[ ]`
HEAD doesn't build (see status.md Health). Don't repair doomed code: `test_peg_ir` dies in P2 anyway — delete it now if it blocks the build; `test_vpa` fix is cheap (5-arg call) but vpa_gen's header emission changes in P3 — fix minimally or trim to compiling subset. g4 `-Werror` unused-param: fix the emitter or drop g4 from default build until P7.
Accept: `ruby config.rb debug && ninja` green, `scripts/test debug` runs.

### P1 — Slim survivor specs `[ ]`
Mechanical, no design needed: utils specs, re/re_ir, aut (drop codegen section), vpa, parse+parse_gen merge, post_process (VPA half). Delete dead specs (peg_*, coloring, memtab, irwriter*, syntax_harness, empty peg_err_report). Keep implementations untouched this phase.
Accept: each rewritten spec fits the governing rules; impl still matches (no behavior change).

### P2 — Excise PEG backend `[ ]`
Delete peg_analyze/peg_alloc/peg_gen/peg_ir, coloring, graph, memtab; strip memoize/best-try from token_tree; drop PEG passes from post_process; drop `-m`/`-s` from CLI. `nest c` temporarily emits lexer-only output (or a stub). Rewrite/trim the ~½ test corpus that rides on PEG (delete, don't port).
Accept: build green; `nest l` unaffected; `nest c` produces valid lexer header; PEG tests gone from build.

### P3 — Excise LLVM emission `[ ]`
Delete irwriter*, llir_parse_gen, aut_gen_dfa, clang pipes, DebugInfo threading (signatures slim). `nest c`/`nest l` emit header + serialized DFA transition tables (interim format — VM may redefine). Re-bootstrap nest itself: `nest_lex` becomes tables generated at build time (no clang); g4_grammar deferred to P7.
Accept: no clang invocation anywhere; nest builds from source with only cc; `nest l` output is a table + header that a C stepper in the runtime can run (interim).

### P4 — Bytecode VM `[!]` (blocked: Q1, Q12)
Design + spec `vm.md` (user will provide detailed design), implement interpreter in libnest_rt, codegen emits bytecode for lexer (+ parser once P5 lands). Bootstrap regenerated on VM.
Accept: `nest c` end-to-end on calc via VM; perf sanity vs old numbers on calc/json; kotlin compiles+runs in sane time.

### P5 — LL(1) parser `[!]` (blocked: Q2, Q3, Q4, Q5, Q9)
Spec `ll1.md`: FIRST/FOLLOW analysis (seed: nullable/first/last fixpoint already exists in peg_alloc.c:248), conflict = compile error, parse strategy, error reporting, token streaming model.
Accept: calc + clike + json examples parse via VM; conflict errors reported with rule/line.

### P6 — Syntax tweaks + docs + bootstrap sync `[!]` (blocked: Q4–Q7, Q11)
Apply decided syntax changes; rewrite doc/nest_syntax.md; sync bootstrap.nest ↔ parse_gen.c (or simplify per Q11); update all example grammars.
Accept: `nest h`/`nest r` show new syntax; examples pass.

### P7 — g4 retarget + kotlin `[!]` (blocked: Q8, P5)
Emission half of g4.c targets LL(1) form; regenerate kotlin grammar; it must compile and pass (or be consciously dropped per Q8).
Accept: kotlin .g4 → .nest → VM in bounded time.

### P8 — Tests, examples, benchmarks `[ ]` (follows each phase, final sweep here)
Test bar for all new code: status.md "Lessons for the new backend" (property/differential tests on spec invariants; no text-grep or presence-only tests).
Rebuild example_test contract, benchmark harness (fix stale link lines; decide comparison set per Q12), full `scripts/test` + `scripts/test_examples` + coverage.
Accept: all green in debug and release.

### P9 — Close-out `[ ]`
Regenerate AGENTS.md (fix all drift noted in status.md), README (drop GVN/clang story), delete vendored kissat, final spec review pass, close status.md.

## Working method (for later agents)

- Read status.md → plan.md → questions.md → the phase's specs before editing. Update the phase table + notes as you go.
- Big investigations: spawn a sub-agent (`pi -p --no-session "<read-only task>"`) writing a report under `.tmp/reports/`, then build on it — same method that produced this plan.
- Old sub-reports for this refactor: `.tmp/reports/parser_stack.md`, `.tmp/reports/codegen_stack.md` (may be deleted once P2/P3 land).
- Specs are user area: propose spec changes in questions.md/notes, don't rewrite unilaterally — except the mechanical slimming explicitly ordered in P1.
