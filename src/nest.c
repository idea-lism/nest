// specs/nest.md
#include "parse.h"
#include "post_process.h"
#include "re.h"
#include "ustr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const cmdopt_set = "set";

#define CMDOPT_MATCH(s, l, n, d, i)                                                                                    \
  {                                                                                                                    \
    if (0 == strcmp("-" #s, argv[i])) {                                                                                \
      if (n == 0) {                                                                                                    \
        arg_##s = cmdopt_set;                                                                                          \
        continue;                                                                                                      \
      } else if (n == 2 && (i + 1 >= argc || argv[i + 1][0] == '-')) {                                                 \
        arg_##s = cmdopt_set;                                                                                          \
        continue;                                                                                                      \
      } else if (i + 1 < argc) {                                                                                       \
        arg_##s = argv[++i];                                                                                           \
        continue;                                                                                                      \
      }                                                                                                                \
    } else if (0 == strncmp(n == 0 ? "--" #l : "--" #l "=", argv[i], strlen("--" #l "="))) {                           \
      if (n == 0) {                                                                                                    \
        arg_##s = cmdopt_set;                                                                                          \
        continue;                                                                                                      \
      } else {                                                                                                         \
        arg_##s = argv[i] + strlen("--" #l "=");                                                                       \
        continue;                                                                                                      \
      }                                                                                                                \
    }                                                                                                                  \
  }

#define CMDOPT_USAGE(s, l, n, d)                                                                                       \
  if (n == 0)                                                                                                          \
    fprintf(stderr, "  -" #s ", --" #l "\t" d "\n");                                                                   \
  else                                                                                                                 \
    fprintf(stderr, "  -" #s " <" #l ">, --" #l "=<arg>\t" d "\n");

// --- CLI ---

static const char* _detect_triple(void) {
#ifdef _WIN32
  FILE* p = popen("clang --print-target-triple", "r");
#else
  FILE* p = popen("clang --print-target-triple 2>/dev/null", "r");
#endif
  if (!p) {
    return DEFAULT_TRIPLE;
  }
  static char buf[128];
  if (!fgets(buf, sizeof(buf), p)) {
    pclose(p);
    return DEFAULT_TRIPLE;
  }
  pclose(p);
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    buf[len - 1] = '\0';
  }
  return buf;
}

static void _usage(void) {
  fprintf(stderr, "usage: nest <command> [options]\n"
                  "\n"
                  "commands:\n"
                  "  l    generate lexer from regex patterns\n"
                  "  c    generate parser from .nest syntax\n"
                  "  h    show .nest syntax reference\n"
                  "  r    show bootstrap.nest reference\n"
                  "\n"
                  "nest l <input> [options]\n");
#define OPTION(s, l, n, d) CMDOPT_USAGE(s, l, n, d)
#include "lex_opts.inc"
  fprintf(stderr, "\nnest c <input.nest> [options]\n");
#define OPTION(s, l, n, d) CMDOPT_USAGE(s, l, n, d)
#include "compile_opts.inc"
  exit(1);
}

#include "../build/nest_syntax.inc"

static void _cmd_help(void) {
  fputs((const char*)NEST_SYNTAX, stdout);
  exit(0);
}

#include "../build/nest_reference.inc"

static void _cmd_reference(void) {
  fputs((const char*)NEST_REFERENCE, stdout);
  exit(0);
}

static int32_t _cmd_lex(int32_t argc, char** argv) {
#define OPTION(s, l, n, d) const char* arg_##s = 0;
#include "lex_opts.inc"

  const char* input = NULL;
  for (int32_t i = 0; i < argc; i++) {
#define OPTION(s, l, n, d) CMDOPT_MATCH(s, l, n, d, i)
#include "lex_opts.inc"
    if (!input) {
      input = argv[i];
      continue;
    }
    _usage();
  }

  if (!input || !arg_o) {
    _usage();
  }

  const char* output = arg_o;
  const char* func_name = arg_f ? arg_f : "lex";
  const char* mode = arg_m ? arg_m : "";
  const char* triple = (arg_t == cmdopt_set) ? _detect_triple() : arg_t;
  FILE* fin = fopen(input, "r");
  if (!fin) {
    perror(input);
    return 1;
  }

  ReLex* l = re_lex_new(func_name, input, mode);
  char line[4096];
  int32_t lineno = 0;
  int32_t action_id = 1;
  while (fgets(line, sizeof(line), fin)) {
    lineno++;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    int32_t r = re_lex_add(l, line, lineno, 0, action_id);
    if (r < 0) {
      fprintf(stderr, "%s:%d: parse error (%d)\n", input, lineno, r);
      fclose(fin);
      re_lex_del(l);
      return 1;
    }
    action_id++;
  }
  fclose(fin);

  FILE* fout = fopen(output, "w");
  if (!fout) {
    perror(output);
    re_lex_del(l);
    return 1;
  }

  IrWriter* w = irwriter_new(fout, triple);
  irwriter_start(w, input, ".");
  re_lex_gen(l, w, false);
  irwriter_end(w);
  irwriter_del(w);
  fclose(fout);
  re_lex_del(l);
  return 0;
}

static int32_t _cmd_compile(int32_t argc, char** argv) {
#define OPTION(s, l, n, d) const char* arg_##s = 0;
#include "compile_opts.inc"

  const char* input = NULL;
  for (int32_t i = 0; i < argc; i++) {
#define OPTION(s, l, n, d) CMDOPT_MATCH(s, l, n, d, i)
#include "compile_opts.inc"
    if (!input) {
      input = argv[i];
      continue;
    }
    _usage();
  }

  if (!input || !arg_o) {
    _usage();
  }

  const char* output = arg_o;
  const char* triple = (arg_t == cmdopt_set) ? _detect_triple() : arg_t;
  FILE* fin = fopen(input, "r");
  if (!fin) {
    perror(input);
    return 1;
  }
  char* src = ustr_from_file(fin);
  fclose(fin);
  if (!src) {
    return 1;
  }

  size_t out_len = strlen(output);
  char* header_path = malloc(out_len + 3);
  memcpy(header_path, output, out_len);
  if (out_len >= 3 && strcmp(output + out_len - 3, ".ll") == 0) {
    strcpy(header_path + out_len - 3, ".h");
  } else {
    strcpy(header_path + out_len, ".h");
  }

  FILE* fout = fopen(output, "w");
  if (!fout) {
    perror(output);
    ustr_del(src);
    free(header_path);
    return 1;
  }

  FILE* fhdr = fopen(header_path, "w");
  if (!fhdr) {
    perror(header_path);
    fclose(fout);
    ustr_del(src);
    free(header_path);
    return 1;
  }

  IrWriter* w = irwriter_new(fout, triple);
  irwriter_start(w, input, ".");
  HeaderWriter* hw = hw_new(fhdr);
  ParseState* ps = parse_state_new();

  int ret = 0;
  if (!parse_nest(ps, src)) {
    fprintf(stderr, "nest: %s\n", parse_get_error(ps));
    ret = -1;
    goto cleanup;
  }
  if (!pp_all_passes(ps)) {
    fprintf(stderr, "nest: %s\n", parse_get_error(ps));
    ret = -1;
    goto cleanup;
  }

  bool compress_memoize = !(arg_k && strcmp(arg_k, "false") == 0);
  const char* main_func_name = arg_m;
  peg_gen(&(PegGenInput){.rules = ps->peg_rules}, hw, w, compress_memoize);
  vpa_gen(
      &(VpaGenInput){
          .rules = ps->vpa_rules,
          .effects = ps->effects,
          .src = ps->src,
      },
      hw, w, main_func_name);

cleanup:
  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);
  fclose(fout);
  fclose(fhdr);
  ustr_del(src);
  free(header_path);
  return ret;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    _usage();
  }

  const char* cmd = argv[1];
  if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    _usage();
  }

  if (strcmp(cmd, "l") == 0) {
    return _cmd_lex(argc - 2, argv + 2);
  } else if (strcmp(cmd, "c") == 0) {
    return _cmd_compile(argc - 2, argv + 2);
  } else if (strcmp(cmd, "h") == 0) {
    _cmd_help();
  } else if (strcmp(cmd, "r") == 0) {
    _cmd_reference();
  } else {
    fprintf(stderr, "unknown command: %s\n", cmd);
    _usage();
  }
  return 1;
}
