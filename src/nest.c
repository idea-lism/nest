// specs/cli.md
#include "common_header_gen.h"
#include "darray.h"
#include "irwriter.h"
#include "parse.h"
#include "post_process.h"
#include "re.h"
#include "ustr.h"
#include "xmalloc.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
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

// Check that the resolved compiler is clang. Prints error and returns false if not.
static bool _check_clang(void) {
  const char* cc = getenv("CC");
  if (!cc || !*cc) {
    cc = "clang";
  }
  // Run --version and check output contains "clang"
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", cc);
  FILE* p = popen(cmd, "r");
  if (!p) {
    fprintf(stderr, "nest: cannot run compiler: %s\n", cc);
    return false;
  }
  char buf[1024] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, p);
  buf[n] = '\0';
  int status = pclose(p);
  if (status != 0 || strstr(buf, "clang") == NULL) {
    fprintf(stderr, "nest: compiler must be clang (resolved: %s)\n", cc);
    return false;
  }
  return true;
}

static void _usage(void) {
  fprintf(stderr, "usage: nest <command> [options]\n"
                  "\n"
                  "commands:\n"
                  "  l    generate lexer from regex patterns\n"
                  "  c    generate parser from .nest syntax\n"
                  "  h    show nest syntax help\n"
                  "  r    show nest grammar reference\n"
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

  IrWriter* w = irwriter_new(fout);
  if (!_check_clang()) {
    fclose(fout);
    re_lex_del(l);
    return 1;
  }
  irwriter_gen_rt_simple(w);
  re_lex_gen(l, w, false);
  irwriter_end(w);
  irwriter_del(w);
  fclose(fout);
  re_lex_del(l);
  return 0;
}

static bool _is_scope_term_ex(Symtab* scope_names, int32_t id) {
  return id < symtab_count(scope_names) + scope_names->start_num;
}

static const char* _term_name_ex(Symtab* tokens, Symtab* scope_names, int32_t id) {
  if (_is_scope_term_ex(scope_names, id)) {
    return symtab_get(scope_names, id);
  }
  return symtab_get(tokens, id);
}

static char* _sanitize_ex(const char* name) {
  size_t len = strlen(name);
  char* out = XMALLOC(len + 1);
  for (size_t i = 0; i < len; i++) {
    out[i] = (name[i] == '@' || name[i] == '.') ? '_' : name[i];
  }
  out[len] = '\0';
  return out;
}

// Simple field name dedup: mirrors _field_dedup_next in peg.c.
typedef struct {
  char** names;
  int32_t count;
  int32_t cap;
} ExDedup;

static void _exd_init(ExDedup* d) { memset(d, 0, sizeof(*d)); }
static void _exd_free(ExDedup* d) {
  for (int32_t i = 0; i < d->count; i++) {
    XFREE(d->names[i]);
  }
  XFREE(d->names);
}
static char* _exd_next(ExDedup* d, const char* base) {
  int32_t occ = 0;
  for (int32_t i = 0; i < d->count; i++) {
    if (strcmp(d->names[i], base) == 0) {
      occ++;
    }
  }
  if (d->count >= d->cap) {
    d->cap = d->cap ? d->cap * 2 : 8;
    d->names = XREALLOC(d->names, (size_t)d->cap * sizeof(char*));
  }
  d->names[d->count++] = strdup(base);
  if (occ == 0) {
    return strdup(base);
  }
  char buf[256];
  snprintf(buf, sizeof(buf), "%s$%d", base, occ);
  return strdup(buf);
}

static char* _field_base(Symtab* tokens, Symtab* scope_names, Symtab* rule_names, PegUnit* u) {
  if (u->kind == PEG_TERM) {
    return _sanitize_ex(_term_name_ex(tokens, scope_names, u->id));
  }
  if (u->kind == PEG_CALL) {
    return strdup(symtab_get(rule_names, u->id));
  }
  return strdup("_unknown");
}

// Emit print statements for children of a SEQ.
static void _gen_print_children(FILE* f, const char* prefix, PegUnit* children, int32_t size, Symtab* tokens,
                                Symtab* scope_names, Symtab* rule_names, ExDedup* fd) {
  for (int32_t i = 0; i < size; i++) {
    PegUnit* u = &children[i];
    bool is_link = (u->multiplier == '*' || u->multiplier == '+');
    bool is_opt = (u->multiplier == '?');

    if (u->kind == PEG_TERM) {
      char* base = _field_base(tokens, scope_names, rule_names, u);
      char* fname = _exd_next(fd, base);
      bool is_scope = _is_scope_term_ex(scope_names, u->id);
      const char* tname = _term_name_ex(tokens, scope_names, u->id);
      if (is_scope) {
        int32_t rid = symtab_find(rule_names, tname);
        if (rid >= 0) {
          if (is_link) {
            fprintf(f, "    for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l))\n", fname, prefix, prefix);
            fprintf(f, "      print_%s(%s_get_lhs(&_l), depth + 1);\n", tname, prefix);
          } else {
            fprintf(f, "    print_%s(_n.%s, depth + 1);\n", tname, fname);
          }
        }
      } else if (is_link) {
        fprintf(f, "    for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l)) {\n", fname, prefix, prefix);
        fprintf(f, "      _indent(depth + 1); printf(\"%s\\n\");\n", tname);
        fprintf(f, "    }\n");
      } else {
        fprintf(f, "    _indent(depth + 1); printf(\"%s\\n\");\n", tname);
      }
      XFREE(fname);
      XFREE(base);
    } else if (u->kind == PEG_CALL) {
      const char* callee = symtab_get(rule_names, u->id);
      char* fname = _exd_next(fd, callee);
      if (is_link) {
        fprintf(f, "    for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l))\n", fname, prefix, prefix);
        fprintf(f, "      print_%s(%s_get_lhs(&_l), depth + 1);\n", callee, prefix);
      } else if (is_opt) {
        fprintf(f, "    if (%s_peg_size(_n.%s) > 0)\n", prefix, fname);
        fprintf(f, "      print_%s(_n.%s, depth + 1);\n", callee, fname);
      } else {
        fprintf(f, "    print_%s(_n.%s, depth + 1);\n", callee, fname);
      }
      XFREE(fname);
    }
  }
}

static void _gen_example_c(FILE* f, const char* prefix, ParseState* ps) {
  Symtab* tokens = &ps->tokens;
  Symtab* scope_names = &ps->scope_names;
  Symtab* rule_names = &ps->rule_names;
  PegRules rules = ps->peg_rules;

  fprintf(f, "#include <stdint.h>\n");
  fprintf(f, "#include <stdio.h>\n\n");
  fprintf(f, "#include \"%s.h\"\n\n", prefix);

  fprintf(f, "int32_t %s_next_cp(void* userdata) {\n", prefix);
  fprintf(f, "  return ustr_iter_next((UstrIter*)userdata);\n");
  fprintf(f, "}\n\n");

  // tok_name
  fprintf(f, "static const char* tok_name(int32_t id) {\n");
  fprintf(f, "  switch (id) {\n");
  int32_t n_tokens = symtab_count(tokens);
  for (int32_t i = 0; i < n_tokens; i++) {
    int32_t tok_id = i + tokens->start_num;
    const char* name = symtab_get(tokens, tok_id);
    if (strncmp(name, "@lit.", 5) == 0) {
      fprintf(f, "  case %d: return \"%s\";\n", tok_id, name);
    } else {
      fprintf(f, "  case TOK_");
      for (const char* s = name + 1; *s; s++) {
        fputc(toupper((unsigned char)*s), f);
      }
      fprintf(f, ": return \"%s\";\n", name);
    }
  }
  fprintf(f, "  default: return \"?\";\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n\n");

  fprintf(f, "static void _indent(int depth) {\n");
  fprintf(f, "  for (int i = 0; i < depth; i++) printf(\"  \");\n");
  fprintf(f, "}\n\n");

  // Token list printer: walk chunks recursively, scope tokens recurse with depth+1
  int32_t total_scope_ids = symtab_count(scope_names) + scope_names->start_num;
  fprintf(f, "static void print_tokens(TokenTree* tt, TokenChunk* chunk, int depth) {\n");
  fprintf(f, "  int32_t n = (int32_t)darray_size(chunk->tokens);\n");
  fprintf(f, "  for (int32_t i = 0; i < n; i++) {\n");
  fprintf(f, "    Token* tok = &chunk->tokens[i];\n");
  fprintf(f, "    if (tok->term_id < %d) {\n", total_scope_ids);
  fprintf(f, "      print_tokens(tt, &tt->table[tok->chunk_id], depth + 1);\n");
  fprintf(f, "    } else {\n");
  fprintf(f, "      _indent(depth);\n");
  fprintf(f, "      UstrByteSlice sl = ustr_slice_bytes(tt->src, tok->cp_start, tok->cp_start + tok->cp_size);\n");
  fprintf(f, "      printf(\"%%s \\\"%%.*s\\\"\\n\", tok_name(tok->term_id), sl.size, sl.s);\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
  fprintf(f, "}\n\n");

  // Forward declarations for print_<rule>
  size_t n_rules = darray_size(rules);
  for (size_t i = 0; i < n_rules; i++) {
    const char* rname = symtab_get(rule_names, rules[i].global_id);
    fprintf(f, "static void print_%s(PegRef ref, int depth);\n", rname);
  }
  fprintf(f, "\n");

  // Per-rule print functions
  for (size_t ri = 0; ri < n_rules; ri++) {
    PegRule* rule = &rules[ri];
    const char* rname = symtab_get(rule_names, rule->global_id);

    fprintf(f, "static void print_%s(PegRef ref, int depth) {\n", rname);
    fprintf(f, "  _indent(depth); printf(\"%s\\n\");\n", rname);
    fprintf(f, "  if (%s_peg_size(ref) <= 0) return;\n", prefix);
    fprintf(f, "  Node_%s _n = %s_load_%s(ref);\n", rname, prefix, rname);

    PegUnit* body = &rule->body;

    if (body->kind == PEG_SEQ) {
      ExDedup fd;
      _exd_init(&fd);
      _gen_print_children(f, prefix, body->children, (int32_t)darray_size(body->children), tokens, scope_names,
                          rule_names, &fd);
      _exd_free(&fd);
    } else if (body->kind == PEG_BRANCHES) {
      int32_t nb = (int32_t)darray_size(body->children);
      bool all_tagged = true;
      for (int32_t bi = 0; bi < nb; bi++) {
        if (!body->children[bi].tag) {
          all_tagged = false;
        }
      }
      ExDedup fd;
      _exd_init(&fd);
      if (all_tagged) {
        for (int32_t bi = 0; bi < nb; bi++) {
          PegUnit* branch = &body->children[bi];
          char* stag = _sanitize_ex(branch->tag);
          fprintf(f, "  %s (_n.is.%s) {\n", bi == 0 ? "if" : "} else if", stag);
          XFREE(stag);
          if (branch->kind == PEG_SEQ) {
            _gen_print_children(f, prefix, branch->children, (int32_t)darray_size(branch->children), tokens,
                                scope_names, rule_names, &fd);
          } else {
            _gen_print_children(f, prefix, branch, 1, tokens, scope_names, rule_names, &fd);
          }
        }
        fprintf(f, "  }\n");
      } else {
        // Untagged branches — all children share one dedup, only matched ones print.
        for (int32_t bi = 0; bi < nb; bi++) {
          PegUnit* branch = &body->children[bi];
          if (branch->kind == PEG_SEQ) {
            _gen_print_children(f, prefix, branch->children, (int32_t)darray_size(branch->children), tokens,
                                scope_names, rule_names, &fd);
          } else {
            _gen_print_children(f, prefix, branch, 1, tokens, scope_names, rule_names, &fd);
          }
        }
      }
      _exd_free(&fd);
    } else if (body->kind == PEG_TERM) {
      bool is_scope = _is_scope_term_ex(scope_names, body->id);
      const char* tname = _term_name_ex(tokens, scope_names, body->id);
      bool is_link = (body->multiplier == '*' || body->multiplier == '+');
      char* san = _sanitize_ex(tname);
      if (is_scope) {
        int32_t rid = symtab_find(rule_names, tname);
        if (rid >= 0) {
          if (is_link) {
            fprintf(f, "  for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l))\n", san, prefix, prefix);
            fprintf(f, "    print_%s(%s_get_lhs(&_l), depth + 1);\n", tname, prefix);
          } else {
            fprintf(f, "  print_%s(_n.%s, depth + 1);\n", tname, san);
          }
        }
      } else if (is_link) {
        fprintf(f, "  for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l)) {\n", san, prefix, prefix);
        fprintf(f, "    _indent(depth + 1); printf(\"%s\\n\");\n", tname);
        fprintf(f, "  }\n");
      } else {
        fprintf(f, "  _indent(depth + 1); printf(\"%s\\n\");\n", tname);
      }
      XFREE(san);
    } else if (body->kind == PEG_CALL) {
      const char* callee = symtab_get(rule_names, body->id);
      bool is_link = (body->multiplier == '*' || body->multiplier == '+');
      bool is_opt = (body->multiplier == '?');
      if (is_link) {
        fprintf(f, "  for (PegLink _l = _n.%s; %s_has_elem(&_l); %s_get_next(&_l))\n", callee, prefix, prefix);
        fprintf(f, "    print_%s(%s_get_lhs(&_l), depth + 1);\n", callee, prefix);
      } else if (is_opt) {
        fprintf(f, "  if (%s_peg_size(_n.%s) > 0)\n", prefix, callee);
        fprintf(f, "    print_%s(_n.%s, depth + 1);\n", callee, callee);
      } else {
        fprintf(f, "  print_%s(_n.%s, depth + 1);\n", callee, callee);
      }
    }

    fprintf(f, "}\n\n");
  }

  // main: lex+parse via <prefix>_parse
  fprintf(f, "int main(int argc, char** argv) {\n");
  fprintf(f, "  if (argc < 2) {\n");
  fprintf(f, "    fprintf(stderr, \"usage: %%s <input>\\n\", argv[0]);\n");
  fprintf(f, "    return 1;\n");
  fprintf(f, "  }\n\n");
  fprintf(f, "  FILE* fp = fopen(argv[1], \"rb\");\n");
  fprintf(f, "  if (!fp) {\n");
  fprintf(f, "    fprintf(stderr, \"cannot open %%s\\n\", argv[1]);\n");
  fprintf(f, "    return 1;\n");
  fprintf(f, "  }\n");
  fprintf(f, "  char* ustr = ustr_from_file(fp);\n");
  fprintf(f, "  fclose(fp);\n");
  fprintf(f, "  int32_t total_cps = ustr_size(ustr);\n");
  fprintf(f, "  ParseContext ctx = {.source_file_name = argv[1]};\n");
  fprintf(f, "  ParseResult res = %s_parse(&ctx, ustr);\n", prefix);
  fprintf(f, "  TokenTree* tt = res.tt;\n\n");

  // error detection: token error if not all input consumed
  fprintf(f, "  int32_t consumed = 0;\n");
  fprintf(f, "  {\n");
  fprintf(f, "    int32_t n = (int32_t)darray_size(tt->root->tokens);\n");
  fprintf(f, "    for (int32_t i = 0; i < n; i++) {\n");
  fprintf(f, "      Token* t = &tt->root->tokens[i];\n");
  fprintf(f, "      int32_t end = t->cp_start + t->cp_size;\n");
  fprintf(f, "      if (end > consumed) consumed = end;\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");
  fprintf(f, "  if (consumed < total_cps) {\n");
  fprintf(f, "    Location loc = tt_locate(tt, consumed);\n");
  fprintf(f, "    printf(\"%%d:%%d: token error\\n\", loc.line, loc.col);\n");
  fprintf(f, "  }\n");
  // syntax error: PEG didn't consume all tokens in its chunk
  fprintf(f, "  {\n");
  fprintf(f, "    int64_t peg_end = res.parse_end_col;\n");
  fprintf(f, "    int32_t n_tok = res.main.tc ? (int32_t)darray_size(res.main.tc->tokens) : 0;\n");
  fprintf(f, "    if (n_tok > 0 && peg_end >= 0 && peg_end < n_tok) {\n");
  fprintf(f, "      Token* err_tok = &res.main.tc->tokens[peg_end];\n");
  fprintf(f, "      Location loc = tt_locate(tt, err_tok->cp_start);\n");
  fprintf(f, "      printf(\"%%d:%%d: syntax error\\n\", loc.line, loc.col);\n");
  fprintf(f, "    }\n");
  fprintf(f, "  }\n");

  fprintf(f, "  printf(\"------\\n\");\n");
  fprintf(f, "  print_tokens(tt, tt->root, 0);\n");
  fprintf(f, "  printf(\"------\\n\");\n");
  fprintf(f, "  if (%s_peg_size(res.main) > 0) print_main(res.main, 0);\n\n", prefix);
  fprintf(f, "  %s_cleanup(&res);\n", prefix);
  fprintf(f, "  ustr_del(ustr);\n");
  fprintf(f, "  return 0;\n");
  fprintf(f, "}\n");
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

  if (!input) {
    _usage();
  }

  const char* prefix = arg_p ? arg_p : "nest";

  size_t prefix_len = strlen(prefix);
  if (prefix_len == 0 || prefix_len > 64) {
    fprintf(stderr, "nest: prefix must be 1-64 characters\n");
    return 1;
  }
  if (!isalpha((unsigned char)prefix[0])) {
    fprintf(stderr, "nest: prefix must start with a letter\n");
    return 1;
  }
  for (size_t i = 1; i < prefix_len; i++) {
    if (!isalnum((unsigned char)prefix[i]) && prefix[i] != '_') {
      fprintf(stderr, "nest: prefix must match [a-zA-Z][a-zA-Z0-9_]*\n");
      return 1;
    }
  }
  if (strcmp(prefix, "parse") == 0 || strcmp(prefix, "ustr") == 0 || strcmp(prefix, "load") == 0 ||
      strcmp(prefix, "lex") == 0) {
    fprintf(stderr, "nest: prefix name preserved\n");
    return 1;
  }
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

  char ll_path[72];
  char h_path[72];
  char c_path[72];
  snprintf(ll_path, sizeof(ll_path), "%s.ll", prefix);
  snprintf(h_path, sizeof(h_path), "%s.h", prefix);
  snprintf(c_path, sizeof(c_path), "%s.c", prefix);

  FILE* fout = fopen(ll_path, "w");
  if (!fout) {
    perror(ll_path);
    ustr_del(src);
    return 1;
  }

  FILE* fhdr = fopen(h_path, "w");
  if (!fhdr) {
    perror(h_path);
    fclose(fout);
    ustr_del(src);
    return 1;
  }

  if (!_check_clang()) {
    fclose(fout);
    fclose(fhdr);
    ustr_del(src);
    return 1;
  }

  IrWriter* w = irwriter_new(fout);
  irwriter_gen_rt(w, input, ".");
  HeaderWriter* hw = hdwriter_new(fhdr);

  // 1. Common header (runtime types, PegRef, PegLink)
  common_header_gen(hw);

  ParseState* ps = parse_state_new();

  int verbose = arg_v ? atoi(arg_v) : 0;
  if (verbose < 0) {
    verbose = 0;
  }

  int ret = 0;
  if (verbose) {
    fprintf(stderr, "[nest] parse\n");
  }
  if (!parse_nest(ps, src)) {
    fprintf(stderr, "nest: %s\n", parse_get_error(ps));
    ret = -1;
    goto cleanup;
  }
  if (verbose) {
    fprintf(stderr, "[nest] post_process\n");
  }
  if (!(verbose ? pp_all_passes_verbose(ps) : pp_all_passes(ps))) {
    fprintf(stderr, "nest: %s\n", parse_get_error(ps));
    ret = -1;
    goto cleanup;
  }

  int memoize_mode = MEMOIZE_SHARED;
  if (arg_m) {
    if (strcmp(arg_m, "none") == 0) {
      memoize_mode = MEMOIZE_NONE;
    } else if (strcmp(arg_m, "naive") == 0) {
      memoize_mode = MEMOIZE_NAIVE;
    } else if (strcmp(arg_m, "shared") == 0) {
      memoize_mode = MEMOIZE_SHARED;
    } else {
      fprintf(stderr, "nest: unknown memoize mode: %s\n", arg_m);
      ret = -1;
      goto cleanup;
    }
  }
  if (verbose) {
    fprintf(stderr, "[nest] peg_analyze\n");
  }
  PegGenInput gen_input = peg_analyze(
      &(PegAnalyzeInput){
          .rules = ps->peg_rules,
          .tokens = ps->tokens,
          .scope_names = ps->scope_names,
          .rule_names = ps->rule_names,
          .verbose_level = verbose,
      },
      memoize_mode, prefix);
  if (verbose) {
    fprintf(stderr, "[nest] peg_gen\n");
  }
  peg_gen(&gen_input, hw, w);
  if (verbose) {
    fprintf(stderr, "[nest] vpa_gen\n");
  }
  // compute main rule row for PegRef
  int32_t main_rule_row = 0;
  if (darray_size(gen_input.scope_closures) > 0) {
    ScopeClosure* cl0 = &gen_input.scope_closures[0];
    if (darray_size(cl0->scoped_rules) > 0) {
      main_rule_row = (int32_t)(cl0->bits_bucket_size * 2 + cl0->scoped_rules[0].slot_index);
    }
  }
  vpa_gen(
      &(VpaGenInput){
          .scopes = ps->vpa_scopes,
          .effect_decls = ps->effect_decls,
          .tokens = ps->tokens,
          .hooks = ps->hooks,
          .ignore_names = ps->ignores.names,
          .source_file_name = input,
          .re_frags = ps->re_frags,
      },
      hw, w, prefix, main_rule_row);

  {
    FILE* fc = fopen(c_path, "w");
    if (!fc) {
      perror(c_path);
      ret = -1;
      goto cleanup;
    }
    _gen_example_c(fc, prefix, ps);
    fclose(fc);
  }

  peg_analyze_free(&gen_input);

cleanup:
  parse_state_del(ps);
  irwriter_end(w);
  irwriter_del(w);
  hdwriter_del(hw);
  fclose(fout);
  fclose(fhdr);
  ustr_del(src);
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
