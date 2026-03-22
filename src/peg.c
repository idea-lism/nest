// PEG (Parsing Expression Grammar) code generation.
// Generates packrat parser functions in LLVM IR,
// and emits node type definitions to the C header.

#include "peg.h"
#include "darray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Collect rules per scope, assign compact rule IDs

typedef struct {
  char* name; // owned
  int32_t id;
} RuleId;

typedef struct {
  char* scope; // owned
  RuleId* rule_ids; // darray
} PegClosure;

static int32_t _assign_rule_id(PegClosure* closure, const char* name) {
  int32_t n = (int32_t)darray_size(closure->rule_ids);
  for (int32_t i = 0; i < n; i++) {
    if (strcmp(closure->rule_ids[i].name, name) == 0) {
      return closure->rule_ids[i].id;
    }
  }
  int32_t id = n + 1;
  RuleId rid;
  rid.name = strdup(name);
  rid.id = id;
  darray_push(closure->rule_ids, rid);
  return id;
}

static void _gen_ref_type(HeaderWriter* hw) {
  hw_blank(hw);
  hw_comment(hw, "PEG reference type");
  hw_struct_begin(hw, "PegRef");
  hw_field(hw, "void*", "table");
  hw_field(hw, "int32_t", "col");
  hw_field(hw, "int32_t", "next_col");
  hw_struct_end(hw);
  hw_raw(hw, " PegRef;\n\n");
}

static void _gen_node_type(HeaderWriter* hw, PegRule* rule) {
  int32_t name_len = snprintf(NULL, 0, "%sNode", rule->name) + 1;
  char struct_name[name_len];
  snprintf(struct_name, (size_t)name_len, "%sNode", rule->name);
  if (struct_name[0] >= 'a' && struct_name[0] <= 'z') {
    struct_name[0] -= 32;
  }

  hw_struct_begin(hw, struct_name);

  PegUnit** all_branches = darray_new(sizeof(PegUnit*), 0);
  for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
    if (rule->seq.children[i].kind == PEG_BRANCHES) {
      PegUnit* bu = &rule->seq.children[i];
      for (int32_t j = 0; j < (int32_t)darray_size(bu->children); j++) {
        darray_push(all_branches, &bu->children[j]);
      }
    }
  }
  int32_t nbranches = (int32_t)darray_size(all_branches);

  if (nbranches > 0) {
    hw_raw(hw, "  struct {\n");
    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      if (branch->tag && branch->tag[0]) {
        hw_fmt(hw, "    bool %s : 1;\n", branch->tag);
      } else {
        hw_fmt(hw, "    bool branch%d : 1;\n", i);
      }
    }
    hw_raw(hw, "  } is;\n");

    for (int32_t i = 0; i < nbranches; i++) {
      PegUnit* branch = all_branches[i];
      for (int32_t j = 0; j < (int32_t)darray_size(branch->children); j++) {
        PegUnit* child = &branch->children[j];
        if (child->name && child->name[0]) {
          hw_field(hw, "PegRef", child->name);
        }
      }
    }
  } else {
    for (int32_t i = 0; i < (int32_t)darray_size(rule->seq.children); i++) {
      PegUnit* child = &rule->seq.children[i];
      if (child->name && child->name[0]) {
        hw_field(hw, "PegRef", child->name);
      }
    }
  }

  darray_del(all_branches);
  hw_struct_end(hw);
  hw_fmt(hw, " %s;\n\n", struct_name);
}

static void _gen_load_decl(HeaderWriter* hw, PegRule* rule) {
  int32_t sn_len = snprintf(NULL, 0, "%sNode", rule->name) + 1;
  int32_t fn_len = snprintf(NULL, 0, "load_%s", rule->name) + 1;
  char struct_name[sn_len], func_name[fn_len];
  snprintf(struct_name, (size_t)sn_len, "%sNode", rule->name);
  snprintf(func_name, (size_t)fn_len, "load_%s", rule->name);
  if (struct_name[0] >= 'a' && struct_name[0] <= 'z') {
    struct_name[0] -= 32;
  }
  hw_fmt(hw, "%s %s(PegRef ref);\n", struct_name, func_name);
}

static void _gen_rule_ir(PegRule* rule, int32_t rule_id, const char* scope, IrWriter* w) {
  (void)rule;
  (void)rule_id;
  (void)scope;
  (void)w;
  // TODO: Generate LLVM IR for packrat parsing
}

void peg_gen(PegGenInput* input, HeaderWriter* hw, IrWriter* w) {
  PegRule* rules = input->rules;

  if ((int32_t)darray_size(rules) == 0) {
    return;
  }

  PegClosure closure = {0};
  closure.rule_ids = darray_new(sizeof(RuleId), 0);
  closure.scope = strdup("main");

  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    _assign_rule_id(&closure, rules[i].name);
  }

  _gen_ref_type(hw);

  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    _gen_node_type(hw, &rules[i]);
    _gen_load_decl(hw, &rules[i]);
  }

  hw_blank(hw);
  hw_comment(hw, "Helper functions");
  hw_raw(hw, "static inline bool peg_has_next(PegRef ref) { return ref.next_col >= 0; }\n");
  hw_raw(hw, "static inline PegRef peg_get_next(PegRef ref) { return (PegRef){ref.table, ref.next_col, -1}; }\n");

  for (int32_t i = 0; i < (int32_t)darray_size(rules); i++) {
    int32_t rid = _assign_rule_id(&closure, rules[i].name);
    _gen_rule_ir(&rules[i], rid, closure.scope, w);
  }

  for (int32_t i = 0; i < (int32_t)darray_size(closure.rule_ids); i++) {
    free(closure.rule_ids[i].name);
  }
  darray_del(closure.rule_ids);
  free(closure.scope);
}
