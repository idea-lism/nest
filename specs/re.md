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
- Debug info
  - `re_append_ch` and `re_append_range` accept `DebugInfo di` (line/col in the regexp source file)
  - passed through to `aut_transition` so generated IR has DWARF locations pointing back to the regexp source
  - callers that don't need debug info pass `(DebugInfo){0, 0}` (line 0 means no debug location)

Regexp don't handle EOF or other boundaries, because they are zero width (can stack multiple times but still consumes 0 input). Other DFA-based libraries don't handle this either.

# Ignore-case APIs

- `re_range_ic(range)` make the range ignore case (if the range is negative, revert first)
- `re_append_ch_ic(re, int32_t codepoint, DebugInfo di)`

# Predefined char groups

- `re_append_group_s(re)`
- `re_append_group_d(re)`
- `re_append_group_w(re)`
- `re_append_group_dot(re)`

# Predefined escape

- `re_append_c_escape(re, char symbol)` - `\\[bfnrtv]`

# Hex

- `re_append_hex(re, char* h, size_t size)`
