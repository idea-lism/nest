### Architecture

Input driven parser.

1. A Vpa compiler.
   - Regex patterns → NFA → DFA (minimized) → bytecode
2. A LL(1) parser.
   - Matching bytecode

IMPLEMENTATION MUST FOLLOW SPECS.

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

Read ustr.h, darray.h

### Code Style

- types use camel case
- vars & functions use snake case
- use stdint. for example, `int32_t` instead of `int`
- static (private) functions should start with `_`, names be simple as possible (without module prefix)

### Naming

Naming is important for readable code, should clearly represents what it does, and honest to spec, no re-inventing new variable/function names when specs has already named them.

Bad code: `n_tags`, `n_tokens`

Good code: `tag_size`, `token_bytesize`, `tag_size_in_i32`, `tag_size_in_i64`

### Precision

Implementation must match spec precisely. If the spec is wrong, point out, ask, don't get around it or changing it randomly.
