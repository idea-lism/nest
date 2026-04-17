#include "../src/darray.h"
#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/peg.h"
#include "../src/symtab.h"
#include "compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s...", #name);                                                                                          \
    name();                                                                                                            \
    printf(" OK\n");                                                                                                   \
  } while (0)

// Find matching '}' accounting for nesting.
static const char* _find_matching_brace(const char* open) {
  int depth = 1;
  for (const char* p = open + 1; *p; p++) {
    if (*p == '{') {
      depth++;
    } else if (*p == '}') {
      depth--;
      if (depth == 0) {
        return p;
      }
    }
  }
  return NULL;
}
// ============================================================
// Fixture helpers: build PegGenInput directly (no peg_analyze)
// ============================================================

// Minimal ScopedRule with a single TERM body.
static ScopedRule _make_term_rule(const char* scoped_name, int32_t term_id, int32_t original_gid, int32_t slot_idx) {
  ScopedRule sr = {0};
  sr.scoped_rule_name = scoped_name;
  sr.body = (ScopedUnit){.kind = SCOPED_UNIT_TERM, .as.term_id = term_id, .tag_bit_local_offset = -1};
  symtab_init(&sr.tags, 0);
  sr.original_global_id = original_gid;
  sr.slot_index = (uint64_t)slot_idx;
  sr.node_fields = NULL;
  return sr;
}

// Add a NodeField to a darray.
static void _add_field(NodeFields* out, const char* name, bool is_link, bool is_scope, int32_t ref_row, int32_t rhs_row,
                       NodeAdvanceKind advance, int32_t advance_slot_row) {
  NodeField nf = {
      .name = strdup(name),
      .is_link = is_link,
      .is_scope = is_scope,
      .ref_row = ref_row,
      .rhs_row = rhs_row,
      .advance = advance,
      .advance_slot_row = advance_slot_row,
  };
  darray_push(*out, nf);
}

// Free NodeFields darray.
static void _free_fields(NodeFields fields) {
  if (!fields) {
    return;
  }
  for (size_t i = 0; i < darray_size(fields); i++) {
    free(fields[i].name);
  }
  darray_del(fields);
}

// Free a ScopeClosure (tags symtabs + scoped_rules darray + node_fields).
static void _free_closure(ScopeClosure* cl) {
  for (size_t i = 0; i < darray_size(cl->scoped_rules); i++) {
    symtab_free(&cl->scoped_rules[i].tags);
    _free_fields(cl->scoped_rules[i].node_fields);
  }
  darray_del(cl->scoped_rules);
}

// Generate IR + header from a PegGenInput. Caller frees GenResult buffers.
typedef struct {
  char* ir_buf;
  size_t ir_len;
  char* hdr_buf;
  size_t hdr_len;
} GenResult;

