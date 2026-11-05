# Refactor — open design questions

High-level only; detailed designs come later from the user. Numbered for reference from plan.md.

## VM / codegen

**Q1. VM scope.** One bytecode VM for everything (lexer DFAs + parser), or lexer DFAs stay *data* (transition tables stepped by a generic C loop — already what `DfaTrans[]` is) and bytecode is only invented for the parse program? The second is much less design; the first is more uniform.

**Q2. What does the VM execute, exactly?** Today's PEG functions are arbitrary control flow (calls, backtracking, indirectbr). An LL(1) parse is a deterministic walk — it may not need a VM at all: a table-driven recursive-descent in C (generated tables, hand-written stepper, like the lexer) might cover it. Is bytecode strictly required for the parser, or only "no LLVM" is required?

**Q12. Performance target.** Benchmarks currently compare against packcc/tree-sitter (generated C, compiled). An interpreted VM will be slower than compiled parsers on big inputs. What's the bar: keep the comparison benchmark, and if so what's an acceptable throughput/RSS position vs packcc?

## Parser / AST / tokens

**Q3. Token storage.** "No token array" — fully streaming (tokens consumed on demand, nothing materialized), or keep chunked storage during lexing for what still needs it (VPA scope nesting, `%ignore` tokens kept for source reconstruction, line/col via newline_map) and only drop the parse-side materialization + memoize tables? Note `%ignore`'s "kept for reconstruction" contract implies *some* retained positions.

**Q4. Parse result API.** Today: zero-alloc decode of memoize columns via `PegRef`/`PegLink` + `Node_*` structs + loaders. With LL(1) streaming, does the parser build AST nodes during the walk (allocations at parse time), or emit ranges (rule → token span) that the user decodes lazily? This decides the whole user-facing header.

**Q9. Error reporting.** PEG best-try (farthest-success heuristic over memoize tables) is gone. LL(1) gives precise "expected one of {FIRST set}, got X" at the conflict point. Is that the desired contract (plus scope path), or is best-try-style recovery wanted too?

**Q10. Scope-level parsers.** Today each `has_parser` VPA scope gets its own parse function invoked per chunk. In a streaming design, does a scope switch become a parser-mode switch on synthetic enter/exit tokens, or do scope parsers stay separate entry points?

## Syntax

**Q5. Branch tags (`:tag`).** They exist to discriminate alternatives in the memoize bit-pattern world. With a real AST they look redundant (alternative = node kind). Drop, or keep as user-chosen node-kind names?

**Q6. Lookaheads (`&e !e`).** Pure LL(1) has no syntactic predicates — conflicts are hard errors. Drop them entirely (conflict errors instead), or keep as a bounded escape hatch that silently makes the grammar non-LL(1) at that point?

**Q7. Multipliers and interlace.** Keep `? * +` and `+<sep>` sugar (expanding to `e (sep e)*`) with LL(1) restrictions (non-nullable element / FOLLOW-disjointness checked), or cut any of them?

**Q8. `TODO` stubs and g4 strictness.** `TODO` scope stubs were tied to the chunk model — keep an incremental-grammar story at all? Related: ANTLR grammars (kotlin) are rarely strict LL(1); the g4 importer will reject many real grammars with conflict errors. Accept strict-LL(1)-only (kotlin may need grammar hand-edits or get dropped as an example), or does the importer need an LL(k)/limited-backtracking story?

## Ecosystem / bootstrap

**Q11. Bootstrap.** Keep the current 3-level bootstrap with its hand-synced bootstrap.nest ↔ parse_gen.c dual maintenance, or use this refactor to collapse it (single grammar source of truth, nest bootstraps from tables it can emit)? Also confirm: vendored kissat dir and all clang probing get deleted outright.
