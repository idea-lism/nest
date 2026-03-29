Takes parser AST, validate and call codegen.

### Nest file definition validation

- `main` must exist in `[[vpa]]`
- each scope in `[[vpa]]` must have a `.begin` (or user hook that produces the `.begin` effect) and one or more `.end` (or user hook that produces the `end` effect)
- for a same scope, used token set in `[[peg]]` must be the same as emit token set in `[[vpa]]`
  - for example, 
    - with vpa rule `foo = /.../ @a`, `bar = foo @b`, `bar`'s emit token set is `{@a, @b}` (including descendant's)
    - with peg rule `foo = @c?`, `bar = foo @b`, `bar`'s used token set is `{@c, @b}`, this doesn't equal to the vpa rule's token set
    - it is a mismatch, then we should raise error to tell user this rule doesn't add up

### Implemenation validations

- Token ids are allocated scopes too
- Recursive descend parsers work on scope level
  - the input token stream buffer is a scoped chunk
  - no expand sub-scope parsing -- sub-scopes are just a token_id match
- String literals are stored with source offset + length, no extra allocations for them
- Regexps are converted to structured AST that can be used by `src/vpa.c`
- When a scope starts, it should allocate a new token stream chunk.
- When a scope is complete (at `.end` hook), it should invoke recursive descend parsing on the token stream chunk

### code generator invocation

Use `src/vpa.c` and `src/peg.c` for the code generating.
