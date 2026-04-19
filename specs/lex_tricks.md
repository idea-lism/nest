The hook system is very extendable.

# Parsing without PEG

Consider parsing a number / string, in traditional lexer we'd associate a value to it to speed up parsing:

```nest
[[lex]]
main = {
  str
  *others
}

%effect .str_end_emit = .end | .str_char | @str

str = /['"]/ .str_start .begin {
  /['"]/ .str_end_emit
  /\\[bfnrtv]/ .str_c_escape
  ...
}

[[peg]]
main = [
  @str
  ...
]
```

Then we design state and hook to skip PEG parsing for `str` scope completely, and output an `@str` token directly.

```c
typedef struct {
  // a table to map original ptr to parsed string
  StrTable str_table;

  // a shared buf for building
  char* str_buf;
  char* str_ptr;
  char* str_buf_end;
  char str_delim;
} LexerState;

static void str_start(void* state, size_t size, const char* str) {
  LexerState* s = state;
  s->str_ptr = s->str_buf;
  s->str_delim = *str;
}

static void str_c_escape(void* state, size_t size, const char* str) {
  LexerState* s = state;
  switch (str[1]) {
    case 't':
      *s->str_ptr = '\t'
      break;
    ...
  }
  s->str_ptr++;
}

static void str_char(void* state, size_t size, const char* str) {
  LexerState* s = state;
  ... // bound check & realloc
  *s->str_ptr = *str;
  s->str_ptr++;
}

static int str_end_emit(void* state, size_t size, const char* str) {
  LexerState* s = state;
  str_table_add(str, s->str_buf, s->str_ptr);
  return TOK_STR;
}
```

The caveat is the resulting text range is set to the last token instead of the whole scope.

# Custom lexing

There is no such thing actually. In PEG we can load the token's string range and use custom converter instead.
