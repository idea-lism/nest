# Post-process AST and validate it

Create `src/post_process.c`.

Passes run in the order listed; each returns `false` on the first error.

### vpa: inline macros

`bool pp_inline_macros(ParseState* ps);`

1. Inline macro VPA rules. Macros may reference each other: topologically sort by dependency, report an error on cycles, then expand in topological order.
2. A sub-scope call to a macro is an error.
3. Replace the scope list with the non-macro scopes.

### ll: auto tag branches

`bool pp_auto_tag_branches(ParseState* ps);`

- Auto-tag an untagged branch by its first token or sub-rule name.
- Tags within one rule must be distinct; report conflicts.
- Keyword literals cannot induce a tag, so such branches need an explicit tag.

```conf
foo = a [
  @foo bar      # auto tag "foo" from the first token
  sub_rule      # auto tag "sub_rule" from the first sub-rule
  @foo : tag2   # "foo" is taken, so tag explicitly
  "a-keyword" : tag3
  : tag4        # epsilon branch needs a tag
]
```

Multiple alternatives in sequence are tagged branch by branch:

```conf
# gives b1..b4
foo = a [ b1 b2 ] [ b3 b4 ]
```

Tags become C identifiers in the generated header, so they must be valid identifiers and not C keywords.

### ll: FIRST/FOLLOW conflicts

`bool pp_check_ll1_conflicts(ParseState* ps);`

For each rule, compute nullable, FIRST and FOLLOW.

- Two branches conflict when their FIRST sets overlap, or when a nullable branch's FOLLOW overlaps another branch's FIRST. Report the token and rule.
- Left recursion is reported as a conflict; no separate left-recursion pass.
- For interlace `lhs*<rhs>` / `lhs+<rhs>`, report an error when both sides are nullable.
- An undefined rule call is an error.
- Report rules unreachable from `main`.
- Skip `TODO` rules.
- Do not expand a call across a scope boundary: the scope token is the terminal.

### vpa scope validity

`bool pp_validate_vpa_scopes(ParseState* ps);`

- A `main` scope must exist, and must contain an `EOF` line.
- No repeated scope names.
- Every scope except `main` must have a `.begin` (or a user hook producing the `.begin` effect).
- Every scope must have one or more `.end` (or a user hook producing the `.end` effect).
- A leading `re` / `re_str` / `re_frag_id` before the scope bracket must match at least one character.
- Inside `{ ... }`, after macro expansion, at most one `re` / `re_str` / `re_frag_id` may be empty.
  - An empty branch is a fallback action; the following action must be `.end` or `.fail`.
- Report scopes unreachable from `main`.

### ll rule validity

`bool pp_validate_ll_rules(ParseState* ps);`

- `main` must exist.
- No repeated rule names.
- A rule `= TODO` must match a scope.

### vpa & ll

`bool pp_match_scopes(ParseState* ps);`

- If a VPA scope has a same-name LL(1) rule, set `VpaScope.has_parser = true`.
- For the `main` scope, `has_parser` must be true.
- For every VPA scope whose same-name LL(1) rule is not `TODO`, `used_set` must equal `emit_set` of that VPA scope.
  - `emit_set` contains tokens and scopes. Expand scopes that have no mapped LL(1) rule.
    - Exclude ignored tokens.
    - Include tokens emitted after the scope's `.begin`.
    - Tokens emitted after `.end` belong to the caller's set.
  - `used_set` contains tokens and scopes. Expand child rules that have no mapped VPA scope.
  - Example:
    1. VPA `foo = /.../ { @a ... }`, `bar = /.../ { @b ... foo ... }`: `bar.emit_set = { @b, foo }`.
    2. LL(1) `foo = @a`, `baz = @c?`, `bar = foo @b baz`, with `baz` not a scope: `bar.used_set = { @b, @c, foo }`.
    3. The sets differ, so report that the VPA and LL(1) definitions do not agree.

### Acceptance criteria

- Macro inlining: recursion, and cascaded expansion (reorder macro definitions to exercise the topological sort).
- LL(1) conflicts: overlapping FIRST, nullable branch vs FOLLOW, left recursion, nullable interlace.
- Emit set: do not expand scopes that have a mapped LL(1) rule; expansion must terminate.
