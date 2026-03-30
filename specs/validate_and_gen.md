Takes parser AST, validate and call codegen.

Create "src/validate_and_gen.c".

- `bool validate_and_gen(ParseState* s, HeaderWriter* header_writer, IrWriter* ir_writer)`

### Nest file definition validation

- `main` must exist in `[[vpa]]`
- each scope in `[[vpa]]` must have a `.begin` (or user hook that produces the `.begin` effect) and one or more `.end` (or user hook that produces the `end` effect)
- for a same scope, used token set in `[[peg]]` must be the same as emit token set in `[[vpa]]`
  - for example, 
    - with vpa rule `foo = /.../ @a`, `bar = foo @b`, `bar`'s emit token set is `{@a, @b}` (including descendant's)
    - with peg rule `foo = @c?`, `bar = foo @b`, `bar`'s used token set is `{@c, @b}`, this doesn't equal to the vpa rule's token set
    - it is a mismatch, then we should raise error to tell user this rule doesn't add up

### Peg scope isolation

`_detect_left_recursions()` will walk down peg rules to detect left recursions, to prevent error.

### code generator invocation

Use `src/vpa.c` and `src/peg.c` for the code generating.
