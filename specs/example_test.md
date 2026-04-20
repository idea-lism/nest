## Example Tests

End-to-end tests for `nest c` generated parsers.
Each test compiles a `.nest` grammar, feeds inputs, and checks output against expected files.

### Directory layout

```
test/examples/
  <name>/
    grammar.nest          # the grammar under test
    <case>.input          # one input file per test case
    <case>.expected       # expected combined output for that input
```

One grammar can have many `<case>` pairs.
`<case>` is any stem — `basic`, `empty`, `error`, etc.

### Memoize modes

Each grammar is compiled and tested under both `naive` and `shared` memoize modes.
Both modes must produce identical output.

### Expected file format

```
<errors lines>
------
<token list>
------
<parse tree>
```

Three sections separated by a line containing exactly `------`.

1. **errors** — tokenizer / syntax / post_process error output (may be empty, separator still required).
2. **Token list** — one token per line, indented by VPA scope depth.
   Each line: `<indent><token_name> "<matched text>"`.
3. **Parse tree** — tree defined by PEG rules, indented by nesting depth.
   Interior nodes are rule names; leaves are token names (no matched text).

#### Multipliers and alternation in the parse tree

- `*` / `+`: each match of the inner rule appears as a direct child of the parent — no synthetic wrapper node.
- `?`: the child is present or absent — no wrapper.
- `[...]` alternation: only the matched branch appears.

Example (`main = stmt*`, `stmt = [if_stmt, assign_stmt]`):

```
------
@kw_if "if"
@lparen "("
@ident "x"
@rparen ")"
  @lbrace "{"
  @ident "y"
  @assign "="
  @number "1"
  @semi ";"
  @rbrace "}"
------
main
  stmt
    if_stmt
      @kw_if
      @lparen
      expr
        @ident
      @rparen
      block
        @lbrace
        stmt
          assign_stmt
            @ident
            @assign
            expr
              @number
            @semi
        @rbrace
```

### Generated example

`nest c` produces `<prefix>.c` which must print both sections:
the scope-indented token list, then `------`, then the indented parse tree.
The `------` separator between errors and the token list is also printed, so the combined captured output matches the expected format directly.

### Test runner

`scripts/test_examples` iterates every directory under `test/examples/`.

For each directory, for each memoize mode (`naive`, `shared`):

1. Run `nest c grammar.nest -p <name> -m <mode>` (prefix = directory name).
2. Compile `<name>.c` + `<name>.ll` with clang.
3. For each `*.input` file, run the binary with the file name as `argv[1]`.
4. Diff combined output against the matching `*.expected` file.
5. Report pass/fail per case; exit non-zero if anything failed.

Temp build artifacts go in a scratch directory and are cleaned up.

### Invocation

```sh
scripts/test_examples          # uses build/debug/nest
scripts/test_examples release  # uses build/release/nest
```

On failure, print directory name, memoize mode, case name, and a unified diff.
