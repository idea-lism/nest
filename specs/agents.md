Generate AGENTS.md, so opencode won't need to explore the whole project to get in context in a new session.

For the "Project Structure" part, list what each source file does in simple words, and point to corresponding specs/xxx.md

So the resulting AGENTS.md layout is like this:

`````markdown
### Architecture

1. A regex-to-LLVM-IR compiler.
   - Regex patterns → NFA → DFA (minimized) → LLVM IR text → native code via clang.
2. A optimized PEG parser.
   - Graph-coloring optimized parsing. uses kissat

IMPLEMENTATION MUST FOLLOW SPECS.

### Project Structure

src/
  aut.h: automata constructing and generating, follows specs/aut.md
  aut.c: impls aut.h
  nest.c: command line interface, follows specs/cli.md and specs/test_examples.md
  ...
test/
  ...
test/examples/: follows specs/test_examples.md
  ...
config.rb: platform-related config, generates build.ninja
config.in.rb: defined sources, targets, dependencies

### Common Tasks

```sh
ruby config.rb debug
ninja
ninja format
```

Run tests / benchmarks:
```sh
scripts/test # in release mode
scripts/test debug
scripts/benchmark
scripts/test_examples
```

Adding new source files:
- add the entry in config.in.rb
- add related test

### Utils to Avoid Boilerplates

Ustr
```c
// typical use, ustr_from_file, ustr_iter ... of ustr.h
```

Darray
```c
// typical use of darray.h
```

IrWriter
```c
// typical use of irwriter.h
```

HeaderWriter
```c
// typical use of header_writer.h, especially the begin / end indentation management
```

TokenTree
```c
// typical use of token_tree.h
```

### Code Style

- types use camel case
- vars & functions use snake case
- use stdint. for example, `int32_t` instead of `int`
- static (private) functions should start with `_`, names be simple as possible (without module prefix)

### Naming

Naming is important for readable code, should clearly represents what it does, and honest to spec, no re-inventing new variable/function names when specs has already named them.

Bad code: `n_tags`, `n_tokens`

Good code: `tag_size`, `token_bytesize`, `tag_size_in_i32`, `tag_size_in_i64`
`````
