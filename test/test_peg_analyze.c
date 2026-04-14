#include "../src/darray.h"
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

// ============================================================
// Helpers
// ============================================================

static void _free_unit(PegUnit* u) {
  free(u->tag);
  if (u->children) {
    for (int32_t i = 0; i < (int32_t)darray_size(u->children); i++) {
      _free_unit(&u->children[i]);
    }
    darray_del(u->children);
  }
}

static void _free_input(PegAnalyzeInput* input) {
  for (int32_t i = 0; i < (int32_t)darray_size(input->rules); i++) {
    _free_unit(&input->rules[i].body);
  }
  darray_del(input->rules);
  symtab_free(&input->tokens);
  symtab_free(&input->scope_names);
  symtab_free(&input->rule_names);
}

static void _build_json_input(PegAnalyzeInput* input) {
  symtab_init(&input->tokens, 1);
  symtab_intern(&input->tokens, "number");   // 1
  symtab_intern(&input->tokens, "string");   // 2
  symtab_intern(&input->tokens, "lbracket"); // 3
  symtab_intern(&input->tokens, "rbracket"); // 4
  symtab_intern(&input->tokens, "comma");    // 5

  symtab_init(&input->scope_names, 0);
  symtab_intern(&input->scope_names, "main"); // 0

  symtab_init(&input->rule_names, 0);
  symtab_intern(&input->rule_names, "main");       // 0
  symtab_intern(&input->rule_names, "value");      // 1
  symtab_intern(&input->rule_names, "array");      // 2
  symtab_intern(&input->rule_names, "value_list"); // 3

  input->rules = darray_new(sizeof(PegRule), 0);

  // value = [ @number  @string  array ]
  PegUnit value_branches = {.kind = PEG_BRANCHES};
  value_branches.children = darray_new(sizeof(PegUnit), 0);
  PegUnit br_num = {.kind = PEG_TERM, .id = 1, .tag = strdup("number")};
  PegUnit br_str = {.kind = PEG_TERM, .id = 2, .tag = strdup("string")};
  PegUnit br_arr = {.kind = PEG_CALL, .id = 2, .tag = strdup("array")};
  darray_push(value_branches.children, br_num);
  darray_push(value_branches.children, br_str);
  darray_push(value_branches.children, br_arr);

  PegUnit value_seq = {.kind = PEG_SEQ};
  value_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(value_seq.children, value_branches);

  PegRule value_rule = {.global_id = 1, .scope_id = -1, .body = value_seq};
  darray_push(input->rules, value_rule);

  // array = @lbracket value_list @rbracket
  PegUnit arr_seq = {.kind = PEG_SEQ};
  arr_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit lb = {.kind = PEG_TERM, .id = 3};
  PegUnit vl = {.kind = PEG_CALL, .id = 3};
  PegUnit rb = {.kind = PEG_TERM, .id = 4};
  darray_push(arr_seq.children, lb);
  darray_push(arr_seq.children, vl);
  darray_push(arr_seq.children, rb);

  PegRule arr_rule = {.global_id = 2, .scope_id = -1, .body = arr_seq};
  darray_push(input->rules, arr_rule);

  // value_list = value*<@comma>
  PegUnit vl_elem = {
      .kind = PEG_CALL, .id = 1, .multiplier = '*', .interlace_rhs_kind = PEG_TERM, .interlace_rhs_id = 5};
  PegUnit vl_seq = {.kind = PEG_SEQ};
  vl_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(vl_seq.children, vl_elem);

  PegRule vl_rule = {.global_id = 3, .scope_id = -1, .body = vl_seq};
  darray_push(input->rules, vl_rule);

  // main = value (scope rule)
  PegUnit main_call = {.kind = PEG_CALL, .id = 1};
  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(main_seq.children, main_call);

  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input->rules, main_rule);
}

// ============================================================
// Tests: scope closure gathering
// ============================================================

