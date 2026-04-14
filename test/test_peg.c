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

// ============================================================
// Helper: build a PegGenInput for a simple JSON-like grammar
//
// Grammar:
//   main = value
//   value = [ @number  @string  array ]
//   array = @lbracket value_list @rbracket
//   value_list = value*<@comma>
//
// Tokens: number=1, string=2, lbracket=3, rbracket=4, comma=5
// Scopes: main scope_id=0
// Rules: main(global_id=0, scope_id=0), value(1,-1), array(2,-1), value_list(3,-1)
// ============================================================

static void _build_json_input(PegGenInput* input) {
  // tokens (start_num=1)
  symtab_init(&input->tokens, 1);
  symtab_intern(&input->tokens, "number");   // 1
  symtab_intern(&input->tokens, "string");   // 2
  symtab_intern(&input->tokens, "lbracket"); // 3
  symtab_intern(&input->tokens, "rbracket"); // 4
  symtab_intern(&input->tokens, "comma");    // 5

  // scope_names (start_num=0)
  symtab_init(&input->scope_names, 0);
  symtab_intern(&input->scope_names, "main"); // 0

  // rule_names (start_num=0)
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
  PegUnit br_arr = {.kind = PEG_CALL, .id = 2, .tag = strdup("array")}; // calls rule 2 (array)
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
  PegUnit vl = {.kind = PEG_CALL, .id = 3}; // calls value_list
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
  PegUnit main_call = {.kind = PEG_CALL, .id = 1}; // calls value
  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(main_seq.children, main_call);

  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input->rules, main_rule);
}

static void _free_unit(PegUnit* u) {
  free(u->tag);
  if (u->children) {
    for (int32_t i = 0; i < (int32_t)darray_size(u->children); i++) {
      _free_unit(&u->children[i]);
    }
    darray_del(u->children);
  }
}

static void _free_input(PegGenInput* input) {
  for (int32_t i = 0; i < (int32_t)darray_size(input->rules); i++) {
    _free_unit(&input->rules[i].body);
  }
  darray_del(input->rules);
  symtab_free(&input->tokens);
  symtab_free(&input->scope_names);
  symtab_free(&input->rule_names);
}

// ============================================================
// Test: scope closure gathers correct rules
// ============================================================

