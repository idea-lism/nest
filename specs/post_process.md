# Post-process AST and validate it

Create "src/post_process.c".

### vpa: inline macros

`bool pp_inline_macros(ParseState* ps);`:
1. inline macro vpa rules
   - note that macro rules reference each other
   - do a topological sort by dependency, report error on recursive dependency
   - expand by topo-sort order
2. check sub-scope calls: if a sub-scope call is a macro, report error
3. then build a new list of VPA scopes to replace the old one, including only the non-macro ones.

### peg: auto tag branches

`bool pp_auto_tag_branches(ParseState* ps);`:
- with the first token / sub_rule name
- in a rule definition, tags must be distinct, or the parser will raise an error on conflict tag names
- rule name must not exceed 128 bytes.

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

To prevent codegen problems, tags cannot take any of the C keywords:
```
alignas (C23)
alignof (C23)
auto
bool (C23)
break
case
char
const
constexpr (C23)
continue
default
do
double
else
enum

extern
false (C23)
float
for
goto
if
inline (C99)
int
long
nullptr (C23)
register
restrict (C99)
return
short
signed

sizeof
static
static_assert (C23)
struct
switch
thread_local (C23)
true (C23)
typedef
typeof (C23)
typeof_unqual (C23)
union
unsigned
void
volatile
while

_Alignas (C11)(deprecated in C23)
_Alignof (C11)(deprecated in C23)
_Atomic (C11)
_BitInt (C23)
_Bool (C99)(deprecated in C23)
_Complex (C99)
_Decimal128 (C23)
_Decimal32 (C23)
_Decimal64 (C23)
_Generic (C11)
_Imaginary (C99)
_Noreturn (C11)(deprecated in C23)
_Static_assert (C11)(deprecated in C23)
_Thread_local (C11)(deprecated in C23)
```

### peg: left recursion / infinite loop detect

`bool pp_detect_left_recursions(ParseState* ps)`:
- walk down peg rules to detect left recursions -- we don't allow this infinite loop.
  - when analyzing, be ware of the scope boundary: if there is a scope, we don't expand it. for example str is defined `str = str_char*`, but it always takes a slot in token stream so parsing the scope won't result in infinite loop.
- for interlace rule `lhs*<rhs>` / `lhs+<rhs>`, if both `lhs` and `rhs` are nullable, report error.
- if a rule being called is not defined, report error

### vpa scope validity

`bool pp_validate_vpa_scopes(ParseState* ps)`:
- a `main` must exist.
- no repeated scope names.
- scopes except `main` must have a `.begin` (or user hook that produces the `.begin` effect) and one or more `.end` (or user hook that produces the `end` effect)
- a leading `re`/`re_str`/``re_frag_id` (before scope bracket) must contain at least 1 char in it
- inside a scope bracket `{ ... }`, after macro expansion, there can only be 1 `re`/`re_str`/``re_frag_id` that is empty
  - if the branch is empty, it means a fallback action, the following action must have one of `.end`/`.fail`

### peg rule validity

`bool pp_validate_peg_rules(ParseState* ps)`:
- `main` must exist.
- no repeated peg rule names.

### vpa & peg

`bool pp_match_scopes(ParseState* ps)`:
- Check if VPA scope has a corresponding PEG parser in the same name, if found, update VPA scope's attr `VpaRule.has_parser = true`.
- For `main` scope, validate `has_parser` must be true.
- for every PEG scope, `used_set` must be the same as `emit_set` in the related VPA scope
  - emit_set including tokens and scopes, but if the scope doesn't have a mapping peg parser, expand it.
    - exclude ignore tokens in emit_set
    - if scope leader has token emits after `.begin` hook, include it
    - if a token is emit after `.end` hook, scopes that calls current scope should include the token
  - used_set including tokens and scopes, if a child-rule doesn't have a mapping vpa scope, expand it.
  - for example,
    1. with vpa rule `foo = /.../ { @a ... }`, `bar = /.../ { @b ... foo ... }`, then `bar`'s emit_set is `{@b, foo}`
    2. with peg rule `foo = @a`, `baz = @c?`, `bar = foo @b baz`, and `baz` is not a scope, then `bar`'s used_set is `{@b, @c, foo}`
    3. `{@b, foo} != {@b, @c, foo}`, then it is a mismatch, then we should raise error to tell user this rule doesn't add up

### Acceptance criteria

- on macro inlining
  - must test recursion
  - must test cascaded expanding (re-arrange macro definitions so we can see the sorting effect)
- on emit set computing
  - must not expand scopes that have a mapping peg parser
  - expanding should not hit infinite recursion