TEST(test_scope_closure_count) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");

  // one scope (main)
  assert(darray_size(result.scope_closures) == 1);
  ScopeClosure* cl = &result.scope_closures[0];
  assert(strcmp(cl->scope_name, "main") == 0);
  assert(cl->scope_id == 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_scoped_rules_breakdown) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // should have scoped rules: main, value, array, value_list, plus sub-rules from multipliers
  int32_t rule_count = (int32_t)darray_size(cl->scoped_rules);
  assert(rule_count >= 4); // at least 4 original rules

  // check named rules exist
  int32_t main_id = symtab_find(&cl->scoped_rule_names, "main$main");
  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  int32_t array_id = symtab_find(&cl->scoped_rule_names, "main$array");
  int32_t vl_id = symtab_find(&cl->scoped_rule_names, "main$value_list");
  assert(main_id >= 0);
  assert(value_id >= 0);
  assert(array_id >= 0);
  assert(vl_id >= 0);

  // value rule should have original_global_id == 1
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];
  assert(value_sr->original_global_id == 1);

  // value_list's multiplier should create a sub-rule (main$value_list$1)
  int32_t vl_sub = symtab_find(&cl->scoped_rule_names, "main$value_list$1");
  assert(vl_sub >= 0);
  ScopedRule* vl_sub_sr = &cl->scoped_rules[vl_sub - cl->scoped_rule_names.start_num];
  assert(vl_sub_sr->original_global_id == -1); // generated sub-rule

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_branches_have_tags) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];

  // value has 3 tags: number, string, array
  int32_t tag_count = symtab_count(&value_sr->tags);
  assert(tag_count == 3);
  assert(symtab_find(&value_sr->tags, "number") >= 0);
  assert(symtab_find(&value_sr->tags, "string") >= 0);
  assert(symtab_find(&value_sr->tags, "array") >= 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: tag bit allocation
// ============================================================

TEST(test_tag_bits_allocated) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];

  // tag_bit_mask should be non-zero (3 tags need at least 3 bits)
  assert(value_sr->tag_bit_mask != 0);
  // at least 3 bits set in the mask
  int pop = __builtin_popcountll(value_sr->tag_bit_mask);
  assert(pop >= 3);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: nullable / first_set / last_set analysis
// ============================================================

TEST(test_nullable_analysis) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // value is not nullable (branches have terms/calls that aren't nullable)
  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];
  assert(!value_sr->nullable);

  // array is not nullable (seq of 3 non-nullable items)
  int32_t array_id = symtab_find(&cl->scoped_rule_names, "main$array");
  ScopedRule* array_sr = &cl->scoped_rules[array_id - cl->scoped_rule_names.start_num];
  assert(!array_sr->nullable);

  // value_list sub-rule (star) should be nullable
  int32_t vl_sub = symtab_find(&cl->scoped_rule_names, "main$value_list$1");
  ScopedRule* vl_sub_sr = &cl->scoped_rules[vl_sub - cl->scoped_rule_names.start_num];
  assert(vl_sub_sr->nullable);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_first_set) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // value's first_set should include token ids 1 (number), 2 (string), 3 (lbracket)
  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];
  assert(value_sr->first_set != NULL);
  assert(bitset_contains(value_sr->first_set, 1)); // number
  assert(bitset_contains(value_sr->first_set, 2)); // string
  assert(bitset_contains(value_sr->first_set, 3)); // lbracket (via array)

  // array's first_set should be just lbracket (id 3)
  int32_t array_id = symtab_find(&cl->scoped_rule_names, "main$array");
  ScopedRule* array_sr = &cl->scoped_rules[array_id - cl->scoped_rule_names.start_num];
  assert(array_sr->first_set != NULL);
  assert(bitset_contains(array_sr->first_set, 3));
  assert(!bitset_contains(array_sr->first_set, 1));

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: slot allocation
// ============================================================