static GenResult _gen(PegGenInput* input) {
  GenResult r = {0};
  FILE* ir_f = compat_open_memstream(&r.ir_buf, &r.ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");
  FILE* hdr_f = compat_open_memstream(&r.hdr_buf, &r.hdr_len);
  HeaderWriter* hw = hdwriter_new(hdr_f);

  peg_gen(input, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hdwriter_del(hw);
  fclose(hdr_f);
  return r;
}

static void _free_gen(GenResult* r) {
  free(r->ir_buf);
  free(r->hdr_buf);
}

// ============================================================
// Build a JSON-like grammar fixture as PegGenInput directly.
//
// Grammar (for reference):
//   main = value
//   value = [ @number : number  @string : string  array : array ]
//   array = @lbracket value_list @rbracket
//   value_list = value*<@comma>
//
// Scoped rules in closure "main":
//   main$main(0): CALL -> main$value
//   main$value(1): BRANCHES -> [TERM 1, TERM 2, CALL main$array]
//   main$array(2): SEQ -> [TERM 3, CALL main$value_list, TERM 4]
//   main$value_list(3): CALL -> main$value_list$1
//   main$value_list$1(4): STAR(CALL main$value, TERM 5)
// ============================================================

static void _build_json_fixture(PegGenInput* input, ScopeClosure* cl) {
  // rule_names symtab: needed by _build_scope_map and _gen_header
  symtab_init(&input->rule_names, 0);
  symtab_intern(&input->rule_names, "main");       // 0
  symtab_intern(&input->rule_names, "value");      // 1
  symtab_intern(&input->rule_names, "array");      // 2
  symtab_intern(&input->rule_names, "value_list"); // 3

  // these are not read by peg_gen but must be valid structs
  symtab_init(&input->tokens, 1);
  symtab_init(&input->scope_names, 0);

  // --- build scope closure ---
  cl->scope_name = "main";
  cl->scope_id = 0;
  cl->bits_bucket_size = 1; // 1 bucket for tag bits
  cl->slots_size = 5;       // 5 slots (naive: one per rule)
  cl->scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // Rule 0: main$main = CALL main$value
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$value", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "value", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl->scoped_rules, sr);
  }

  // Rule 1: main$value = BRANCHES [TERM 1, TERM 2, CALL main$array]
  {
    ScopedUnit* children = darray_new(sizeof(ScopedUnit), 0);
    ScopedUnit t1 = {.kind = SCOPED_UNIT_TERM, .as.term_id = 1, .tag_bit_local_offset = 0};
    ScopedUnit t2 = {.kind = SCOPED_UNIT_TERM, .as.term_id = 2, .tag_bit_local_offset = 1};
    ScopedUnit c3 = {.kind = SCOPED_UNIT_CALL, .as.callee = "main$array", .tag_bit_local_offset = 2};
    darray_push(children, t1);
    darray_push(children, t2);
    darray_push(children, c3);

    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$value";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_BRANCHES, .as.children = children, .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    symtab_intern(&sr.tags, "number"); // 0
    symtab_intern(&sr.tags, "string"); // 1
    symtab_intern(&sr.tags, "array");  // 2
    sr.original_global_id = 1;
    sr.slot_index = 1;
    sr.tag_bit_index = 0;
    sr.tag_bit_offset = 0;
    sr.tag_bit_mask = 0x7; // 3 bits
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "number", false, false, 0, -1, NODE_ADVANCE_NONE, 0);
    _add_field(&sr.node_fields, "string", false, false, 0, -1, NODE_ADVANCE_NONE, 0);
    _add_field(&sr.node_fields, "array", false, false, 2, -1, NODE_ADVANCE_NONE, 0);
    darray_push(cl->scoped_rules, sr);
  }

  // Rule 2: main$array = SEQ [TERM 3, CALL main$value_list, TERM 4]
  {
    ScopedUnit* children = darray_new(sizeof(ScopedUnit), 0);
    ScopedUnit t3 = {.kind = SCOPED_UNIT_TERM, .as.term_id = 3, .tag_bit_local_offset = -1};
    ScopedUnit cvl = {.kind = SCOPED_UNIT_CALL, .as.callee = "main$value_list", .tag_bit_local_offset = -1};
    ScopedUnit t4 = {.kind = SCOPED_UNIT_TERM, .as.term_id = 4, .tag_bit_local_offset = -1};
    darray_push(children, t3);
    darray_push(children, cvl);
    darray_push(children, t4);

    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$array";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_SEQ, .as.children = children, .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 2;
    sr.slot_index = 2;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "lbracket", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    _add_field(&sr.node_fields, "value_list", false, false, 3, -1, NODE_ADVANCE_SLOT, 3);
    _add_field(&sr.node_fields, "rbracket", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    darray_push(cl->scoped_rules, sr);
  }

  // Rule 3: main$value_list = CALL main$value_list$1
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$value_list";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$value_list$1", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 3;
    sr.slot_index = 3;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "value", true, false, 4, -1, NODE_ADVANCE_NONE, 0);
    darray_push(cl->scoped_rules, sr);
  }

  // Rule 4: main$value_list$1 = STAR(CALL main$value, TERM 5)
  {
    ScopedUnit* lhs = malloc(sizeof(ScopedUnit));
    *lhs = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$value", .tag_bit_local_offset = -1};
    ScopedUnit* rhs = malloc(sizeof(ScopedUnit));
    *rhs = (ScopedUnit){.kind = SCOPED_UNIT_TERM, .as.term_id = 5, .tag_bit_local_offset = -1};

    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$value_list$1";
    sr.body = (ScopedUnit){
        .kind = SCOPED_UNIT_STAR, .as.interlace = {lhs, rhs}, .tag_bit_local_offset = -1, .nullable = true};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = -1; // generated sub-rule
    sr.slot_index = 4;
    sr.node_fields = NULL;
    darray_push(cl->scoped_rules, sr);
  }

  input->scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input->scope_closures, *cl);
  input->memoize_mode = MEMOIZE_NAIVE;
  input->prefix = "json";
  input->verbose = 0;
}