TEST(test_closure_and_breakdown) {
  PegGenInput input = {0};
  _build_json_input(&input);

  // generate with naive mode
  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, MEMO_NAIVE, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // verify IR was generated
  assert(ir_buf != NULL);
  assert(ir_len > 0);
  // should contain parse_main function
  assert(strstr(ir_buf, "parse_main") != NULL);

  // verify header was generated
  assert(hdr_buf != NULL);
  assert(hdr_len > 0);
  // should contain PegRef type
  assert(strstr(hdr_buf, "PegRef") != NULL);
  // should contain node types
  assert(strstr(hdr_buf, "Node_value") != NULL);
  // should contain loader functions
  assert(strstr(hdr_buf, "json_load_value") != NULL);

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Test: compressed mode generates code
// ============================================================

TEST(test_compressed_mode) {
  PegGenInput input = {0};
  _build_json_input(&input);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, MEMO_SHARED, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  assert(ir_buf != NULL);
  assert(strstr(ir_buf, "parse_main") != NULL);
  assert(hdr_buf != NULL);
  assert(strstr(hdr_buf, "PegRef") != NULL);

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Test: loader functions don't call parse functions
// (acceptance criterion: load_xxx MUST NOT call parse_xxx)
// ============================================================

TEST(test_loaders_decode_only) {
  PegGenInput input = {0};
  _build_json_input(&input);

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  peg_gen(&input, hw, w, MEMO_NAIVE, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // extract each loader function body and verify it doesn't contain parse_ calls
  // look for json_load_ functions
  const char* p = hdr_buf;
  while ((p = strstr(p, "json_load_")) != NULL) {
    // find the function body (between { and matching })
    const char* body_start = strchr(p, '{');
    assert(body_start != NULL);
    const char* body_end = strchr(body_start, '}');
    assert(body_end != NULL);

    // check that between { and } there's no parse_ call
    for (const char* q = body_start; q < body_end; q++) {
      if (strncmp(q, "parse_", 6) == 0) {
        fprintf(stderr, "FAIL: loader calls parse_ function\n");
        assert(0);
      }
    }

    p = body_end + 1;
  }

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Test: header has_next / get_next helpers for iteration
// ============================================================

TEST(test_iteration_helpers) {
  PegGenInput input = {0};
  _build_json_input(&input);

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  peg_gen(&input, hw, w, MEMO_NAIVE, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  assert(strstr(hdr_buf, "json_has_next") != NULL);
  assert(strstr(hdr_buf, "json_get_next") != NULL);

  // has_next and get_next must use tc->value, not tc directly
  assert(strstr(hdr_buf, "l.elem.tc->value") != NULL);

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Test: branch tags appear in generated node
// ============================================================

TEST(test_branch_tags) {
  PegGenInput input = {0};
  _build_json_input(&input);

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  peg_gen(&input, hw, w, MEMO_NAIVE, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // value has branches with tags: number, string, array
  assert(strstr(hdr_buf, "bool number : 1") != NULL);
  assert(strstr(hdr_buf, "bool string : 1") != NULL);
  assert(strstr(hdr_buf, "bool array : 1") != NULL);

  // branch children must appear as fields in Node_value
  assert(strstr(hdr_buf, "PegRef number;") != NULL);
  assert(strstr(hdr_buf, "PegRef string;") != NULL);
  assert(strstr(hdr_buf, "PegRef array;") != NULL);

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Shared helper: generate IR + header for the JSON grammar
// ============================================================

typedef struct {
  char* ir_buf;
  size_t ir_len;
  char* hdr_buf;
  size_t hdr_len;
} GenResult;

static GenResult _gen_json(int memoize_mode) {
  GenResult r = {0};
  PegGenInput input = {0};
  _build_json_input(&input);

  FILE* ir_f = compat_open_memstream(&r.ir_buf, &r.ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  FILE* hdr_f = compat_open_memstream(&r.hdr_buf, &r.hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, memoize_mode, "json");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  _free_input(&input);
  return r;
}

static void _free_gen(GenResult* r) {
  free(r->ir_buf);
  free(r->hdr_buf);
}

// ============================================================
// Finding 3: Call targets must resolve to the correct broken-down
// rule, not a symtab index that diverges from the rules[] index.
//
// After breakdown, `main` calls `value`. The generated IR should
// reference the root of value's breakdown (named "value"), not
// some interior sub-node. We check that the IR contains a branch
// to a label whose name matches the declared rule name.
// ============================================================

TEST(test_call_targets_correct) {
  GenResult g = _gen_json(MEMO_NAIVE);

  // The IR should contain a branch to the rule for "value".
  // peg_ir_call emits: br label %<name>
  // where <name> comes from ctx->rules[scoped_rule_id].name
  //
  // If call targets are wrong (symtab ID != rules[] index),
  // the name will be some interior node like "value$0" instead of "value".
  //
  // Check: the IR must contain "br label %value" (not %value$...)
  // and there must be a corresponding "value:" label.

  // First, check that we have a branch TO value
  const char* br_to_value = strstr(g.ir_buf, "br label %main$value\n");
  assert(br_to_value != NULL && "main should branch to value, not an interior node");

  // Check that value label exists in the IR
  assert(strstr(g.ir_buf, "main$value:") != NULL && "value: label must exist in IR");

  // For completeness, check array and value_list are also reachable
  assert(strstr(g.ir_buf, "main$array:") != NULL && "array: label must exist");
  assert(strstr(g.ir_buf, "main$value_list:") != NULL && "value_list: label must exist");

  _free_gen(&g);
}

TEST(test_memoization_slots_used) {
  GenResult g = _gen_json(MEMO_NAIVE);

  // The generated IR for parse_main should contain at least one
  // peg_ir_write_slot pattern: a store of an i32 value into a
  // GEP-computed table address. The memset initializes the table,
  // but that uses llvm.memset, not individual stores.
  //
  // Look for the pattern: after the memset, there should be a
  // "store i32" to a table-derived pointer. peg_ir_write_slot
  // computes: gep table + col*stride + slot_offset, then store.
  //
  // If no memoization is happening, the only stores to the table
  // are the initial memset and the tc->value assignment.

  // Find the parse_main function body
  const char* fn_start = strstr(g.ir_buf, "define");
  assert(fn_start != NULL);
  const char* fn_body = strstr(fn_start, "{\n");
  assert(fn_body != NULL);

  // After the table allocation call, count "store i32" instructions.
  // The alloc line is: call ... @tt_alloc_memoize_table(...)
  const char* after_alloc = strstr(fn_body, "tt_alloc_memoize_table");
  assert(after_alloc != NULL);
  after_alloc = strchr(after_alloc, '\n');
  assert(after_alloc != NULL);

  // Count store i32 instructions after the table allocation
  int store_count = 0;
  const char* p = after_alloc;
  while ((p = strstr(p, "store i32")) != NULL) {
    store_count++;
    p += 9;
  }
  (void)store_count;

  // With proper memoization, each rule match writes its result to
  // a table slot. For the JSON grammar, there should be multiple
  // slot writes. Without memoization, the only stores after alloc
  // are to local allocas (col_index, stack, ret_val, etc.), not to
  // table-derived pointers — but counting is imprecise. So let's
  // be more specific: look for the read-slot pattern.
  //
  // peg_ir_read_slot emits a load from a table GEP, then compares
  // with -1. Look for "icmp ne i32 %rN, -1" — the cache-hit check.
  int cache_check = 0;
  p = after_alloc;
  while ((p = strstr(p, ", -1")) != NULL) {
    // Check this is an icmp, not just any -1
    // Look backwards for "icmp"
    const char* line_start = p;
    while (line_start > after_alloc && *line_start != '\n') {
      line_start--;
    }
    if (strstr(line_start, "icmp") != NULL && strstr(line_start, "icmp") < p) {
      cache_check++;
    }
    p += 4;
  }

  // There must be at least one cache-hit check (read slot, compare with -1)
  assert(cache_check > 0 && "no memoization slot reads found — packrat memoization is missing");

  _free_gen(&g);
}

// ============================================================
// Finding 5: Loaders must actually decode from the memoize table,
// not return zero-initialized structs.
//
// The spec says load_xxx reads from ref.tc->value (the memoize
// table) using ref.col to index into it. The current loaders just
// do: node = {0}; (void)ref; return node;
// ============================================================

TEST(test_loader_decodes_from_table) {
  GenResult g = _gen_json(MEMO_NAIVE);

  // Find the json_load_value function body
  const char* loader = strstr(g.hdr_buf, "json_load_value(PegRef ref)");
  assert(loader != NULL);
  const char* body_start = strchr(loader, '{');
  assert(body_start != NULL);
  const char* body_end = strchr(body_start + 1, '}');
  assert(body_end != NULL);

  // The loader body must reference ref.col or ref.tc to actually
  // decode from the table. A placeholder that ignores ref fails this.
  // Check that the body does NOT contain "(void)ref" — which is the
  // hallmark of a placeholder that ignores its argument.
  size_t body_len = (size_t)(body_end - body_start);
  char* body = malloc(body_len + 1);
  memcpy(body, body_start, body_len);
  body[body_len] = '\0';

  // A real decoder must access ref.tc->value to read from the memo table
  bool accesses_table = (strstr(body, "ref.tc->value") != NULL);
  assert(accesses_table && "loader must decode from memo table via ref.tc->value");

  free(body);
  _free_gen(&g);
}

// ============================================================
// Finding 4: Branch rules must store the chosen child's
// scoped_rule_id in the memoize slot, not just the consumed length.
//
// The spec says: "if it is a branch rule, store the chosen branch's
// scope_rule_id". This enables the loader to determine which branch
// was taken by reading the slot.
//
// The loader's `is` bitfield should be set based on the stored ID.
// If the loader body is a placeholder, this is already caught by
// test_loader_decodes_from_table. Here we check from the IR side:
// the ordered-choice codegen should store a branch index / ID into
// a table slot after a successful match.
// ============================================================

TEST(test_branch_stores_child_id) {
  GenResult g = _gen_json(MEMO_NAIVE);

  // In a correct implementation, after peg_ir_choice succeeds for
  // a branch, the code should call peg_ir_write_slot with the
  // chosen branch's scoped_rule_id (a small integer constant).
  //
  // We check this indirectly: the loader for "value" (which has
  // branches) must set the `is` bitfield. Since the loader is
  // generated in the header, check that it sets `.is.number`,
  // `.is.string`, or `.is.array` based on table data.
  //
  // Since the loader is currently a placeholder, this will fail.

  const char* loader = strstr(g.hdr_buf, "json_load_value(PegRef ref)");
  assert(loader != NULL);
  const char* body_start = strchr(loader, '{');
  assert(body_start != NULL);
  const char* body_end = strchr(body_start + 1, '}');
  assert(body_end != NULL);

  size_t body_len = (size_t)(body_end - body_start);
  char* body = malloc(body_len + 1);
  memcpy(body, body_start, body_len);
  body[body_len] = '\0';

  // The loader must set tag bits: either individual .is.xxx fields or bulk &n.is assignment
  bool sets_is = (strstr(body, ".is.") != NULL || strstr(body, "&n.is)") != NULL);
  assert(sets_is && "branch loader must set is.* fields from memoize table");

  free(body);
  _free_gen(&g);
}

// ============================================================
// Finding 6: In row_shared mode, the generated IR must contain
// bit test / deny / exclude operations. Currently the coloring
// analysis runs but the codegen path never uses the results.
// ============================================================

TEST(test_row_shared_emits_bit_ops) {
  GenResult g = _gen_json(MEMO_SHARED);

  // With compress_memoize=true, the generated IR should contain
  // the bit_test pattern: "and i32" + "icmp ne" — the sequence
  // emitted by peg_ir_bit_test.
  //
  // Current _gen_scope never calls peg_ir_bit_test, so this will
  // fail — the IR won't contain any "and i32" inside parse_main.

  const char* fn = strstr(g.ir_buf, "parse_main");
  assert(fn != NULL);

  // Look for calls to bit helper functions
  bool has_bit_op =
      (strstr(fn, "@bit_test") != NULL || strstr(fn, "@bit_deny") != NULL || strstr(fn, "@bit_exclude") != NULL);
  assert(has_bit_op && "row_shared mode must emit bit test/deny/exclude operations");

  _free_gen(&g);
}

// ============================================================
// Finding 8: Branch wrapping must use the lowered rule kind,
// not just the original AST PegUnit kind.
//
// Grammar: outer = [inner_call x] where inner_call is a PEG_CALL
// to a rule that itself is PEG_BRANCHES. After lowering, the call
// resolves to a BRANCHES-kind ScopedRule, so the branch child
// should be wrapped in a SEQ. Current code only checks
// branch->kind == PEG_BRANCHES on the original AST node.

// ============================================================
// Finding 8: nested PEG_BRANCHES must be wrapped in SEQ.
//
// Grammar: foo = [ a [ b c ] ]
// After breakdown: foo$0 = branches(foo$0$0, foo$0$1)
// foo$0$1 is the inner branches [b c]. Since a branch child is
// itself PEG_BRANCHES, it must be wrapped: foo$0$1000 = seq(foo$0$1)
// This ensures the slot-reading invariant.
// ============================================================

TEST(test_branch_wrap_by_lowered_kind) {
  PegGenInput input = {0};
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
  darray_push(outer_br.children, inner_br); // nested branches

  PegUnit foo_seq = {.kind = PEG_SEQ};
  foo_seq.children = darray_new(sizeof(PegUnit), 0);
  darray_push(foo_seq.children, outer_br);
  PegRule foo_rule = {.global_id = 1, .scope_id = -1, .body = foo_seq};
  darray_push(input.rules, foo_rule);

  // main = foo (scope)
  PegUnit ms = {.kind = PEG_SEQ};
  ms.children = darray_new(sizeof(PegUnit), 0);
  PegUnit mc = {.kind = PEG_CALL, .id = 1};
  darray_push(ms.children, mc);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = ms};
  darray_push(input.rules, main_rule);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");
  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, MEMO_NAIVE, "wrap");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // The nested PEG_BRANCHES should produce a wrapper SEQ rule
  bool has_wrapper = (strstr(ir_buf, "main$foo$1:") != NULL);
  assert(has_wrapper && "nested PEG_BRANCHES must be wrapped in SEQ");

  free(ir_buf);
  free(hdr_buf);

  for (int32_t i = 0; i < (int32_t)darray_size(input.rules); i++) {
    _free_unit(&input.rules[i].body);
  }
  darray_del(input.rules);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
  symtab_free(&input.rule_names);
}

// ============================================================
// Test: node field naming scheme
// Token names like @lparen -> _lparen, @lit.while -> _lit_while
// ============================================================

TEST(test_node_field_naming) {
  PegGenInput input = {0};

  // tokens (start_num=1): @lit.while=1, @lparen=2, @rparen=3
  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "@lit.while"); // 1
  symtab_intern(&input.tokens, "@lparen");    // 2
  symtab_intern(&input.tokens, "@rparen");    // 3

  // scope_names
  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "main"); // 0

  // rule_names
  symtab_init(&input.rule_names, 0);
  symtab_intern(&input.rule_names, "main");       // 0
  symtab_intern(&input.rule_names, "while_stmt"); // 1
  symtab_intern(&input.rule_names, "expr");       // 2
  symtab_intern(&input.rule_names, "block");      // 3

  input.rules = darray_new(sizeof(PegRule), 0);

  // while_stmt = "while" @lparen expr @rparen block
  PegUnit ws_seq = {.kind = PEG_SEQ};
  ws_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit lit_while = {.kind = PEG_TERM, .id = 1}; // @lit.while
  PegUnit lp = {.kind = PEG_TERM, .id = 2};        // @lparen
  PegUnit ex = {.kind = PEG_CALL, .id = 2};        // expr
  PegUnit rp = {.kind = PEG_TERM, .id = 3};        // @rparen
  PegUnit bl = {.kind = PEG_CALL, .id = 3};        // block
  darray_push(ws_seq.children, lit_while);
  darray_push(ws_seq.children, lp);
  darray_push(ws_seq.children, ex);
  darray_push(ws_seq.children, rp);
  darray_push(ws_seq.children, bl);
  PegRule ws_rule = {.global_id = 1, .scope_id = -1, .body = ws_seq};
  darray_push(input.rules, ws_rule);

  // expr = @lit.while (dummy single-element body, exercises PEG_TERM path)
  PegUnit expr_body = {.kind = PEG_TERM, .id = 1};
  PegRule expr_rule = {.global_id = 2, .scope_id = -1, .body = expr_body};
  darray_push(input.rules, expr_rule);

  // block = @lparen (dummy)
  PegUnit block_body = {.kind = PEG_TERM, .id = 2};
  PegRule block_rule = {.global_id = 3, .scope_id = -1, .body = block_body};
  darray_push(input.rules, block_rule);

  // main = while_stmt (scope)
  PegUnit main_seq = {.kind = PEG_SEQ};
  main_seq.children = darray_new(sizeof(PegUnit), 0);
  PegUnit main_call = {.kind = PEG_CALL, .id = 1};
  darray_push(main_seq.children, main_call);
  PegRule main_rule = {.global_id = 0, .scope_id = 0, .body = main_seq};
  darray_push(input.rules, main_rule);

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");

  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, MEMO_NAIVE, "lang");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // Sequence fields: @lit.while -> _lit_while, @lparen -> _lparen, @rparen -> _rparen
  assert(strstr(hdr_buf, "PegRef _lit_while;") != NULL);
  assert(strstr(hdr_buf, "PegRef _lparen;") != NULL);
  assert(strstr(hdr_buf, "PegRef _rparen;") != NULL);
  // Rule references stay as-is
  assert(strstr(hdr_buf, "PegRef expr;") != NULL);
  assert(strstr(hdr_buf, "PegRef block;") != NULL);

  // Single-element body: expr rule body is @lit.while -> _lit_while
  // Node_expr struct should contain _lit_while field
  assert(strstr(hdr_buf, "} Node_expr;") != NULL);
  // Find the typedef for Node_expr and check it has the right field
  const char* expr_td = strstr(hdr_buf, "} Node_expr;");
  // Walk backwards to find the opening typedef struct {
  const char* expr_open = expr_td;
  while (expr_open > hdr_buf && strncmp(expr_open, "typedef struct", 14) != 0) {
    expr_open--;
  }
  // Between expr_open and expr_td, we should find _lit_while
  size_t expr_span = (size_t)(expr_td - expr_open);
  char* expr_chunk = malloc(expr_span + 1);
  memcpy(expr_chunk, expr_open, expr_span);
  expr_chunk[expr_span] = '\0';
  assert(strstr(expr_chunk, "PegRef _lit_while;") != NULL);
  free(expr_chunk);

  // Must NOT contain raw @ or . in field names
  // Search for "PegRef @" which would indicate unsanitized names
  assert(strstr(hdr_buf, "PegRef @") == NULL);
  assert(strstr(hdr_buf, "bool @") == NULL);

  free(ir_buf);
  free(hdr_buf);
  _free_input(&input);
}

// ============================================================
// Test: multi-scope loader uses per-scope slot numbering
// ============================================================

TEST(test_multi_scope_loader) {
  PegGenInput input = {0};

  // tokens
  symtab_init(&input.tokens, 1);
  symtab_intern(&input.tokens, "a"); // 1
  symtab_intern(&input.tokens, "b"); // 2

  // scopes: s1(0), s2(1)
  symtab_init(&input.scope_names, 0);
  symtab_intern(&input.scope_names, "s1"); // 0
  symtab_intern(&input.scope_names, "s2"); // 1

  // rules: shared(0), extra(1), s1(2), s2(3)
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

  // extra = a (only used in s1)
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

  char* ir_buf = NULL;
  size_t ir_len = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_len);
  IrWriter* w = irwriter_new(ir_f, NULL);
  irwriter_start(w, "test.c", ".");
  char* hdr_buf = NULL;
  size_t hdr_len = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_len);
  HeaderWriter* hw = hw_new(hdr_f);

  peg_gen(&input, hw, w, MEMO_NAIVE, "ms");

  irwriter_end(w);
  irwriter_del(w);
  fclose(ir_f);
  hw_del(hw);
  fclose(hdr_f);

  // The loader for "shared" must switch on scope_id since it appears in both s1 and s2
  const char* loader = strstr(hdr_buf, "ms_load_shared");
  assert(loader != NULL);
  // Find the body
  const char* body_start = strchr(loader, '{');
  assert(body_start != NULL);

  // Must contain switch on scope_id with both case 0 and case 1
  assert(strstr(body_start, "switch (ref.tc->scope_id)") != NULL);
  assert(strstr(body_start, "case 0:") != NULL);
  assert(strstr(body_start, "case 1:") != NULL);

  free(ir_buf);
  free(hdr_buf);

  for (int32_t i = 0; i < (int32_t)darray_size(input.rules); i++) {
    _free_unit(&input.rules[i].body);
  }
  darray_del(input.rules);
  symtab_free(&input.tokens);
  symtab_free(&input.scope_names);
  symtab_free(&input.rule_names);
}

int main(void) {
  printf("test_peg:\n");
  RUN(test_closure_and_breakdown);
  RUN(test_compressed_mode);
  RUN(test_loaders_decode_only);
  RUN(test_iteration_helpers);
  RUN(test_branch_tags);
  RUN(test_call_targets_correct);
  RUN(test_memoization_slots_used);
  RUN(test_loader_decodes_from_table);
  RUN(test_branch_stores_child_id);
  RUN(test_row_shared_emits_bit_ops);
  RUN(test_branch_wrap_by_lowered_kind);
  RUN(test_node_field_naming);
  RUN(test_multi_scope_loader);
  printf("test_peg: OK\n");
  return 0;
}
