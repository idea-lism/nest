- Use Ruby to config -- `config.rb` loads `config.in.rb`, create `build.ninja` by environment
  - `config.rb` should be universal -- as if it can be used in other projects
  - `config.in.rb` should be simple, just:
    - what target wants what sources
    - what extra flags / cflags should be used in different envs
  - controls: `config.rb debug` and `config.rb release` generates different results

- Implement in C, not C++.
