Regexp building helpers

Some automatic state management.

On top of aut.

- Creating and destructing builder: `re_new(aut)`, `re_del()`.
- Range management
  - `re_range_new()`, `re_range_del()`
  - `re_range_add(range, start_cp, end_cp)` -- sorted and merged
    - min codepoint = 0, max codepoint = 0x10FFFF
  - in-place negate a range  `re_range_neg(range)`
- Regexp group management
  - stack top is 0 by default
  - `re_lparen(re)` push a stack (left paren)
  - `re_fork(re)` start a new branch, forked at stack top
  - `re_append_ch(re, int32_t codepoint, DebugInfo di)`: can have negative codepoints for special purpose
  - `re_append_range(re, range, DebugInfo di)`
  - `re_rparen(re)` pop a stack state
  - `re_action(re, action_id)` make it emit action at current state
- Define special codepoint (matches spec in aut.md)
  - `LEX_CP_BOF`: -1
  - `LEX_CP_EOF`: -2
- Debug info
  - `re_append_ch` and `re_append_range` accept `DebugInfo di` (line/col in the regexp source file)
  - passed through to `aut_transition` so generated IR has DWARF locations pointing back to the regexp source
  - callers that don't need debug info pass `(DebugInfo){0, 0}` (line 0 means no debug location)
