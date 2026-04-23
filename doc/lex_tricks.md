The hook system is very extendable.

# Parsing without PEG

Consider parsing a number / string, in traditional lexer we'd associate a value to it to speed up parsing:

```nest
[[lex]]
main = {
  str
  *others
}

%effect .str_try_emit = @string | .noop
%effect .str_try_end = .end | .noop

str = /['"]/ .str_start .begin {
  /['"]/ .str_try_emit .str_try_end
  /\\[bfnrtv]/ .str_c_escape
  ...
  /./ .str_char
}

[[peg]]
main = [
  @string
  ...
]
```

Then we design state and hook to skip PEG parsing for `str` scope completely, and output an `@string` token directly.

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

static int str_try_emit(void* state, size_t size, const char* str) {
  LexerState* s = state;
  if (s->str_delim == *str) {
    str_table_add(str, s->str_buf, s->str_ptr);
    return TOK_STRING;
  } else {
    str_char(state, size, str);
    return TOK_HOOK_NOOP;
  }
}

static int str_try_end(void* state, size_t size, const char* str) {
  LexerState* s = state;
  if (s->str_delim == *str) {
    return TOK_HOOK_END;
  } else {
    str_char(state, size, str);
    return TOK_HOOK_NOOP;
  }
}
```

The caveat is the resulting text range is set to the last token instead of the whole scope.

# Custom lexing

There is no such thing actually. In PEG we can load the token's string range and use custom converter instead.
