- Use Ruby to config -- `config.rb` loads `config.in.rb`, create `build.ninja` by environment
  - `config.rb` should be universal -- as if it can be used in other projects
  - `config.in.rb` should be simple, just:
    - what target wants what sources
    - what extra flags / cflags should be used in different envs
  - controls: `config.rb debug` and `config.rb release` generates different results
  - for download task, use Ruby's builtin open-uri to download
  - for unzip task, add unzip prereq in README.md

Additional verifications:

- ASAN
- Test coverage

Outputs:

- out/libre.a : includes everything

CI:

- Setup github CI related files

Cross-Platform compatibility

- test/compat.c defines compat functions used in test