static void _free_json_fixture(PegGenInput* input, ScopeClosure* cl) {
  // free heap-allocated ScopedUnit nodes in the star rule
  ScopedRule* star_sr = &cl->scoped_rules[4];
  free(star_sr->body.as.interlace.lhs);
  free(star_sr->body.as.interlace.rhs);
  // free darray children in branches and seq bodies
  darray_del(cl->scoped_rules[1].body.as.children);
  darray_del(cl->scoped_rules[2].body.as.children);
  _free_closure(cl);
  darray_del(input->scope_closures);
  symtab_free(&input->rule_names);
  symtab_free(&input->tokens);
  symtab_free(&input->scope_names);
}

// ============================================================
// Tests: IR generation
// ============================================================

TEST(test_ir_has_parse_function) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(g.ir_buf != NULL);
  assert(g.ir_len > 0);
  assert(strstr(g.ir_buf, "parse_main") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_ir_has_rule_labels) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(strstr(g.ir_buf, "main$main:") != NULL);
  assert(strstr(g.ir_buf, "main$value:") != NULL);
  assert(strstr(g.ir_buf, "main$array:") != NULL);
  assert(strstr(g.ir_buf, "main$value_list:") != NULL);
  assert(strstr(g.ir_buf, "main$value_list$1:") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_ir_call_targets) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // main$main should branch to main$value
  const char* br = strstr(g.ir_buf, "br label %main$value,");
  if (!br) {
    br = strstr(g.ir_buf, "br label %main$value\n");
  }
  assert(br != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_ir_memoization_naive) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // naive mode: should have icmp with -1 (cache check)
  const char* after_alloc = strstr(g.ir_buf, "tt_alloc_memoize_table");
  assert(after_alloc != NULL);

  int cache_check = 0;
  const char* p = after_alloc;
  while ((p = strstr(p, ", -1")) != NULL) {
    const char* line_start = p;
    while (line_start > after_alloc && *line_start != '\n') {
      line_start--;
    }
    if (strstr(line_start, "icmp") != NULL && strstr(line_start, "icmp") < p) {
      cache_check++;
    }
    p += 4;
  }
  assert(cache_check > 0);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_ir_shared_bit_ops) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  input.memoize_mode = MEMOIZE_SHARED;
  // set up shared-mode fields on rules
  for (size_t i = 0; i < darray_size(cl.scoped_rules); i++) {
    cl.scoped_rules[i].segment_index = 0;
    cl.scoped_rules[i].segment_mask = (1ULL << darray_size(cl.scoped_rules)) - 1;
    cl.scoped_rules[i].rule_bit_mask = 1ULL << i;
  }
  // rebuild scope_closures darray since we modified cl in-place
  darray_del(input.scope_closures);
  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);

  GenResult g = _gen(&input);

  const char* fn = strstr(g.ir_buf, "parse_main");
  assert(fn != NULL);
  bool has_bit_op =
      (strstr(fn, "@bit_test") != NULL || strstr(fn, "@bit_deny") != NULL || strstr(fn, "@bit_exclude") != NULL);
  assert(has_bit_op);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

// ============================================================
// Tests: header generation
// ============================================================