TEST(test_naive_slots) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // naive mode: each rule gets its own slot
  int32_t rule_count = (int32_t)darray_size(cl->scoped_rules);
  assert(cl->slots_size == rule_count);

  // all slot indices should be unique
  for (int32_t i = 0; i < rule_count; i++) {
    for (int32_t j = i + 1; j < rule_count; j++) {
      assert(cl->scoped_rules[i].slot_index != cl->scoped_rules[j].slot_index);
    }
  }

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_shared_slots_fewer) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_SHARED, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t rule_count = (int32_t)darray_size(cl->scoped_rules);
  // shared mode: slots_size should be <= rule_count (coloring reduces slots)
  assert(cl->slots_size <= rule_count);
  assert(cl->slots_size > 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: multi-scope
// ============================================================

TEST(test_multi_scope_independent_numbering) {
  PegAnalyzeInput input = {0};

  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "a"); // 1
  symtab_intern(&input.tokens, "b"); // 2

  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "s1"); // 0
  symtab_intern(&input.scope_names, "s2"); // 1

  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "shared"); // 0
  symtab_intern(&input.rule_names, "extra");  // 1
  symtab_intern(&input.rule_names, "s1");     // 2
  symtab_intern(&input.rule_names, "s2");     // 3

  input.rules = darray_new(sizeof(PegRule), 0);

  // shared = a b
  PegUnit sh_seq = {.kind = PEG_SEQ};
  sh_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit sh_a = {.kind = PEG_TERM, .id = 1};
  PegUnit sh_b = {.kind = PEG_TERM, .id = 2};
  darray_push(sh_seq.children, sh_a);
  darray_push(sh_seq.children, sh_b);
  PegRule shared_rule = {.global_id = 0, .scope_id = -1, .body = sh_seq};
  darray_push(input.rules, shared_rule);

  // extra = a
  PegUnit ex_body = {.kind = PEG_TERM, .id = 1};
  PegRule extra_rule = {.global_id = 1, .scope_id = -1, .body = ex_body};
  darray_push(input.rules, extra_rule);

  // s1 = shared extra (scope_id=0)
  PegUnit s1_seq = {.kind = PEG_SEQ};
  s1_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit s1_c1 = {.kind = PEG_CALL, .id = 0};
  PegUnit s1_c2 = {.kind = PEG_CALL, .id = 1};
  darray_push(s1_seq.children, s1_c1);
  darray_push(s1_seq.children, s1_c2);
  PegRule s1_rule = {.global_id = 2, .scope_id = 0, .body = s1_seq};
  darray_push(input.rules, s1_rule);

  // s2 = shared (scope_id=1)
  PegUnit s2_seq = {.kind = PEG_SEQ};
  s2_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit s2_c1 = {.kind = PEG_CALL, .id = 0};
  darray_push(s2_seq.children, s2_c1);
  PegRule s2_rule = {.global_id = 3, .scope_id = 1, .body = s2_seq};
  darray_push(input.rules, s2_rule);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "ms");

  // should have 2 scope closures
  assert(darray_size(result.scope_closures) == 2);

  ScopeClosure* s1_cl = &result.scope_closures[0];
  ScopeClosure* s2_cl = &result.scope_closures[1];

  // s1 has 3 rules (s1, shared, extra), s2 has 2 rules (s2, shared)
  int32_t s1_count = (int32_t)darray_size(s1_cl->scoped_rules);
  int32_t s2_count = (int32_t)darray_size(s2_cl->scoped_rules);
  assert(s1_count == 3);
  assert(s2_count == 2);

  // per-scope slot sizes should match rule counts in naive mode
  assert(s1_cl->slots_size == s1_count);
  assert(s2_cl->slots_size == s2_count);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: node field layout
// ============================================================

