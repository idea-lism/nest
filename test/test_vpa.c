// Unit tests for src/vpa.c — VPA code generation.
// Tests vpa_gen() by constructing VpaRule data directly and inspecting the
// generated C header and LLVM IR output.

#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "../src/re_ast.h"
#include "../src/vpa.h"
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

#define TARGET "arm64-apple-macosx14.0.0"

// --- Helpers ---

static void _gen(VpaGenInput* input, char** hdr_out, size_t* hdr_sz, char** ir_out, size_t* ir_sz) {
  char* hdr_buf = NULL;
  size_t hsz = 0;
  FILE* hf = compat_open_memstream(&hdr_buf, &hsz);
  assert(hf);

  char* ir_buf = NULL;
  size_t isz = 0;
  FILE* irf = compat_open_memstream(&ir_buf, &isz);
  assert(irf);

  HeaderWriter* hw = hw_new(hf);
  IrWriter* w = irwriter_new(irf, TARGET);
  irwriter_start(w, "test_vpa.nest", ".");

  vpa_gen(input, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);

  compat_close_memstream(hf, &hdr_buf, &hsz);
  compat_close_memstream(irf, &ir_buf, &isz);

  *hdr_out = hdr_buf;
  *hdr_sz = hsz;
  *ir_out = ir_buf;
  *ir_sz = isz;
}

static VpaUnit _make_regexp_unit(const char* pattern, const char* tok_name) {
  VpaUnit u = {0};
  u.kind = VPA_REGEXP;
  u.re_ast = re_ast_build_literal(pattern, 0, (int32_t)strlen(pattern));
  u.name = tok_name ? strdup(tok_name) : NULL;
  return u;
}

static void _free_vpa_unit(VpaUnit* u) {
  if (u->re_ast) {
    re_ast_free(u->re_ast);
    free(u->re_ast);
  }
  free(u->name);
  free(u->state_name);
  free(u->user_hook);
  for (int32_t i = 0; i < (int32_t)darray_size(u->children); i++) {
    _free_vpa_unit(&u->children[i]);
  }
  darray_del(u->children);
}

static void _free_rules(VpaRule* rules) {
  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    free(rules[i].name);
    for (int32_t j = 0; j < (int32_t)darray_size(rules[i].units); j++) {
      _free_vpa_unit(&rules[i].units[j]);
    }
    darray_del(rules[i].units);
  }
  darray_del(rules);
}

// --- Tests ---

