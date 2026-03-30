#include "../src/parse.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Lifecycle ---

TEST(test_state_new_del) {
  ParseState* ps = parse_state_new();
  assert(ps != NULL);
  parse_state_del(ps);
}

TEST(test_state_initial_no_error) {
  ParseState* ps = parse_state_new();
  assert(!parse_has_error(ps));
  assert(parse_get_error(ps)[0] == '\0');
  parse_state_del(ps);
}

// --- Error helpers ---

TEST(test_parse_error_sets_message) {
  ParseState* ps = parse_state_new();
  parse_error(ps, "bad token at %d", 42);
  assert(parse_has_error(ps));
  assert(strstr(parse_get_error(ps), "42") != NULL);
  parse_state_del(ps);
}

TEST(test_parse_error_truncates_long_message) {
  ParseState* ps = parse_state_new();
  char buf[1024];
  memset(buf, 'x', sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  parse_error(ps, "%s", buf);
  assert(parse_has_error(ps));
  assert(strlen(parse_get_error(ps)) < 512);
  parse_state_del(ps);
}

// --- String helpers ---

TEST(test_parse_sfmt) {
  char* s = parse_sfmt("hello %s %d", "world", 7);
  assert(s != NULL);
  assert(strcmp(s, "hello world 7") == 0);
  free(s);
}

TEST(test_parse_set_str) {
  char* dst = NULL;
  char* s = parse_sfmt("abc");
  parse_set_str(&dst, s);
  assert(dst == s);
  char* s2 = parse_sfmt("def");
  parse_set_str(&dst, s2);
  assert(dst == s2);
  free(dst);
}

// --- Reject invalid input ---

TEST(test_empty_input) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, "");
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

TEST(test_null_src) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, NULL);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

TEST(test_garbage_input) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, "not a nest file at all");
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

TEST(test_vpa_only_no_peg) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, "[[vpa]]\nmain = { *noise }\n*noise = { /[ \\t\\n]+/ @space }\n");
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

TEST(test_peg_only_no_vpa) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, "[[peg]]\nmain = @peg_id\n");
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

// --- Minimal valid input ---

static const char MINIMAL_NEST[] = "[[vpa]]\n"
                                   "main = { *noise }\n"
                                   "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment }\n"
                                   "%ignore @space @comment\n"
                                   "[[peg]]\n"
                                   "main = @nl*\n";

TEST(test_minimal_parse) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, MINIMAL_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  assert(!parse_has_error(ps));
  parse_state_del(ps);
}

// --- VPA directives ---

static const char DIRECTIVES_NEST[] = "[[vpa]]\n"
                                      "%ignore @space @comment\n"
                                      "%state $mode\n"
                                      "%keyword ops \"=\" \"|\"\n"
                                      "%define ID /[a-z_]\\w*/\n"
                                      "main = { *noise }\n"
                                      "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                      "[[peg]]\n"
                                      "main = @nl*\n";

TEST(test_directives_state) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, DIRECTIVES_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- VPA rules with scope ---

static const char SCOPE_NEST[] = "[[vpa]]\n"
                                 "%ignore @space @comment\n"
                                 "main = { inner *noise }\n"
                                 "inner = /\\(/ .begin { /\\)/ .end *noise }\n"
                                 "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                 "[[peg]]\n"
                                 "main = @nl*\n";

TEST(test_scope_rule) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, SCOPE_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- VPA hooks ---

static const char HOOKS_NEST[] = "[[vpa]]\n"
                                 "%ignore @space @comment\n"
                                 "main = { grp *noise }\n"
                                 "grp = /\\(/ .begin { /\\)/ .unparse .end *noise }\n"
                                 "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                 "[[peg]]\n"
                                 "main = @nl*\n";

TEST(test_hooks) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, HOOKS_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Keyword expansion ---

static const char KEYWORD_NEST[] = "[[vpa]]\n"
                                   "%ignore @space @comment\n"
                                   "%keyword ops \"=\" \"|\"\n"
                                   "main = { ops *noise }\n"
                                   "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                   "[[peg]]\n"
                                   "main = @nl*\n";

TEST(test_keyword_expansion) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, KEYWORD_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Macro rules ---

static const char MACRO_NEST[] = "[[vpa]]\n"
                                 "%ignore @space @comment\n"
                                 "main = { *noise }\n"
                                 "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                 "[[peg]]\n"
                                 "main = @nl*\n";

TEST(test_macro_rule) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, MACRO_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Regexp with character classes ---

static const char RE_CHARCLASS_NEST[] = "[[vpa]]\n"
                                        "%ignore @space @comment\n"
                                        "main = { *noise }\n"
                                        "*noise = { /[a-zA-Z_]\\w*/ @space /[ \\t\\n]+/ @comment /\\n+/ @nl }\n"
                                        "[[peg]]\n"
                                        "main = @nl*\n";

TEST(test_re_charclass) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, RE_CHARCLASS_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Regexp with alternation and grouping ---