TEST(test_header_has_types) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(g.hdr_buf != NULL);
  assert(g.hdr_len > 0);
  assert(strstr(g.hdr_buf, "PegRef") != NULL);
  assert(strstr(g.hdr_buf, "PegLink") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_header_node_struct) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // Node_value should have tag bits and branch fields
  assert(strstr(g.hdr_buf, "Node_value") != NULL);
  assert(strstr(g.hdr_buf, "bool number : 1") != NULL);
  assert(strstr(g.hdr_buf, "bool string : 1") != NULL);
  assert(strstr(g.hdr_buf, "bool array : 1") != NULL);
  assert(strstr(g.hdr_buf, "PegRef number;") != NULL);
  assert(strstr(g.hdr_buf, "PegRef string;") != NULL);
  assert(strstr(g.hdr_buf, "PegRef array;") != NULL);

  // Node_array should have seq fields
  assert(strstr(g.hdr_buf, "Node_array") != NULL);
  assert(strstr(g.hdr_buf, "PegRef lbracket;") != NULL);
  assert(strstr(g.hdr_buf, "PegRef value_list;") != NULL);
  assert(strstr(g.hdr_buf, "PegRef rbracket;") != NULL);

  // Node_value_list should have a PegLink field
  assert(strstr(g.hdr_buf, "Node_value_list") != NULL);
  assert(strstr(g.hdr_buf, "PegLink value;") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_header_loader_exists) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(strstr(g.hdr_buf, "json_load_value") != NULL);
  assert(strstr(g.hdr_buf, "json_load_array") != NULL);
  assert(strstr(g.hdr_buf, "json_load_value_list") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_loader_decodes_from_table) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  const char* loader = strstr(g.hdr_buf, "json_load_value(PegRef ref)");
  assert(loader != NULL);
  const char* body_start = strchr(loader, '{');
  assert(body_start != NULL);
  const char* body_end = _find_matching_brace(body_start);
  assert(body_end != NULL);

  size_t body_len = (size_t)(body_end - body_start);
  char* body = malloc(body_len + 1);
  memcpy(body, body_start, body_len);
  body[body_len] = '\0';
  assert(strstr(body, "ref.tc->value") != NULL);
  free(body);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_loader_no_parse_calls) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  const char* p = g.hdr_buf;
  while ((p = strstr(p, "json_load_")) != NULL) {
    const char* body_start = strchr(p, '{');
    assert(body_start != NULL);
    const char* body_end = strchr(body_start, '}');
    assert(body_end != NULL);
    for (const char* q = body_start; q < body_end; q++) {
      if (strncmp(q, "parse_", 6) == 0) {
        fprintf(stderr, "FAIL: loader calls parse_ function\n");
        assert(0);
      }
    }
    p = body_end + 1;
  }

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_loader_sets_tag_bits) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  const char* loader = strstr(g.hdr_buf, "json_load_value(PegRef ref)");
  assert(loader != NULL);
  const char* body_start = strchr(loader, '{');
  const char* body_end = _find_matching_brace(body_start);
  size_t body_len = (size_t)(body_end - body_start);
  char* body = malloc(body_len + 1);
  memcpy(body, body_start, body_len);
  body[body_len] = '\0';

  bool sets_is = (strstr(body, "&$1.is)") != NULL);
  assert(sets_is);
  free(body);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_iteration_helpers) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(strstr(g.hdr_buf, "json_has_elem") != NULL);
  assert(strstr(g.hdr_buf, "json_get_next") != NULL);
  assert(strstr(g.hdr_buf, "json_get_lhs") != NULL);
  assert(strstr(g.hdr_buf, "json_get_rhs") != NULL);
  assert(strstr(g.hdr_buf, "darray_size(l->tc->tokens)") != NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

TEST(test_peg_size_helper) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  assert(strstr(g.hdr_buf, "json_peg_size") != NULL);
  assert(strstr(g.hdr_buf, "case 0:") != NULL); // scope_id 0

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

// ============================================================
// Tests: multi-scope loader
// ============================================================

TEST(test_multi_scope_loader) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "shared"); // 0
  symtab_intern(&input.rule_names, "s1");     // 1
  symtab_intern(&input.rule_names, "s2");     // 2
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  // Scope s1: shared(gid=0) + s1(gid=1)
  ScopeClosure cl1 = {.scope_name = "s1", .scope_id = 0, .bits_bucket_size = 0, .slots_size = 2};
  cl1.scoped_rules = darray_new(sizeof(ScopedRule), 0);
  {
    ScopedRule sr = _make_term_rule("s1$shared", 1, 0, 0);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "a", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    darray_push(cl1.scoped_rules, sr);
  }
  {
    ScopedRule sr = _make_term_rule("s1$s1", 1, 1, 1);
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "s1$shared", .tag_bit_local_offset = -1};
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "shared", false, false, 0, -1, NODE_ADVANCE_SLOT, 0);
    darray_push(cl1.scoped_rules, sr);
  }

  // Scope s2: shared(gid=0) + s2(gid=2)
  ScopeClosure cl2 = {.scope_name = "s2", .scope_id = 1, .bits_bucket_size = 0, .slots_size = 2};
  cl2.scoped_rules = darray_new(sizeof(ScopedRule), 0);
  {
    ScopedRule sr = _make_term_rule("s2$shared", 1, 0, 0);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "a", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    darray_push(cl2.scoped_rules, sr);
  }
  {
    ScopedRule sr = _make_term_rule("s2$s2", 1, 2, 1);
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "s2$shared", .tag_bit_local_offset = -1};
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "shared", false, false, 0, -1, NODE_ADVANCE_SLOT, 0);
    darray_push(cl2.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl1);
  darray_push(input.scope_closures, cl2);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "ms";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // shared appears in both scopes -> loader has switch with case 0 and case 1
  const char* loader = strstr(g.hdr_buf, "ms_load_shared");
  assert(loader != NULL);
  const char* body_start = strchr(loader, '{');
  assert(body_start != NULL);
  assert(strstr(body_start, "switch (ref.tc->scope_id)") != NULL);
  assert(strstr(body_start, "case 0:") != NULL);
  assert(strstr(body_start, "case 1:") != NULL);

  _free_gen(&g);
  _free_closure(&cl1);
  _free_closure(&cl2);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