TEST(test_node_fields_seq) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // array = @lbracket value_list @rbracket -> 3 node fields
  int32_t array_id = symtab_find(&cl->scoped_rule_names, "main$array");
  ScopedRule* array_sr = &cl->scoped_rules[array_id - cl->scoped_rule_names.start_num];
  assert(array_sr->node_fields != NULL);
  assert(darray_size(array_sr->node_fields) == 3);

  // field 0: lbracket (term, not link, advance ONE)
  assert(strcmp(array_sr->node_fields[0].name, "lbracket") == 0);
  assert(!array_sr->node_fields[0].is_link);
  assert(array_sr->node_fields[0].advance == NODE_ADVANCE_ONE);

  // field 1: value_list (call, not link, advance SLOT)
  assert(strcmp(array_sr->node_fields[1].name, "value_list") == 0);
  assert(!array_sr->node_fields[1].is_link);
  assert(array_sr->node_fields[1].advance == NODE_ADVANCE_SLOT);

  // field 2: rbracket (term, not link, advance ONE)
  assert(strcmp(array_sr->node_fields[2].name, "rbracket") == 0);
  assert(!array_sr->node_fields[2].is_link);
  assert(array_sr->node_fields[2].advance == NODE_ADVANCE_ONE);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_node_fields_branches) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // value = [ @number @string array ] -> 3 node fields (one per branch)
  int32_t value_id = symtab_find(&cl->scoped_rule_names, "main$value");
  ScopedRule* value_sr = &cl->scoped_rules[value_id - cl->scoped_rule_names.start_num];
  assert(value_sr->node_fields != NULL);
  assert(darray_size(value_sr->node_fields) == 3);

  // check field names
  assert(strcmp(value_sr->node_fields[0].name, "number") == 0);
  assert(strcmp(value_sr->node_fields[1].name, "string") == 0);
  assert(strcmp(value_sr->node_fields[2].name, "array") == 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_node_fields_link) {
  PegAnalyzeInput input = {0};
  _build_json_input(&input);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "json");
  ScopeClosure* cl = &result.scope_closures[0];

  // value_list = value*<@comma> -> 1 node field (link)
  int32_t vl_id = symtab_find(&cl->scoped_rule_names, "main$value_list");
  ScopedRule* vl_sr = &cl->scoped_rules[vl_id - cl->scoped_rule_names.start_num];
  assert(vl_sr->node_fields != NULL);
  assert(darray_size(vl_sr->node_fields) == 1);

  assert(strcmp(vl_sr->node_fields[0].name, "value") == 0);
  assert(vl_sr->node_fields[0].is_link);
  assert(vl_sr->node_fields[0].advance == NODE_ADVANCE_NONE);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_node_fields_sanitized) {
  PegAnalyzeInput input = {0};

  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "@lit.while"); // 1
  symtab_intern(&input.tokens, "@lparen");    // 2

  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "main"); // 0

  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1

  input.rules = darray_new(sizeof(PegRule), 0);

  // foo = @lit.while @lparen
  PegUnit foo_seq = {.kind = PEG_SEQ};
  foo_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit t1 = {.kind = PEG_TERM, .id = 1};
  PegUnit t2 = {.kind = PEG_TERM, .id = 2};
  darray_push(foo_seq.children, t1);
  darray_push(foo_seq.children, t2);
  PegRule foo_rule = {.global_id = 1, .scope_id = -1, .body = foo_seq};
  darray_push(input.rules, foo_rule);

  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit mc = {.kind = PEG_CALL, .id = 1};
  darray_push(main_seq.children, mc);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input.rules, main_rule);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "test");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t foo_id = symtab_find(&cl->scoped_rule_names, "main$foo");
  ScopedRule* foo_sr = &cl->scoped_rules[foo_id - cl->scoped_rule_names.start_num];
  assert(foo_sr->node_fields != NULL);
  assert(darray_size(foo_sr->node_fields) == 2);

  // @lit.while -> _lit_while, @lparen -> _lparen
  assert(strcmp(foo_sr->node_fields[0].name, "_lit_while") == 0);
  assert(strcmp(foo_sr->node_fields[1].name, "_lparen") == 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

TEST(test_node_fields_dedup) {
  PegAnalyzeInput input = {0};

  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "a"); // 1

  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "main"); // 0

  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1

  input.rules = darray_new(sizeof(PegRule), 0);

  // foo = [ @a @a ] (branches with repeated term)
  PegUnit foo_br = {.kind = PEG_BRANCHES};
  foo_br.children = darray_new(sizeof(PegUnit), 0);
  PegUnit a1 = {.kind = PEG_TERM, .id = 1, .tag = strdup("first")};
  PegUnit a2 = {.kind = PEG_TERM, .id = 1, .tag = strdup("second")};
  darray_push(foo_br.children, a1);
  darray_push(foo_br.children, a2);

  PegUnit foo_seq = {.kind = PEG_SEQ};
  foo_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(foo_seq.children, foo_br);
  PegRule foo_rule = {.global_id = 1, .scope_id = -1, .body = foo_seq};
  darray_push(input.rules, foo_rule);

  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit mc = {.kind = PEG_CALL, .id = 1};
  darray_push(main_seq.children, mc);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input.rules, main_rule);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "test");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t foo_id = symtab_find(&cl->scoped_rule_names, "main$foo");
  ScopedRule* foo_sr = &cl->scoped_rules[foo_id - cl->scoped_rule_names.start_num];
  assert(foo_sr->node_fields != NULL);
  assert(darray_size(foo_sr->node_fields) == 2);

  // first occurrence: "a", second: "a$1"
  assert(strcmp(foo_sr->node_fields[0].name, "a") == 0);
  assert(strcmp(foo_sr->node_fields[1].name, "a$1") == 0);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: nested branches wrapping
// ============================================================

