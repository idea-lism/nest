src/vpa.c is a vpa generator.

It iterates parsed regexp structure, utilize src/re.h to generate code.

### Generated code

generated header defines interface to interact with the LLVM-IR defined parser.

- user must define states and hooks for lexing
- each state is a `void*`, user have custom interpretation of it (string? number? string array? etc).
  - in hooks user can update states.
  - if states are used in matching, user also need to implement `match_{state_id}(const char*, userdata)` so lexer can do dynamic matching by current state. for example:
    - `foo = $foo` means the matching of the `foo` rule is totally customed by `match_foo(current_src_pointer, userdata)`
