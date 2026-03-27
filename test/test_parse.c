#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "compat.h"

#include <assert.h>
#include <stdint.h>
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

void parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer);

#define TARGET "arm64-apple-macosx14.0.0"

// Test source exercising scopes, leader inlining, hooks, keywords, and macros.
// Mirrors bootstrap.nest structure without %state (not yet parseable in scope bodies).
static const char* TEST_SRC =
    "[[vpa]]\n"
    "\n"
    "%ignore @comment @space\n"
    "\n"
    "%keyword directives \"%state\" \"%ignore\" \"%effect\" \"%keyword\"\n"
    "%keyword ops \"=\" \"|\"\n"
    "\n"
    "main = {\n"
    "  vpa\n"
    "  peg\n"
    "  *ignores\n"
    "}\n"
    "\n"
    "*ignores = {\n"
    "  /#[^\\n]*/ @comment\n"
    "  /[ \\t\\n]+/ @space\n"
    "  /\\n+/ @nl\n"
    "}\n"
    "\n"
    "vpa = /\\s*\\[\\[vpa\\]\\]\\n/ .begin {\n"
    "  /\\[\\[peg\\]\\]/ .unparse .end\n"
    "  re\n"
    "  id @vpa_id\n"
    "  /\\.begin/ @hook_begin\n"
    "  /\\.end/ @hook_end\n"
    "  /=/ @assign\n"
    "  directives\n"
    "  ops\n"
    "  /{/ @scope_begin\n"
    "  /}/ @scope_end\n"
    "  *ignores\n"
    "}\n"
    "\n"
    "re = /(b|i|ib|bi)?\\//"
    " .begin {\n"
    "  /\\// .end\n"
    "  charclass\n"
    "  /\\./ @re_dot\n"
    "  /\\\\s/ @re_space_class\n"
    "  /\\\\w/ @re_word_class\n"
    "  /./ @char\n"
    "}\n"
    "\n"
    "charclass = /\\[\\^?/ .begin {\n"
    "  /\\]/ @class_end .end\n"
    "  /.-/ @range_start\n"
    "  /./ @char\n"
    "}\n"
    "\n"
    "id = /[a-z_]\\w*/\n"
    "\n"
    "peg = /\\s*\\[\\[peg\\]\\]\\n/ .begin {\n"
    "  /[a-z_]\\w*/ @id\n"
    "  /=/ @assign\n"
    "  *ignores\n"
    "}\n"
    "\n"
    "[[peg]]\n"
    "\n"
    "main = vpa peg\n"
    "\n"
    "vpa = @nl* rule+<@nl> @nl*\n"
    "\n"
    "rule = [\n"
    "  vpa_rule\n"
    "]\n"
    "\n"
    "vpa_rule = @vpa_id \"=\" @vpa_id\n"
    "\n";

static void _gen_output(const char* src, char** hdr_out, size_t* hdr_sz_out, char** ir_out, size_t* ir_sz_out) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_sz);
  assert(hdr_f);

  char* ir_buf = NULL;
  size_t ir_sz = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_sz);
  assert(ir_f);

  HeaderWriter* hw = hw_new(hdr_f);
  IrWriter* w = irwriter_new(ir_f, TARGET);
  irwriter_start(w, "test.nest", ".");

  parse_nest(src, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);

  compat_close_memstream(hdr_f, &hdr_buf, &hdr_sz);
  compat_close_memstream(ir_f, &ir_buf, &ir_sz);

  *hdr_out = hdr_buf;
  *hdr_sz_out = hdr_sz;
  *ir_out = ir_buf;
  *ir_sz_out = ir_sz;
}

TEST(test_parse_basic) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  assert(hdr_buf != NULL);
  assert(hdr_sz > 0);
  assert(ir_buf != NULL);
  assert(ir_sz > 0);

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_scopes) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // DFA functions for all scopes
  assert(strstr(ir_buf, "@lex_main("));
  assert(strstr(ir_buf, "@lex_vpa("));
  assert(strstr(ir_buf, "@lex_re("));
  assert(strstr(ir_buf, "@lex_charclass("));
  assert(strstr(ir_buf, "@lex_peg("));

  // Action dispatch and lex loop
  assert(strstr(ir_buf, "@vpa_dispatch("));
  assert(strstr(ir_buf, "@vpa_lex("));

  // Scope IDs in header
  assert(strstr(hdr_buf, "SCOPE_MAIN"));
  assert(strstr(hdr_buf, "SCOPE_VPA"));
  assert(strstr(hdr_buf, "SCOPE_RE"));
  assert(strstr(hdr_buf, "SCOPE_CHARCLASS"));
  assert(strstr(hdr_buf, "SCOPE_PEG"));

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_leader_inlining) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // vpa_dispatch should push scopes (leader inlining creates SCOPE_ENTER actions)
  assert(strstr(ir_buf, "call void @vpa_rt_push_scope"));

  // vpa_dispatch should pop scopes (.end hooks create SCOPE_EXIT actions)
  assert(strstr(ir_buf, "call void @vpa_rt_pop_scope"));

  // Token emission
  assert(strstr(ir_buf, "call void @vpa_rt_emit_token"));

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_tokens) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // Token IDs from the test source
  assert(strstr(hdr_buf, "TOK_COMMENT"));
  assert(strstr(hdr_buf, "TOK_SPACE"));
  assert(strstr(hdr_buf, "TOK_VPA_ID"));
  assert(strstr(hdr_buf, "TOK_ASSIGN"));
  assert(strstr(hdr_buf, "TOK_RE_DOT"));
  assert(strstr(hdr_buf, "TOK_CLASS_END"));
  assert(strstr(hdr_buf, "TOK_CHAR"));

  // Keyword tokens
  assert(strstr(hdr_buf, "TOK_DIRECTIVES_"));
  assert(strstr(hdr_buf, "TOK_OPS_"));

  free(hdr_buf);
  free(ir_buf);
}

TEST(test_vpa_ir_compiles) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  char* ir_buf = NULL;
  size_t ir_sz = 0;
  _gen_output(TEST_SRC, &hdr_buf, &hdr_sz, &ir_buf, &ir_sz);

  // Truncate before PEG functions (PEG may have forward refs that fail standalone)
  char* peg_start = strstr(ir_buf, "define i32 @parse_");
  if (peg_start) {
    *peg_start = '\0';
  }

  char ll_path[256];
  char obj_path[256];
  snprintf(ll_path, sizeof(ll_path), "%s/test_vpa_compile.ll", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_vpa_compile.o", BUILD_DIR);
  FILE* f = fopen(ll_path, "w");
  assert(f);
  fputs(ir_buf, f);
  fclose(f);

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -c -o %s %s 2>&1", compat_llvm_cc(), obj_path, ll_path);
  int rc = system(cmd);
  if (rc != 0) {
    fprintf(stderr, "clang compilation failed for %s\n", ll_path);
  }
  assert(rc == 0);

  remove(obj_path);
  remove(ll_path);
  free(hdr_buf);
  free(ir_buf);
}

int main(void) {
  printf("test_parse:\n");

  RUN(test_parse_basic);
  RUN(test_vpa_scopes);
  RUN(test_vpa_leader_inlining);
  RUN(test_vpa_tokens);
  RUN(test_vpa_ir_compiles);

  printf("all ok\n");
  return 0;
}