// ============================================================
// Tests: field naming in generated node structs
// ============================================================

TEST(test_node_field_naming) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  ScopeClosure cl = {.scope_name = "main", .scope_id = 0, .bits_bucket_size = 0, .slots_size = 2};
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // foo: single term body
  {
    ScopedRule sr = _make_term_rule("main$foo", 1, 1, 1);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "_lit_while", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    _add_field(&sr.node_fields, "_lparen", false, false, 0, -1, NODE_ADVANCE_ONE, 0);
    darray_push(cl.scoped_rules, sr);
  }
  // main: scope entry
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$foo", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "foo", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "lang";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // Node_foo should have sanitized field names
  assert(strstr(g.hdr_buf, "PegRef _lit_while;") != NULL);
  assert(strstr(g.hdr_buf, "PegRef _lparen;") != NULL);
  // no raw @ or . in field names
  assert(strstr(g.hdr_buf, "PegRef @") == NULL);
  assert(strstr(g.hdr_buf, "bool @") == NULL);

  _free_gen(&g);
  _free_closure(&cl);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

// ============================================================
// Tests: link field generates PegLink, not PegRef
// ============================================================

TEST(test_link_field_type) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // Node_value_list should contain "PegLink value;"
  assert(strstr(g.hdr_buf, "Node_value_list") != NULL);
  // find the typedef closing line
  const char* close = strstr(g.hdr_buf, "} Node_value_list;");
  assert(close != NULL);
  // search backwards for "typedef struct"
  const char* open = close;
  while (open > g.hdr_buf && strncmp(open, "typedef struct", 14) != 0) {
    open--;
  }
  size_t span = (size_t)(close - open);
  char* chunk = malloc(span + 1);
  memcpy(chunk, open, span);
  chunk[span] = '\0';
  assert(strstr(chunk, "PegLink value;") != NULL);
  free(chunk);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