TEST(test_nested_branches_wrapped) {
  PegAnalyzeInput input = {0};

  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "a"); // 1
  symtab_intern(&input.tokens, "b"); // 2
  symtab_intern(&input.tokens, "c"); // 3

  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "main"); // 0

  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "foo");  // 1

  input.rules = darray_new(sizeof(PegRule), 0);

  // foo = [ a [ b c ] ]
  PegUnit inner_br = {.kind = PEG_BRANCHES};
  inner_br.children = darray_new(sizeof(PegUnit), 0);
  PegUnit bb = {.kind = PEG_TERM, .id = 2, .tag = strdup("b")};
  PegUnit bc = {.kind = PEG_TERM, .id = 3, .tag = strdup("c")};
  darray_push(inner_br.children, bb);
  darray_push(inner_br.children, bc);

  PegUnit outer_br = {.kind = PEG_BRANCHES};
  outer_br.children = darray_new(sizeof(PegUnit), 0);
  PegUnit ba = {.kind = PEG_TERM, .id = 1, .tag = strdup("a")};
  darray_push(outer_br.children, ba);
  darray_push(outer_br.children, inner_br);

  PegUnit foo_seq = {.kind = PEG_SEQ};
  foo_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(foo_seq.children, outer_br);
  PegRule foo_rule = {.global_id = 1, .scope_id = -1, .body = foo_seq};
  darray_push(input.rules, foo_rule);

  PegUnit ms = {.kind = PEG_SEQ};
  ms.children = darray_new(sizeof(PegUnit), 0);
  PegUnit mc = {.kind = PEG_CALL, .id = 1};
  darray_push(ms.children, mc);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = ms};
  darray_push(input.rules, main_rule);

  PegGenInput result = peg_analyze(&input, MEMOIZE_NAIVE, "wrap");
  ScopeClosure* cl = &result.scope_closures[0];

  // nested branches should create a wrapper rule (main$foo$1)
  int32_t wrapper_id = symtab_find(&cl->scoped_rule_names, "main$foo$1");
  assert(wrapper_id >= 0);
  ScopedRule* wrapper_sr = &cl->scoped_rules[wrapper_id - cl->scoped_rule_names.start_num];
  assert(wrapper_sr->body.kind == SCOPED_UNIT_SEQ);

  peg_analyze_free(&result);
  _free_input(&input);
}

// ============================================================
// Tests: exclusiveness analysis in shared mode
// ============================================================

TEST(test_exclusive_rules_share_slots) {
  PegAnalyzeInput input = {0};

  // grammar with rules that have disjoint first sets
  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "a"); // 1
  symtab_intern(&input.tokens, "b"); // 2
  symtab_intern(&input.tokens, "c"); // 3

  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "main"); // 0

  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main"); // 0
  symtab_intern(&input.rule_names, "r1");   // 1
  symtab_intern(&input.rule_names, "r2");   // 2

  input.rules = darray_new(sizeof(PegRule), 0);

  // r1 = @a (first_set = {a})
  PegUnit r1_body = {.kind = PEG_TERM, .id = 1};
  PegRule r1_rule = {.global_id = 1, .scope_id = -1, .body = r1_body};
  darray_push(input.rules, r1_rule);

  // r2 = @b (first_set = {b}) -- disjoint from r1
  PegUnit r2_body = {.kind = PEG_TERM, .id = 2};
  PegRule r2_rule = {.global_id = 2, .scope_id = -1, .body = r2_body};
  darray_push(input.rules, r2_rule);

  // main = [ r1 r2 ] (branches)
  PegUnit main_br = {.kind = PEG_BRANCHES};
  main_br.children = darray_new(sizeof(PegUnit), 0);
  PegUnit c1 = {.kind = PEG_CALL, .id = 1, .tag = strdup("r1")};
  PegUnit c2 = {.kind = PEG_CALL, .id = 2, .tag = strdup("r2")};
  darray_push(main_br.children, c1);
  darray_push(main_br.children, c2);
  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(main_seq.children, main_br);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input.rules, main_rule);

  PegGenInput result = peg_analyze(&input, MEMOIZE_SHARED, "test");
  ScopeClosure* cl = &result.scope_closures[0];

  int32_t r1_id = symtab_find(&cl->scoped_rule_names, "main$r1");
  int32_t r2_id = symtab_find(&cl->scoped_rule_names, "main$r2");
  ScopedRule* r1_sr = &cl->scoped_rules[r1_id - cl->scoped_rule_names.start_num];
  ScopedRule* r2_sr = &cl->scoped_rules[r2_id - cl->scoped_rule_names.start_num];

  // r1 and r2 have disjoint first sets, so they should share a slot
  assert(r1_sr->slot_index == r2_sr->slot_index);

  peg_analyze_free(&result);
  _free_input(&input);
}

int main(void) {
  printf("test_peg_analyze:\n");
  RUN(test_scope_closure_count);
  RUN(test_scoped_rules_breakdown);
  RUN(test_branches_have_tags);
  RUN(test_tag_bits_allocated);
  RUN(test_nullable_analysis);
  RUN(test_first_set);
  RUN(test_naive_slots);
  RUN(test_shared_slots_fewer);
  RUN(test_multi_scope_independent_numbering);
  RUN(test_node_fields_seq);
  RUN(test_node_fields_branches);
  RUN(test_node_fields_link);
  RUN(test_node_fields_sanitized);
  RUN(test_node_fields_dedup);
  RUN(test_nested_branches_wrapped);
  RUN(test_exclusive_rules_share_slots);
  printf("test_peg_analyze: OK\n");
  return 0;
}