// Single bare scope: main = { /x/ @tok_x  /y/ @tok_y }
TEST(test_single_scope) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_x"));
  darray_push(main_rule.units, _make_regexp_unit("y", "tok_y"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  assert(hsz > 0);
  assert(isz > 0);

  // Header: runtime types
  assert(strstr(hdr, "VpaToken"));
  assert(strstr(hdr, "TokenChunk"));
  assert(strstr(hdr, "TokenTree"));
  assert(strstr(hdr, "ChunkTable"));

  // Header: runtime helpers
  assert(strstr(hdr, "tt_new"));
  assert(strstr(hdr, "tt_del"));
  assert(strstr(hdr, "tt_alloc_chunk"));
  assert(strstr(hdr, "tt_add_token"));

  // Header: scope ID
  assert(strstr(hdr, "SCOPE_MAIN"));

  // Header: token IDs
  assert(strstr(hdr, "TOK_TOK_X"));
  assert(strstr(hdr, "TOK_TOK_Y"));

  // Header: lex declaration
  assert(strstr(hdr, "lex_main"));

  // IR: DFA function for main scope
  assert(strstr(ir, "@lex_main("));

  // IR: dispatch + lex loop
  assert(strstr(ir, "@vpa_dispatch("));
  assert(strstr(ir, "@vpa_lex("));

  // IR: dispatch emits tokens
  assert(strstr(ir, "call void @vpa_rt_emit_token"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Two scopes: main = { inner }  inner = /\{/ .begin { /\}/ .end  /a/ @tok_a }
TEST(test_multi_scope) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  // Build inner scope body: /}/ .end, /a/ @tok_a
  VpaUnit* inner_body = darray_new(sizeof(VpaUnit), 0);
  VpaUnit end_unit = _make_regexp_unit("}", NULL);
  end_unit.hook = TOK_HOOK_END;
  darray_push(inner_body, end_unit);
  darray_push(inner_body, _make_regexp_unit("a", "tok_a"));

  // inner rule: non-scope rule with leader+body
  // leader = /\{/, body = inner_body, stored in a VPA_SCOPE unit
  VpaRule inner_rule = {.name = strdup("inner"), .units = darray_new(sizeof(VpaUnit), 0)};
  VpaUnit scope_unit = {0};
  scope_unit.kind = VPA_SCOPE;
  scope_unit.re_ast = re_ast_build_literal("{", 0, 1);
  scope_unit.hook = TOK_HOOK_BEGIN;
  scope_unit.children = inner_body;
  darray_push(inner_rule.units, scope_unit);
  darray_push(rules, inner_rule);

  // main = { inner }
  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  VpaUnit ref_unit = {.kind = VPA_REF, .name = strdup("inner")};
  darray_push(main_rule.units, ref_unit);
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  // Two scope IDs
  assert(strstr(hdr, "SCOPE_INNER"));
  assert(strstr(hdr, "SCOPE_MAIN"));

  // Two DFA functions
  assert(strstr(ir, "@lex_main("));
  assert(strstr(ir, "@lex_inner("));

  // Dispatch: scope enter + scope exit + emit token
  assert(strstr(ir, "call void @vpa_rt_push_scope"));
  assert(strstr(ir, "call void @vpa_rt_pop_scope"));
  assert(strstr(ir, "call void @vpa_rt_emit_token"));

  // Token for inner scope
  assert(strstr(hdr, "TOK_TOK_A"));

  // Lex loop switches on scope
  assert(strstr(ir, "switch i32 %scope"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Keywords are pre-registered as tokens before scope processing
TEST(test_keywords) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_x"));
  darray_push(rules, main_rule);

  KeywordEntry* keywords = darray_new(sizeof(KeywordEntry), 0);
  const char* kw_src = "if";
  darray_push(keywords, ((KeywordEntry){.group = strdup("kw"), .lit_off = 0, .lit_len = 2, .src = kw_src}));

  VpaGenInput input = {.rules = rules, .keywords = keywords, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  // Keyword token appears in header
  assert(strstr(hdr, "TOK_KW_IF"));

  free(hdr);
  free(ir);
  free(keywords[0].group);
  darray_del(keywords);
  _free_rules(rules);
}

// Action count and scope count metadata
TEST(test_metadata) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, _make_regexp_unit("a", "tok_a"));
  darray_push(main_rule.units, _make_regexp_unit("b", "tok_b"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  assert(strstr(hdr, "VPA_N_ACTIONS"));
  assert(strstr(hdr, "VPA_N_SCOPES 1"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Token dedup: same name used twice should produce one token ID
TEST(test_token_dedup) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_a"));
  darray_push(main_rule.units, _make_regexp_unit("y", "tok_a"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  // TOK_TOK_A should appear exactly once as a #define
  const char* first = strstr(hdr, "#define TOK_TOK_A");
  assert(first);
  const char* second = strstr(first + 1, "#define TOK_TOK_A");
  assert(!second);

  // Only 1 action since the token is deduped
  assert(strstr(hdr, "VPA_N_ACTIONS 1"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Generated IR should compile with clang
TEST(test_ir_compiles) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  // inner scope
  VpaUnit* inner_body = darray_new(sizeof(VpaUnit), 0);
  VpaUnit end_u = _make_regexp_unit("}", NULL);
  end_u.hook = TOK_HOOK_END;
  darray_push(inner_body, end_u);
  darray_push(inner_body, _make_regexp_unit("a", "tok_a"));

  VpaRule inner_rule = {.name = strdup("inner"), .units = darray_new(sizeof(VpaUnit), 0)};
  VpaUnit su = {0};
  su.kind = VPA_SCOPE;
  su.re_ast = re_ast_build_literal("{", 0, 1);
  su.hook = TOK_HOOK_BEGIN;
  su.children = inner_body;
  darray_push(inner_rule.units, su);
  darray_push(rules, inner_rule);

  // main scope
  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, ((VpaUnit){.kind = VPA_REF, .name = strdup("inner")}));
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_x"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  char ll_path[256];
  char obj_path[256];
  snprintf(ll_path, sizeof(ll_path), "%s/test_vpa_compile.ll", BUILD_DIR);
  snprintf(obj_path, sizeof(obj_path), "%s/test_vpa_compile.o", BUILD_DIR);
  FILE* f = fopen(ll_path, "w");
  assert(f);
  fputs(ir, f);
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
  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Macro rules are skipped during scope collection
TEST(test_macro_skip) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule macro_rule = {
      .name = strdup("ignores"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true, .is_macro = true};
  darray_push(macro_rule.units, _make_regexp_unit(" ", "space"));
  darray_push(rules, macro_rule);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_x"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  // Macro scope should not appear
  assert(!strstr(hdr, "SCOPE_IGNORES"));
  // Only main scope
  assert(strstr(hdr, "VPA_N_SCOPES 1"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

// Ref inlining: main = { id }  id = /[a-z]+/
// The ref's regexp should be inlined into main's DFA
TEST(test_ref_inlining) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  // id = /abc/ (simple literal for deterministic test)
  VpaRule id_rule = {.name = strdup("id"), .units = darray_new(sizeof(VpaUnit), 0)};
  darray_push(id_rule.units, _make_regexp_unit("abc", NULL));
  darray_push(rules, id_rule);

  // main = { id }
  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, ((VpaUnit){.kind = VPA_REF, .name = strdup("id")}));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  // id is not a scope, so no SCOPE_ID or lex_id
  assert(!strstr(hdr, "SCOPE_ID"));
  assert(!strstr(ir, "@lex_id("));

  // id's regexp is inlined into main's DFA — check main DFA exists
  assert(strstr(ir, "@lex_main("));

  // The ref rule's name is used as the token name (since it has no explicit @tok)
  assert(strstr(hdr, "TOK_ID"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

TEST(test_user_hook_callback) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  VpaUnit hook_unit = _make_regexp_unit("x", "tok_x");
  hook_unit.user_hook = strdup(".mark");
  darray_push(main_rule.units, hook_unit);
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  assert(strstr(hdr, "void vpa_hook_mark(void* tt, int32_t cp_start, int32_t cp_size);"));
  assert(strstr(ir, "declare void @vpa_hook_mark(ptr, i32, i32)"));
  assert(strstr(ir, "call void @vpa_hook_mark(ptr %tt, i32 %cp_start, i32 %cp_size)"));

  free(hdr);
  free(ir);
  _free_rules(rules);
}

TEST(test_state_matcher_codegen) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);
  StateDecl* states = darray_new(sizeof(StateDecl), 0);
  darray_push(states, ((StateDecl){.name = strdup("ident")}));

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  VpaUnit state_unit = {.kind = VPA_STATE, .state_name = strdup("ident"), .name = strdup("tok_ident")};
  darray_push(main_rule.units, state_unit);
  darray_push(main_rule.units, _make_regexp_unit("x", "tok_x"));
  darray_push(rules, main_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .states = states, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  assert(strstr(hdr, "int32_t match_ident(void* src, int32_t cp_off, void* tt);"));
  assert(strstr(ir, "declare i32 @match_ident(ptr, i32, ptr)"));
  assert(strstr(ir, "define {i32, i32} @state_main(ptr %src, i32 %cp_off, ptr %tt)"));
  assert(strstr(ir, "call {i32, i32} @state_main(ptr %src, i32 %sm_off_in, ptr %tt)"));
  assert(strstr(ir, "call i32 @match_ident(ptr %src, i32 %cp_off, ptr %tt)"));

  free(hdr);
  free(ir);
  free(states[0].name);
  darray_del(states);
  _free_rules(rules);
}

TEST(test_parse_scope_callback) {
  VpaRule* rules = darray_new(sizeof(VpaRule), 0);
  PegRule* peg_rules = darray_new(sizeof(PegRule), 0);

  VpaUnit* inner_body = darray_new(sizeof(VpaUnit), 0);
  VpaUnit end_unit = _make_regexp_unit("}", NULL);
  end_unit.hook = TOK_HOOK_END;
  darray_push(inner_body, end_unit);

  VpaRule inner_rule = {.name = strdup("inner"), .units = darray_new(sizeof(VpaUnit), 0)};
  VpaUnit scope_unit = {0};
  scope_unit.kind = VPA_SCOPE;
  scope_unit.re_ast = re_ast_build_literal("{", 0, 1);
  scope_unit.hook = TOK_HOOK_BEGIN;
  scope_unit.children = inner_body;
  darray_push(inner_rule.units, scope_unit);
  darray_push(rules, inner_rule);

  VpaRule main_rule = {.name = strdup("main"), .units = darray_new(sizeof(VpaUnit), 0), .is_scope = true};
  darray_push(main_rule.units, ((VpaUnit){.kind = VPA_REF, .name = strdup("inner")}));
  darray_push(rules, main_rule);

  PegRule peg_rule = {.name = strdup("inner"), .seq = {.kind = PEG_SEQ}, .scope = strdup("inner")};
  darray_push(peg_rules, peg_rule);

  VpaGenInput input = {.rules = rules, .keywords = NULL, .peg_rules = peg_rules, .src = ""};

  char* hdr = NULL;
  size_t hsz = 0;
  char* ir = NULL;
  size_t isz = 0;
  _gen(&input, &hdr, &hsz, &ir, &isz);

  assert(strstr(hdr, "void vpa_parse_inner(void* tt);"));
  assert(strstr(ir, "declare void @vpa_parse_inner(ptr)"));
  assert(strstr(ir, "call void @vpa_parse_inner(ptr %tt)"));
  assert(strstr(ir, "call void @vpa_rt_pop_scope(ptr %tt)"));

  free(hdr);
  free(ir);
  free(peg_rules[0].name);
  free(peg_rules[0].scope);
  darray_del(peg_rules[0].seq.children);
  darray_del(peg_rules);
  _free_rules(rules);
}

int main(void) {
  printf("test_vpa:\n");

  RUN(test_single_scope);
  RUN(test_multi_scope);
  RUN(test_keywords);
  RUN(test_metadata);
  RUN(test_token_dedup);
  RUN(test_ir_compiles);
  RUN(test_macro_skip);
  RUN(test_ref_inlining);
  RUN(test_user_hook_callback);
  RUN(test_state_matcher_codegen);
  RUN(test_parse_scope_callback);

  printf("all ok\n");
  return 0;
}
