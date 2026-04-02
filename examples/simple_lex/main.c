#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  int64_t state;
  int64_t action;
} LexResult;

extern LexResult lex(int64_t state, int64_t cp);

static const char* tok_names[] = {
    [1] = "IF",
    [2] = "ELSE",
    [3] = "WHILE",
    [4] = "IDENT",
    [5] = "NUMBER",
    [6] = "SPACE",
};

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <input>\n", argv[0]);
    return 1;
  }

  const char* input = argv[1];
  int32_t len = (int32_t)strlen(input);
  int32_t pos = 0;

  while (pos < len) {
    int64_t state = 0;
    int32_t last_action = 0;
    int32_t last_end = pos;

    for (int32_t cur = pos; cur < len; cur++) {
      int32_t cp = (unsigned char)input[cur];
      LexResult r = lex(state, (int64_t)cp);
      if (r.action == -2) {
        break;
      }
      if (r.action > 0) {
        last_action = (int32_t)r.action;
        last_end = cur + 1;
      }
      state = r.state;
    }

    if (last_action > 0) {
      int32_t tok_len = last_end - pos;
      printf("%-8s \"%.*s\"\n", tok_names[last_action], tok_len, input + pos);
      pos = last_end;
    } else {
      fprintf(stderr, "unrecognized: '%c'\n", input[pos]);
      pos++;
    }
  }

  return 0;
}
