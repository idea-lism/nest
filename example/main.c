#include "re_rt.h"

#include <stdio.h>
#include <string.h>

#define LEX_CP_EOF (-2)
#define LEX_ACTION_NOMATCH (-2)

typedef struct {
  int64_t state;
  int64_t action;
} LexResult;

extern LexResult lex(int64_t state, int64_t cp);

static const char* action_names[] = {
    [0] = "?", [1] = "if", [2] = "else", [3] = "while", [4] = "ident", [5] = "number", [6] = "space",
    [7] = "=", [8] = "+",  [9] = "(",    [10] = ")",    [11] = "{",    [12] = "}",     [13] = ";",
};

int main(int argc, char** argv) {
  const char* input = (argc > 1) ? argv[1] : "if x = 42;";

  char* s = ustr_new(strlen(input), input);
  int32_t size = ustr_bytesize(s);

  UstrIter it;
  ustr_iter_init(&it, s, 0);

  int64_t state = 0;
  int32_t last_action = 0, tok_start = 0;

  for (;;) {
    int32_t byte_off = it.byte_off;
    int32_t cp = (byte_off < size) ? ustr_iter_next(&it) : LEX_CP_EOF;

    LexResult r = lex(state, cp);

    if (r.action == LEX_ACTION_NOMATCH) {
      if (last_action > 0) {
        printf("[%s] %.*s\n", action_names[last_action], (int)(byte_off - tok_start), input + tok_start);
      }
      tok_start = byte_off;
      last_action = 0;
      state = 0;
      if (cp == LEX_CP_EOF) {
        break;
      }
      r = lex(0, cp);
      if (r.action == LEX_ACTION_NOMATCH) {
        tok_start = it.byte_off;
        continue;
      }
    }

    last_action = (int32_t)r.action;
    state = r.state;
    if (cp == LEX_CP_EOF) {
      break;
    }
  }

  ustr_del(s);
}
