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
| `%ignore @tok ...` | Tokens kept for source reconstruction but excluded from AST |
| `%define ID /regex/` | Define a named regex fragment (ID must start with uppercase) |
| `%effect .hook = @tok \| .hook ...` | Define a compound effect |

### Rules

```
name = /regex/ @token_id scope?
name = "literal" @token_id scope?
name = FragName @token_id scope?
```

- `name` — lowercase identifier (`[a-z_]\w*`)
- `EOF` - special identifier, matches end of file
- `@token_id` — token tag emitted on match
- Scope `{ ... }` — nested lexer context (pushdown)

### Module rules

```
*module_name = { ... }      # module scope (reusable group of rules)
*module_name = @{ ... }     # literal scope (each line is a string literal token)
```

Module names are prefixed with `*`.

### Actions

Each rule can have zero or more actions appended:

| Action | Meaning |
|--------|---------|
| `@token_id` | Emit a token |
| `.begin` | Push scope |
| `.end` | Pop scope |
| `.fail` | Signal parse failure |
| `.unparse` | Unparse (put back) |
| `.user_hook` | User-defined hook (`.` prefix, lowercase) |

### Literal scope

`@{ ... }` contains one string literal per line — each becomes a keyword token.

```
*directive_keywords = @{
  "%ignore"
  "%effect"
  "%define"
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
| `[^abc]` | Negated character class |
| `[a-z]` | Character range |
| `#{FragName}` | Reference to `%define`d fragment, can't reference EOF |

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

## String literals

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

### Branches (ordered choice)

```
rule = [
  seq1 :tag1
  seq2 :tag2
  seq3
]
```

Each line is an alternative. Optional `:tag` labels the branch for AST construction.

### Example

```
expr = term+<"+">
term = factor+<"*">
factor = [
  @number
  "(" expr ")"
]
```