static const char RE_ALT_NEST[] = "[[vpa]]\n"
                                  "%ignore @space @comment\n"
                                  "main = { *noise }\n"
                                  "*noise = { /(a|b|c)+/ @space /[ \\t\\n]+/ @comment }\n"
                                  "[[peg]]\n"
                                  "main = @nl*\n";

TEST(test_re_alternation) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, RE_ALT_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Regexp with special classes ---

static const char RE_CLASSES_NEST[] = "[[vpa]]\n"
                                      "%ignore @space @comment\n"
                                      "main = { *noise }\n"
                                      "*noise = { /\\w+/ @space /\\d+/ @comment /\\s+/ @nl }\n"
                                      "[[peg]]\n"
                                      "main = @nl*\n";

TEST(test_re_special_classes) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, RE_CLASSES_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- PEG with branches ---

static const char PEG_BRANCHES_NEST[] = "[[vpa]]\n"
                                        "%ignore @space @comment\n"
                                        "main = { /[a-z]+/ @tok_a /[0-9]+/ @tok_b *noise }\n"
                                        "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                        "[[peg]]\n"
                                        "main = item+\n"
                                        "item = [\n"
                                        "  @tok_a\n"
                                        "  @tok_b\n"
                                        "]\n";

TEST(test_peg_branches) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, PEG_BRANCHES_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- PEG with tagged branches ---

static const char PEG_TAGGED_NEST[] = "[[vpa]]\n"
                                      "%ignore @space @comment\n"
                                      "main = { /[a-z]+/ @tok_a /[0-9]+/ @tok_b *noise }\n"
                                      "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                      "[[peg]]\n"
                                      "main = item+\n"
                                      "item = [\n"
                                      "  @tok_a : alpha\n"
                                      "  @tok_b : numeric\n"
                                      "]\n";

TEST(test_peg_tagged_branches) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, PEG_TAGGED_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- PEG with multipliers ---

static const char PEG_MULT_NEST[] = "[[vpa]]\n"
                                    "%ignore @space @comment\n"
                                    "main = { /[a-z]+/ @tok_a /,/ @tok_sep *noise }\n"
                                    "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                    "[[peg]]\n"
                                    "main = @tok_a+<@tok_sep>\n";

TEST(test_peg_multiplier_interlace) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, PEG_MULT_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- PEG with optional ---

static const char PEG_OPT_NEST[] = "[[vpa]]\n"
                                   "%ignore @space @comment\n"
                                   "main = { /[a-z]+/ @tok_a /;/ @tok_semi *noise }\n"
                                   "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                   "[[peg]]\n"
                                   "main = @tok_a @tok_semi?\n";

TEST(test_peg_optional) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, PEG_OPT_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- PEG duplicate tag error ---

static const char PEG_DUP_TAG_NEST[] = "[[vpa]]\n"
                                       "%ignore @space @comment\n"
                                       "main = { /[a-z]+/ @tok_a /[0-9]+/ @tok_b *noise }\n"
                                       "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                       "[[peg]]\n"
                                       "main = item+\n"
                                       "item = [\n"
                                       "  @tok_a : dup\n"
                                       "  @tok_b : dup\n"
                                       "]\n";

TEST(test_peg_duplicate_tag_error) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, PEG_DUP_TAG_NEST);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

// --- Re-parse (reuse ParseState) ---

TEST(test_reparse_fresh_state) {
  ParseState* ps = parse_state_new();

  bool ok1 = parse_nest(ps, "garbage");
  assert(!ok1);
  assert(parse_has_error(ps));
  parse_state_del(ps);

  ps = parse_state_new();
  bool ok2 = parse_nest(ps, MINIMAL_NEST);
  if (!ok2) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok2);
  parse_state_del(ps);
}

// --- Bootstrap file ---

TEST(test_bootstrap_nest) {
  FILE* f = fopen("specs/bootstrap.nest", "r");
  if (!f) {
    f = fopen("../specs/bootstrap.nest", "r");
  }
  assert(f != NULL);

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);

  char* buf = malloc(sz + 1);
  assert(buf != NULL);
  size_t rd = fread(buf, 1, sz, f);
  buf[rd] = '\0';
  fclose(f);

  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, buf);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  assert(!parse_has_error(ps));

  parse_state_del(ps);
  free(buf);
}

// --- String literal in VPA ---

static const char STRLIT_NEST[] = "[[vpa]]\n"
                                  "%ignore @space @comment\n"
                                  "%state $last_quote\n"
                                  "main = { str *noise }\n"
                                  "str = /[\"']/ .begin { /[\"']/ .end /[^\"']+/ @char }\n"
                                  "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                  "[[peg]]\n"
                                  "main = @nl*\n";

TEST(test_string_literal_scope) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, STRLIT_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- %define with fragment reference ---

static const char DEFINE_REF_NEST[] = "[[vpa]]\n"
                                      "%ignore @space @comment\n"
                                      "%define ID /[a-z_]\\w*/\n"
                                      "main = { /\\{ID}/ @tok_a *noise }\n"
                                      "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                      "[[peg]]\n"
                                      "main = @tok_a*\n";