// ============================================================
// Tests: scope field generates aux_value dereference
// ============================================================

TEST(test_scope_field_aux_value) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  ScopeClosure cl = {.scope_name = "main", .scope_id = 0, .bits_bucket_size = 0, .slots_size = 2};
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // foo has a scope field
  {
    ScopedRule sr = _make_term_rule("main$foo", 1, 1, 1);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "inner", false, true, 0, -1, NODE_ADVANCE_ONE, 0); // is_scope=true
    darray_push(cl.scoped_rules, sr);
  }
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$foo", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "foo", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "test";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // scope field loader should use aux_value to dereference the child chunk
  const char* loader = strstr(g.hdr_buf, "test_load_foo(PegRef ref)");
  assert(loader != NULL);
  assert(strstr(loader, "aux_value") != NULL);

  _free_gen(&g);
  _free_closure(&cl);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

// ============================================================
// Tests: cursor advancement in loader
// ============================================================

TEST(test_loader_cursor_advance) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // array loader should have _cur++ (for terms) and slot-based advance (for call)
  const char* loader = strstr(g.hdr_buf, "json_load_array(PegRef ref)");
  assert(loader != NULL);
  const char* body_end = strstr(loader, "return $1;");
  assert(body_end != NULL);
  size_t span = (size_t)(body_end - loader);
  char* body = malloc(span + 1);
  memcpy(body, loader, span);
  body[span] = '\0';

  // should have _cur++ for term fields
  assert(strstr(body, "ref.col++") != NULL);
  // should have slot-based advance for value_list call
  assert(strstr(body, "((int32_t*)ref.tc->value)") != NULL);

  free(body);
  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

// ============================================================
// Tests: no node_fields -> no struct/loader emitted
// ============================================================

TEST(test_null_node_fields_skips_struct) {
  PegGenInput input = {0};
  ScopeClosure cl = {0};
  _build_json_fixture(&input, &cl);
  GenResult g = _gen(&input);

  // value_list$1 has node_fields=NULL (generated sub-rule) -> no Node_value_list$1 struct
  assert(strstr(g.hdr_buf, "Node_value_list$1") == NULL);

  _free_gen(&g);
  _free_json_fixture(&input, &cl);
}

// ============================================================
// Tests: interlaced link emits positive rhs_row
// ============================================================

TEST(test_interlaced_link_rhs_row) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  ScopeClosure cl = {.scope_name = "main", .scope_id = 0, .bits_bucket_size = 0, .slots_size = 3};
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // main$foo: body is a single term (used for IR), node_fields has interlaced link
  {
    ScopedRule sr = _make_term_rule("main$foo", 1, 1, 1);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "items", true, false, 2, 3, NODE_ADVANCE_NONE, 0); // rhs_row=3
    darray_push(cl.scoped_rules, sr);
  }
  // main$main: scope entry
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$foo", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "foo", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl.scoped_rules, sr);
  }
  // main$foo$1: star sub-rule (generated)
  {
    ScopedUnit* lhs = malloc(sizeof(ScopedUnit));
    *lhs = (ScopedUnit){.kind = SCOPED_UNIT_TERM, .as.term_id = 1, .tag_bit_local_offset = -1};
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$foo$1";
    sr.body = (ScopedUnit){
        .kind = SCOPED_UNIT_STAR, .as.interlace = {lhs, NULL}, .tag_bit_local_offset = -1, .nullable = true};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = -1;
    sr.slot_index = 2;
    sr.node_fields = NULL;
    darray_push(cl.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "il";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // loader for foo must emit rhs_row = 3
  const char* loader = strstr(g.hdr_buf, "il_load_foo(PegRef ref)");
  assert(loader != NULL);
  const char* body_end = strstr(loader, "return $1;");
  assert(body_end != NULL);
  size_t span = (size_t)(body_end - loader);
  char* body = malloc(span + 1);
  memcpy(body, loader, span);
  body[span] = '\0';

  assert(strstr(body, "$1.items.col_size_in_i32") != NULL);
  assert(strstr(body, "$1.items.rhs_row") != NULL);

  free(body);
  _free_gen(&g);
  free(cl.scoped_rules[2].body.as.interlace.lhs);
  _free_closure(&cl);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

// ============================================================
// Tests: scope + link field dereferences via aux_value with elem.tc
// ============================================================

TEST(test_scope_link_field) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "bar");  // 1
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  ScopeClosure cl = {.scope_name = "main", .scope_id = 0, .bits_bucket_size = 0, .slots_size = 2};
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // main$bar: body is a term, node_fields has is_link=true + is_scope=true
  {
    ScopedRule sr = _make_term_rule("main$bar", 1, 1, 1);
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "inner", true, true, 1, -1, NODE_ADVANCE_NONE, 0);
    darray_push(cl.scoped_rules, sr);
  }
  // main$main
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$bar", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "bar", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "sl";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // Node_bar should have PegLink inner (link field)
  assert(strstr(g.hdr_buf, "PegLink inner;") != NULL);

  // loader for bar: scope+link -> elem.tc via aux_value
  const char* loader = strstr(g.hdr_buf, "sl_load_bar(PegRef ref)");
  assert(loader != NULL);
  const char* body_end = strstr(loader, "return $1;");
  assert(body_end != NULL);
  size_t span = (size_t)(body_end - loader);
  char* body = malloc(span + 1);
  memcpy(body, loader, span);
  body[span] = '\0';

  // must use aux_value for scope dereference with elem.tc (not just .tc)
  assert(strstr(body, "$1.inner.tc") != NULL);
  assert(strstr(body, "aux_value") != NULL);

  free(body);
  _free_gen(&g);
  _free_closure(&cl);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

