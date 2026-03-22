Generate AGENTS.md, so opencode won't need to explore the whole project to get in context in a new session.

The AGENTS.md must highlight the following info:

### Brief

1. A regex-to-LLVM-IR compiler. 
   - Regex patterns → NFA → DFA (minimized) → LLVM IR text → native code via clang.
2. A optimized PEG parser.
   - Graph-coloring optimized parsing.

### Develop

```
ruby config.rb debug
ninja
```

Run tests / benchmarks

```
scripts/test # in release mode
scripts/test debug
scripts/benchmark
```

### Code Style

- types use camel case
- vars & functions use snake case
- use stdint. for example, `int32_t` instead of `int`
- static (private) functions should start with `_`, names be simple as possible (without module prefix)

clang-format

- line width: `120`
- if style: `if (xxx) {\n ... \n} else {...`
- pointer arg style: `foo* foo`
- enforce brace on block bodies
- on macOS, use `xcrun clang-format` to format
- others use default (indent = 2 space)