TEST(test_define_fragment) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, DEFINE_REF_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- VPA multiple token emissions ---

static const char MULTI_TOK_NEST[] = "[[vpa]]\n"
                                     "%ignore @space @comment\n"
                                     "main = { /[a-z]+/ @tok_a /[0-9]+/ @tok_b /[A-Z]+/ @tok_c *noise }\n"
                                     "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                     "[[peg]]\n"
                                     "main = @tok_a @tok_b? @tok_c?\n";

TEST(test_multiple_tokens) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, MULTI_TOK_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Case-insensitive regexp ---

static const char RE_IC_NEST[] = "[[vpa]]\n"
                                 "%ignore @space @comment\n"
                                 "main = { /i[a-z]+/ @tok_a *noise }\n"
                                 "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                 "[[peg]]\n"
                                 "main = @tok_a*\n";

TEST(test_case_insensitive_re) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, RE_IC_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- %effect directive ---

static const char EFFECT_NEST[] = "[[vpa]]\n"
                                  "%ignore @space @comment\n"
                                  "%effect .on_id = @tok_a\n"
                                  "main = { /[a-z]+/ .on_id *noise }\n"
                                  "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                  "[[peg]]\n"
                                  "main = @tok_a*\n";

TEST(test_effect_directive) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, EFFECT_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Nested scopes ---

static const char NESTED_SCOPE_NEST[] = "[[vpa]]\n"
                                        "%ignore @space @comment\n"
                                        "main = { outer *noise }\n"
                                        "outer = /\\(/ .begin { inner /\\)/ .end *noise }\n"
                                        "inner = /\\[/ .begin { /\\]/ .end *noise }\n"
                                        "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                        "[[peg]]\n"
                                        "main = @nl*\n";

TEST(test_nested_scopes) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, NESTED_SCOPE_NEST);
  if (!ok) {
    fprintf(stderr, "error: %s\n", parse_get_error(ps));
  }
  assert(ok);
  parse_state_del(ps);
}

// --- Invalid regexp ---

static const char BAD_RE_NEST[] = "[[vpa]]\n"
                                  "main = { // @tok_a }\n"
                                  "[[peg]]\n"
                                  "main = @tok_a\n";

TEST(test_invalid_empty_regexp) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, BAD_RE_NEST);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

// --- Unclosed scope ---

static const char UNCLOSED_SCOPE_NEST[] = "[[vpa]]\n"
                                          "main = { /a/ @tok_a\n"
                                          "[[peg]]\n"
                                          "main = @tok_a\n";

TEST(test_unclosed_scope) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, UNCLOSED_SCOPE_NEST);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

// --- Missing PEG main ---

static const char NO_PEG_MAIN_NEST[] = "[[vpa]]\n"
                                       "%ignore @space @comment\n"
                                       "main = { *noise }\n"
                                       "*noise = { /[ \\t\\n]+/ @space /#[^\\n]*/ @comment /\\n+/ @nl }\n"
                                       "[[peg]]\n"
                                       "helper = @nl*\n";

TEST(test_missing_peg_main) {
  ParseState* ps = parse_state_new();
  bool ok = parse_nest(ps, NO_PEG_MAIN_NEST);
  assert(!ok);
  assert(parse_has_error(ps));
  parse_state_del(ps);
}

int main(void) {
  printf("test_parse:\n");

  // lifecycle
  RUN(test_state_new_del);
  RUN(test_state_initial_no_error);

  // error helpers
  RUN(test_parse_error_sets_message);
  RUN(test_parse_error_truncates_long_message);

  // string helpers
  RUN(test_parse_sfmt);
  RUN(test_parse_set_str);

  // reject invalid input
  RUN(test_empty_input);
  RUN(test_null_src);
  RUN(test_garbage_input);
  RUN(test_vpa_only_no_peg);
  RUN(test_peg_only_no_vpa);

  // minimal valid
  RUN(test_minimal_parse);

  // VPA directives
  RUN(test_directives_state);
  RUN(test_keyword_expansion);
  RUN(test_macro_rule);
  RUN(test_effect_directive);
  RUN(test_define_fragment);

  // VPA scopes and hooks
  RUN(test_scope_rule);
  RUN(test_hooks);
  RUN(test_nested_scopes);
  RUN(test_string_literal_scope);

  // regexp patterns
  RUN(test_re_charclass);
  RUN(test_re_alternation);
  RUN(test_re_special_classes);
  RUN(test_case_insensitive_re);

  // PEG rules
  RUN(test_peg_branches);
  RUN(test_peg_tagged_branches);
  RUN(test_peg_multiplier_interlace);
  RUN(test_peg_optional);
  RUN(test_multiple_tokens);

  // PEG errors
  RUN(test_peg_duplicate_tag_error);
  RUN(test_missing_peg_main);

  // VPA errors
  RUN(test_invalid_empty_regexp);
  RUN(test_unclosed_scope);

  // reuse
  RUN(test_reparse_fresh_state);

  // bootstrap
  RUN(test_bootstrap_nest);

  printf("all ok\n");
  return 0;
}