// ============================================================
// Tests: multi-bucket tags (tag_bit_index > 0)
// ============================================================

TEST(test_multi_bucket_tags) {
  PegGenInput input = {0};
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "r1");   // 1
  symtab_intern(&input.rule_names, "r2");   // 2
  symtab_init(&input.tokens, 1);
  symtab_init(&input.scope_names, 0);

  // Two rules each with 40 tags -> 80 total bits -> needs 2 buckets
  // r1 at bucket 0, r2 at bucket 1
  ScopeClosure cl = {.scope_name = "main", .scope_id = 0, .bits_bucket_size = 2, .slots_size = 3};
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  // main$main
  {
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$main";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_CALL, .as.callee = "main$r1", .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    sr.original_global_id = 0;
    sr.slot_index = 0;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    _add_field(&sr.node_fields, "r1", false, false, 1, -1, NODE_ADVANCE_SLOT, 1);
    darray_push(cl.scoped_rules, sr);
  }

  // main$r1: 40 tags, bucket 0 offset 0
  {
    ScopedUnit* children = darray_new(sizeof(ScopedUnit), 0);
    for (int32_t i = 0; i < 40; i++) {
      ScopedUnit t = {.kind = SCOPED_UNIT_TERM, .as.term_id = 1, .tag_bit_local_offset = i};
      darray_push(children, t);
    }
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$r1";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_BRANCHES, .as.children = children, .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    char tag_name[16];
    for (int32_t i = 0; i < 40; i++) {
      snprintf(tag_name, sizeof(tag_name), "t%d", i);
      symtab_intern(&sr.tags, tag_name);
    }
    sr.original_global_id = 1;
    sr.slot_index = 1;
    sr.tag_bit_index = 0;
    sr.tag_bit_offset = 0;
    sr.tag_bit_mask = (1ULL << 40) - 1;
    // node_fields: one field per branch
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    for (int32_t i = 0; i < 40; i++) {
      snprintf(tag_name, sizeof(tag_name), "t%d", i);
      _add_field(&sr.node_fields, tag_name, false, false, 0, -1, NODE_ADVANCE_NONE, 0);
    }
    darray_push(cl.scoped_rules, sr);
  }

  // main$r2: 40 tags, bucket 1 offset 0
  {
    ScopedUnit* children = darray_new(sizeof(ScopedUnit), 0);
    for (int32_t i = 0; i < 40; i++) {
      ScopedUnit t = {.kind = SCOPED_UNIT_TERM, .as.term_id = 1, .tag_bit_local_offset = i};
      darray_push(children, t);
    }
    ScopedRule sr = {0};
    sr.scoped_rule_name = "main$r2";
    sr.body = (ScopedUnit){.kind = SCOPED_UNIT_BRANCHES, .as.children = children, .tag_bit_local_offset = -1};
    symtab_init(&sr.tags, 0);
    char tag_name[16];
    for (int32_t i = 0; i < 40; i++) {
      snprintf(tag_name, sizeof(tag_name), "u%d", i);
      symtab_intern(&sr.tags, tag_name);
    }
    sr.original_global_id = 2;
    sr.slot_index = 2;
    sr.tag_bit_index = 1; // second bucket
    sr.tag_bit_offset = 0;
    sr.tag_bit_mask = (1ULL << 40) - 1;
    sr.node_fields = darray_new(sizeof(NodeField), 0);
    for (int32_t i = 0; i < 40; i++) {
      snprintf(tag_name, sizeof(tag_name), "u%d", i);
      _add_field(&sr.node_fields, tag_name, false, false, 0, -1, NODE_ADVANCE_NONE, 0);
    }
    darray_push(cl.scoped_rules, sr);
  }

  input.scope_closures = darray_new(sizeof(ScopeClosure), 0);
  darray_push(input.scope_closures, cl);
  input.memoize_mode = MEMOIZE_NAIVE;
  input.prefix = "mb";
  input.verbose = 0;

  GenResult g = _gen(&input);

  // r1 loader should read col[0] (tag_bit_index=0)
  const char* loader_r1 = strstr(g.hdr_buf, "mb_load_r1(PegRef ref)");
  assert(loader_r1 != NULL);
  const char* r1_end = strstr(loader_r1, "return $1;");
  size_t r1_span = (size_t)(r1_end - loader_r1);
  char* r1_body = malloc(r1_span + 1);
  memcpy(r1_body, loader_r1, r1_span);
  r1_body[r1_span] = '\0';
  assert(strstr(r1_body, "$col[0]") != NULL);

  // r2 loader should read col[1] (tag_bit_index=1)
  const char* loader_r2 = strstr(g.hdr_buf, "mb_load_r2(PegRef ref)");
  assert(loader_r2 != NULL);
  const char* r2_end = strstr(loader_r2, "return $1;");
  size_t r2_span = (size_t)(r2_end - loader_r2);
  char* r2_body = malloc(r2_span + 1);
  memcpy(r2_body, loader_r2, r2_span);
  r2_body[r2_span] = '\0';
  assert(strstr(r2_body, "$col[1]") != NULL);

  free(r1_body);
  free(r2_body);
  _free_gen(&g);
  darray_del(cl.scoped_rules[1].body.as.children);
  darray_del(cl.scoped_rules[2].body.as.children);
  _free_closure(&cl);
  darray_del(input.scope_closures);
  symtab_free(&input.rule_names);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
}

int main(void) {
  printf("test_peg_gen:\n");
  RUN(test_ir_has_parse_function);
  RUN(test_ir_has_rule_labels);
  RUN(test_ir_call_targets);
  RUN(test_ir_memoization_naive);
  RUN(test_ir_shared_bit_ops);
  RUN(test_header_has_types);
  RUN(test_header_node_struct);
  RUN(test_header_loader_exists);
  RUN(test_loader_decodes_from_table);
  RUN(test_loader_no_parse_calls);
  RUN(test_loader_sets_tag_bits);
  RUN(test_iteration_helpers);
  RUN(test_peg_size_helper);
  RUN(test_multi_scope_loader);
  RUN(test_node_field_naming);
  RUN(test_link_field_type);
  RUN(test_scope_field_aux_value);
  RUN(test_loader_cursor_advance);
  RUN(test_null_node_fields_skips_struct);
  RUN(test_interlaced_link_rhs_row);
  RUN(test_scope_link_field);
  RUN(test_multi_bucket_tags);
  printf("test_peg_gen: OK\n");
  return 0;
}
