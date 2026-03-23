# re

[![CI](https://github.com/idea-lism/re/actions/workflows/ci.yml/badge.svg)](https://github.com/idea-lism/re/actions/workflows/ci.yml)

Regex-to-LLVM-IR compiler. Define patterns, generate a DFA as LLVM IR, link it into your program.

## Build

Requires Ruby, [Ninja](https://ninja-build.org/), and `unzip`. On Windows, build under [MSYS2](https://www.msys2.org/) (CLANG64).

```
ruby config.rb debug    # or: ruby config.rb release
ninja
```

This produces:

- `out/libre.a` -- static library (all modules)
- `out/re_rt.h` -- single-header runtime (ustr + bitset), use with `#define RE_RT_IMPLEMENTATION`
- `out/lex.h` -- lex API header

## Test

```
scripts/test
```

## Example

```
cd example
export CFLAGS='-fsanitize=address -fsanitize=undefined'
./build.sh
lldb -- ./main "while (x + 1) { y = 42; }"
```
