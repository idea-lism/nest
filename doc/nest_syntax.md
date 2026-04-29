# .nest Syntax Reference

A `.nest` file defines a two-phase parser: a **VPA** (visibly pushdown automaton) lexer
and a **PEG** (parsing expression grammar) parser. Sections are introduced by `[[vpa]]`
and `[[peg]]` headers.

## Comments

Lines starting with `#` are comments.

```
# this is a comment
```

## Sections

```
[[vpa]]
... lexer rules ...

[[peg]]
... parser rules ...
```

## VPA Section

### Directives

| Directive | Meaning |
|-----------|---------|
| `%ignore @tok ...` | Tokens kept for source reconstruction but excluded from PEG parsing |
| `%define ID /regex/` | Define a named regex fragment (ID must start with uppercase) |
| `%effect .hook = @tok \| .hook ...` | Define a compound effect (sequence of actions) |

### Rules

```
name = /regex/ action* scope?
name = "literal" action* scope?
name = FragName action* scope?
name = EOF action*
```

- `name` — lowercase identifier (`[a-z_]\w*`)
- `EOF` — special identifier, matches end of file
- `FragName` — reference to `%define`d fragment (starts with uppercase)
- `action*` — zero or more actions (see below)
- `scope?` — optional nested lexer context `{ ... }`

### Module rules

```
*module_name = { ... }      # module scope (reusable group of rules)
*module_name = @{ ... }     # literal scope (each line is a string literal token)
```

Module names are prefixed with `*`. Module rules are inlined into the scopes
that reference them during post-processing.

### Actions

Each rule can have zero or more actions appended:

| Action | Meaning |
|--------|---------|
| `@token_id` | Emit a token |
| `.begin` | Push scope |
| `.end` | Pop scope |
| `.fail` | Signal parse failure |
| `.unparse` | Unparse (put back matched input) |
| `.noop` | No operation (placeholder) |
| `.user_hook` | User-defined hook (`.` prefix, lowercase identifier) |

### Literal scope

`@{ ... }` contains one string literal per line — each becomes a keyword token.

```
*directive_keywords = @{
  "%ignore"
  "%effect"
  "%define"
  "="
  "|"
}
```

## Regex Syntax

Regexes appear between `/` delimiters. Optional mode prefix: `i` (case-insensitive),
`b` (binary), or combinations (`ib`, `bi`).

```
/[a-z_]\w*/       # basic regex
i/hello/          # case-insensitive
b/\x00\xff/       # binary mode
```

### Atoms

| Pattern | Meaning |
|---------|---------|
| `.` | Any character |
| `\s` | Whitespace class |
| `\w` | Word class `[a-zA-Z0-9_]` |
| `\d` | Digit class `[0-9]` |
| `\h` | Hex digit class `[0-9a-fA-F]` |
| `\n` `\t` `\r` etc. | C escape sequences |
| `\{XXXX}` | Unicode codepoint (hex) |
| `[abc]` | Character class |
| `[^abc]` | Negated character class (`^` immediately after `[`) |
| `[a-z]` | Character range |
| `#{FragName}` | Reference to `%define`d fragment |

### Quantifiers

| Quantifier | Meaning |
|------------|---------|
| `?` | Zero or one |
| `+` | One or more |
| `*` | Zero or more |

### Grouping and alternation

```
(a|b)         # alternation
(abc)?        # optional group
```

## String Literals

Delimited by `"` or `'`. Support the same escape sequences as regexes.

```
"hello"
'world'
"\{2603}"     # snowman codepoint
```

## PEG Section

### Rules

```
name = seq
name = TODO      # stub: see "Stub rules" below
```

- `name` — lowercase identifier
- `seq` — sequence of units

### Stub rules (`= TODO`)

Writing `name = TODO` marks a rule as a stub, letting you grow a grammar
incrementally: the VPA lexer is wired up end-to-end, but the PEG body is left
for later.

```
class_body = TODO
```

Rules for `TODO`:

- Only **scope** rules (PEG rules whose name matches a VPA scope) may be
  `TODO`. Stubbing a plain helper rule is rejected with an error — stubs exist
  to defer parsing an entire scope, not to skip helpers.
- Left-recursion, undefined-call, orphan, and used/emit-set checks are all
  skipped for `TODO` rules.
- The generated parser for a `TODO` scope always succeeds, consuming whatever
  token span the lexer produced for that scope. No `Node_{name}` struct or
  `{prefix}_load_{name}` function is emitted — you add them by replacing the
  `TODO` with a real body.
- The memoize column for a `TODO` scope is a minimal 8 bytes (no slots, no tag
  bits).

Typical workflow: get `[[vpa]]` accepting your input, stub every scope with
`= TODO` in `[[peg]]`, then replace the stubs one scope at a time.

### Units

| Unit | Meaning |
|------|---------|
| `name` | Reference to another PEG rule |
| `@token_id` | Match a token from the lexer |
| `"literal"` | Match a literal string token |
| `[...]` | Branches (ordered choice) |

### Multipliers

| Suffix | Meaning |
|--------|---------|
| `?` | Optional |
| `+` | One or more |
| `*` | Zero or more |
| `+<sep>` | One or more, interlaced with separator |
| `*<sep>` | Zero or more, interlaced with separator |

The separator in `+<sep>` / `*<sep>` can be a rule name, `@token_id`, or `"literal"`.

### Lookahead Predicates

| Prefix | Meaning |
|--------|---------|
| `&e` | And-predicate: succeeds iff `e` matches, consumes nothing |
| `!e` | Not-predicate: succeeds iff `e` fails, consumes nothing |

The expression `e` can be a rule name, `@token_id`, `"literal"`, or `[branches]`.
Lookaheads cannot have multipliers (`?`, `+`, `*`).

```
item = [
  &@number @number : is_num    # match number only if it looks like a number
  !@number @ident  : is_id     # match ident only if it's not a number
]

# and-predicate on a rule call
checked = &num @number

# not-predicate on a string literal
filtered = !"end" @ident

# and-predicate on branches
guarded = &[
  @number
  @ident
] value
```

Lookaheads produce no node fields in the generated parse tree — they consume
no input and exist only to guide the parse.

### Branches (ordered choice)

```
rule = [
  seq1 :tag1
  seq2 :tag2
  seq3
]
```

Each line is an alternative. Optional `:tag` labels the branch for AST node
discrimination. Tags are auto-generated from rule names when omitted in
branches with more than one alternative (see post-processing).

Branches cannot be nested directly inside other branches — use a
named rule as an intermediary.

### Example

```
[[vpa]]
%ignore @space
main = {
  /\s+/ @space
  /[0-9]+/ @number
  /[+*()]/ @op
}

[[peg]]
main = expr
expr = term+<"+">
term = factor+<"*">
factor = [
  @number
  "(" expr ")"
]
```
