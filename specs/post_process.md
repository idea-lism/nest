# Post-process AST and validate it

Create "src/post_process.c".

### vpa: inline macros

`bool pp_inline_macros(ParseState* ps);`:
- inline macro vpa rules

### peg: auto tag branches

`bool pp_auto_tag_branches(ParseState* ps);`:
- with the first token / sub_rule name
- in a rule definition, tags must be distinct, or the parser will raise an error on conflict tag names
- epsilon branch can have a tag too, for example:

### peg: tag logic

`bool pp_check_duplicate_tags(ParseState* ps);`:
branch tagging example:

```conf
foo = a [
  @foo bar # not specifying, induce tag "foo" by first token
  sub_rule # not specifying, induce tag "sub_rule" by first sub rule
  @foo : tag2 # tag "foo" already used, must specify tag so there won't be conflict
  "a-keyword" : tag3 # tag can't be induced by keyword literal, so tag it manually
  : tag4             # epsilon branch needs to be tagged
]
```

When there are multiple branches in one rule, all branches are tagged. for example:

```conf
# resulting rule will have 4 branch tags b1 .. b4
foo = a [
  b1
  b2
] [
  b3
  b4
]
```

### peg: left recursion detect

`bool pp_detect_left_recursions(ParseState* ps)`:
- walk down peg rules to detect left recursions -- we don't allow this infinite loop.
  - when analyzing, be ware of the scope boundary: if there is a scope, we don't expand it. for example str is defined `str = str_char*`, but it always takes a slot in token stream so parsing the scope won't result in infinite loop.

### other validations

`bool pp_validate(ParseState* ps);`:
- `main` must exist in `[[vpa]]` and `[[peg]]`
- a leading `re`/`re_str`/``re_frag_id` must contain at least 1 char in it
- in a scope, there can only be 1 `re`/`re_str`/``re_frag_id` that is empty
  - if the branch is empty, it means a fallback action, the following action must have one of `.end`/`.fail`
- each scope in `[[vpa]]` except `main` must have a `.begin` (or user hook that produces the `.begin` effect) and one or more `.end` (or user hook that produces the `end` effect)
- for a same scope, used token set in `[[peg]]` must be the same as emit token set in `[[vpa]]`
  - for example, 
    - with vpa rule `foo = /.../ @a`, `bar = foo @b`, `bar`'s emit token set is `{@a, @b}` (including descendant's)
    - with peg rule `foo = @c?`, `bar = foo @b`, `bar`'s used token set is `{@c, @b}`, this doesn't equal to the vpa rule's token set
    - it is a mismatch, then we should raise error to tell user this rule doesn't add up
