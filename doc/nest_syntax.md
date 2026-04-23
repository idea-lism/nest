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
```

- `name` — lowercase identifier
- `seq` — sequence of units

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
