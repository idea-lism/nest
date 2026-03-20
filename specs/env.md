- Use Ruby to config -- `config.rb` loads `config.in.rb`, create `build.ninja` by environment
  - `config.rb` should be universal -- as if it can be used in other projects
  - `config.in.rb` should be simple, just:
    - what target wants what sources
    - what extra flags / cflags should be used in different envs
  - controls: `config.rb debug` and `config.rb release` generates different results

- Implement in C, not C++.

- Clang-format:
  - line width: `120`
  - if style: `if (xxx) {\n ... \n} else {...`
  - pointer arg style: `foo* foo`
  - enforce brace on block bodies
  - on macOS, use `xcrun clang-format` to format
  - others use default (indent = 2 space)
