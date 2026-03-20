#include "aut.h"
#include "bitset.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- NFA transition ---

typedef struct {
  int32_t from;
  int32_t to;
  int32_t cp_start;
  int32_t cp_end;
  int32_t action_id;
  int32_t line;
  int32_t col;
} NfaTrans;

typedef struct {
  int32_t from;
  int32_t to;
} EpsTrans;

// --- DFA state (after determinization) ---

typedef struct {
  Bitset* nfa_states;
} DfaState;

typedef struct {
  int32_t from;
  int32_t to;
  int32_t cp_start;
  int32_t cp_end;
  int32_t action_id;
  int32_t line;
  int32_t col;
} DfaTrans;

// --- Aut ---

struct Aut {
  const char* function_name;
  const char* source_file_name;

  NfaTrans* nfa_trans;
  int nfa_ntrans;
  int nfa_trans_cap;

  EpsTrans* eps_trans;
  int eps_ntrans;
  int eps_trans_cap;

  int32_t max_state;

  DfaState* dfa_states;
  int dfa_nstates;
  int dfa_states_cap;

  DfaTrans* dfa_trans;
  int dfa_ntrans;
  int dfa_trans_cap;

  int optimized;
};

Aut* aut_new(const char* function_name, const char* source_file_name) {
  Aut* a = calloc(1, sizeof(Aut));
  a->function_name = function_name;
  a->source_file_name = source_file_name;
  a->max_state = -1;
  return a;
}

void aut_del(Aut* a) {
  if (!a) {
    return;
  }
  free(a->nfa_trans);
  free(a->eps_trans);
  for (int i = 0; i < a->dfa_nstates; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  free(a->dfa_states);
  free(a->dfa_trans);
  free(a);
}

static void track_state(Aut* a, int32_t s) {
  if (s > a->max_state) {
    a->max_state = s;
  }
}

void aut_transition(Aut* a, TransitionDef tdef, DebugInfo di) {
  if (a->nfa_ntrans == a->nfa_trans_cap) {
    a->nfa_trans_cap = a->nfa_trans_cap ? a->nfa_trans_cap * 2 : 64;
    a->nfa_trans = realloc(a->nfa_trans, (size_t)a->nfa_trans_cap * sizeof(NfaTrans));
  }
  a->nfa_trans[a->nfa_ntrans++] = (NfaTrans){
      .from = tdef.from_state_id,
      .to = tdef.to_state_id,
      .cp_start = tdef.cp_start,
      .cp_end = tdef.cp_end_inclusive,
      .action_id = tdef.action_id,
      .line = di.source_file_line,
      .col = di.source_file_col,
  };
  track_state(a, tdef.from_state_id);
  track_state(a, tdef.to_state_id);
}

void aut_epsilon(Aut* a, int32_t from_state, int32_t to_state) {
  if (a->eps_ntrans == a->eps_trans_cap) {
    a->eps_trans_cap = a->eps_trans_cap ? a->eps_trans_cap * 2 : 64;
    a->eps_trans = realloc(a->eps_trans, (size_t)a->eps_trans_cap * sizeof(EpsTrans));
  }
  a->eps_trans[a->eps_ntrans++] = (EpsTrans){from_state, to_state};
  track_state(a, from_state);
  track_state(a, to_state);
}

// --- Epsilon closure ---

static Bitset* epsilon_closure(Bitset* states, EpsTrans* eps, int neps, int nstates) {
  Bitset* result = bitset_or(states, states); // copy
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int i = 0; i < neps; i++) {
      uint32_t from = (uint32_t)eps[i].from;
      uint32_t to = (uint32_t)eps[i].to;
      if (from < (uint32_t)nstates && bitset_contains(result, from) && !bitset_contains(result, to)) {
        bitset_add_bit(result, to);
        changed = 1;
      }
    }
  }
  return result;
}

// --- Bitset equality ---

static int bitset_equal(Bitset* a, Bitset* b, int nstates) {
  for (int i = 0; i < nstates; i++) {
    if (bitset_contains(a, (uint32_t)i) != bitset_contains(b, (uint32_t)i)) {
      return 0;
    }
  }
  return 1;
}

// --- Interval splitting for subset construction ---

typedef struct {
  int32_t val;
} SplitPoint;

static int cmp_int32(const void* a, const void* b) {
  int32_t x = *(const int32_t*)a;
  int32_t y = *(const int32_t*)b;
  return (x > y) - (x < y);
}

// --- Subset construction ---
// Takes NFA transitions, epsilon transitions, initial state set, and nfa state count.
// Produces DFA states and transitions. Returns them through the Aut struct fields.

static void determinize(Aut* a, Bitset* initial, NfaTrans* nfa, int nnfa, EpsTrans* eps, int neps, int nstates) {
  // Clear existing DFA
  for (int i = 0; i < a->dfa_nstates; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  a->dfa_nstates = 0;
  a->dfa_ntrans = 0;

  // DFA state 0 = epsilon closure of initial
  Bitset* start = epsilon_closure(initial, eps, neps, nstates);

  if (a->dfa_nstates == a->dfa_states_cap) {
    a->dfa_states_cap = a->dfa_states_cap ? a->dfa_states_cap * 2 : 64;
    a->dfa_states = realloc(a->dfa_states, (size_t)a->dfa_states_cap * sizeof(DfaState));
  }
  a->dfa_states[a->dfa_nstates++] = (DfaState){.nfa_states = start};

  // Collect all split points from NFA transitions
  int32_t* splits = NULL;
  int nsplits = 0;
  int splits_cap = 0;

  for (int i = 0; i < nnfa; i++) {
    if (nsplits + 2 > splits_cap) {
      splits_cap = splits_cap ? splits_cap * 2 : 128;
      splits = realloc(splits, (size_t)splits_cap * sizeof(int32_t));
    }
    splits[nsplits++] = nfa[i].cp_start;
    splits[nsplits++] = nfa[i].cp_end + 1;
  }
  qsort(splits, (size_t)nsplits, sizeof(int32_t), cmp_int32);
  // Deduplicate
  int dedup = 0;
  for (int i = 0; i < nsplits; i++) {
    if (dedup == 0 || splits[dedup - 1] != splits[i]) {
      splits[dedup++] = splits[i];
    }
  }
  nsplits = dedup;

  // Worklist
  int worklist_head = 0;
  while (worklist_head < a->dfa_nstates) {
    int cur_dfa = worklist_head++;
    Bitset* cur_set = a->dfa_states[cur_dfa].nfa_states;

    // For each interval [splits[i], splits[i+1]-1]
    for (int si = 0; si < nsplits; si++) {
      int32_t lo = splits[si];
      int32_t hi = (si + 1 < nsplits) ? splits[si + 1] - 1 : splits[si];
      if (si + 1 >= nsplits && nsplits > 0) {
        // Last split point: the interval is just [splits[si], splits[si]]
        // But actually, for correctness we only need intervals between consecutive split points.
        // The last split point is an "end+1" marker, no interval starts there unless there's
        // a transition with cp_start == splits[si].
        // We handle this by only processing intervals [splits[i], splits[i+1)-1] for i < nsplits-1
        break;
      }

      // Find all NFA transitions that fire on any codepoint in [lo, hi]
      Bitset* target = bitset_new();
      int32_t min_action = 0;
      int has_action = 0;
      int32_t best_line = 0, best_col = 0;

      for (int t = 0; t < nnfa; t++) {
        if (nfa[t].cp_start <= lo && nfa[t].cp_end >= hi) {
          uint32_t from = (uint32_t)nfa[t].from;
          if (from < (uint32_t)nstates && bitset_contains(cur_set, from)) {
            bitset_add_bit(target, (uint32_t)nfa[t].to);
            if (nfa[t].action_id != 0) {
              if (!has_action || nfa[t].action_id < min_action) {
                min_action = nfa[t].action_id;
                best_line = nfa[t].line;
                best_col = nfa[t].col;
              }
              has_action = 1;
            } else if (!has_action) {
              best_line = nfa[t].line;
              best_col = nfa[t].col;
            }
          }
        }
      }

      if (bitset_size(target) == 0) {
        bitset_del(target);
        continue;
      }

      Bitset* closed = epsilon_closure(target, eps, neps, nstates);
      bitset_del(target);

      // Find or create DFA state for closed
      int found = -1;
      for (int d = 0; d < a->dfa_nstates; d++) {
        if (bitset_equal(a->dfa_states[d].nfa_states, closed, nstates)) {
          found = d;
          break;
        }
      }
      if (found < 0) {
        if (a->dfa_nstates == a->dfa_states_cap) {
          a->dfa_states_cap = a->dfa_states_cap ? a->dfa_states_cap * 2 : 64;
          a->dfa_states = realloc(a->dfa_states, (size_t)a->dfa_states_cap * sizeof(DfaState));
        }
        found = a->dfa_nstates;
        a->dfa_states[a->dfa_nstates++] = (DfaState){.nfa_states = closed};
      } else {
        bitset_del(closed);
      }

      // Add DFA transition
      if (a->dfa_ntrans == a->dfa_trans_cap) {
        a->dfa_trans_cap = a->dfa_trans_cap ? a->dfa_trans_cap * 2 : 64;
        a->dfa_trans = realloc(a->dfa_trans, (size_t)a->dfa_trans_cap * sizeof(DfaTrans));
      }
      a->dfa_trans[a->dfa_ntrans++] = (DfaTrans){
          .from = cur_dfa,
          .to = found,
          .cp_start = lo,
          .cp_end = hi,
          .action_id = min_action,
          .line = best_line,
          .col = best_col,
      };
    }
  }

  free(splits);
}

// --- Reverse an NFA ---
// Swaps from/to on all transitions and epsilon transitions.
// Creates a new super-start state with epsilon transitions to all old "accepting" states.
// For our model: every NFA state that has a non-zero action_id on an outgoing transition,
// or every state reachable from the original NFA, is considered for reversal.
// The old start state (0) becomes an accepting concept — but since we don't have accept states,
// we just reverse all edges and add a super start.

static void reverse_nfa(NfaTrans** out_trans, int* out_ntrans, EpsTrans** out_eps, int* out_neps,
                        int32_t* out_max_state, NfaTrans* trans, int ntrans, EpsTrans* eps, int neps,
                        int32_t old_max_state, int32_t old_start) {
  int32_t super_start = old_max_state + 1;
  *out_max_state = super_start;

  // Reversed transitions: swap from/to
  *out_ntrans = ntrans;
  *out_trans = malloc((size_t)ntrans * sizeof(NfaTrans));
  for (int i = 0; i < ntrans; i++) {
    (*out_trans)[i] = (NfaTrans){
        .from = trans[i].to,
        .to = trans[i].from,
        .cp_start = trans[i].cp_start,
        .cp_end = trans[i].cp_end,
        .action_id = trans[i].action_id,
        .line = trans[i].line,
        .col = trans[i].col,
    };
  }

  // Reversed epsilon transitions + super start -> all states that were accepting
  // "Accepting" in our context: states that have at least one outgoing transition with action_id != 0
  // Plus the old start gets an epsilon from super_start
  // Actually in standard Brzozowski: the reversed NFA has as initial states the old final states,
  // and as final state the old initial state.
  // Since we don't have explicit final states, we consider all states as potential endpoints
  // and just reverse. The super start connects to all states that had *incoming* transitions
  // to the old start? No — for Brzozowski, we need:
  //   reversed_initial = old_final_states
  //   reversed_final = {old_initial_state}
  //
  // For us, we don't have explicit final states — actions are on transitions.
  // The approach: treat every state that is a "to" state of a transition with action_id != 0
  // as a final state. Plus if there are no action transitions, we still need all reachable states.
  //
  // Simpler approach: in our DFA, every state is potentially interesting. For the reversed NFA,
  // the super start has epsilon transitions to ALL states (except old_start).
  // This is conservative but correct — determinization will prune unreachable states.
  //
  // Even simpler: epsilon from super_start to every state except old_start.

  int max_eps = neps + (int)(old_max_state + 1);
  *out_eps = malloc((size_t)max_eps * sizeof(EpsTrans));
  *out_neps = 0;

  // Reversed epsilon transitions
  for (int i = 0; i < neps; i++) {
    (*out_eps)[(*out_neps)++] = (EpsTrans){eps[i].to, eps[i].from};
  }

  // Super start -> all states except old_start
  for (int32_t s = 0; s <= old_max_state; s++) {
    if (s != old_start) {
      (*out_eps)[(*out_neps)++] = (EpsTrans){super_start, s};
    }
  }
}

// --- Brzozowski minimization ---
// reverse -> determinize -> reverse -> determinize

void aut_optimize(Aut* a) {
  if (a->max_state < 0) {
    a->optimized = 1;
    return;
  }

  (void)0;

  // --- First pass: reverse -> determinize ---
  NfaTrans* rev1_trans = NULL;
  int rev1_ntrans = 0;
  EpsTrans* rev1_eps = NULL;
  int rev1_neps = 0;
  int32_t rev1_max = 0;

  reverse_nfa(&rev1_trans, &rev1_ntrans, &rev1_eps, &rev1_neps, &rev1_max, a->nfa_trans, a->nfa_ntrans, a->eps_trans,
              a->eps_ntrans, a->max_state, 0);

  int rev1_nstates = rev1_max + 1;

  // Initial state for determinization = {super_start}
  Bitset* init1 = bitset_new();
  bitset_add_bit(init1, (uint32_t)rev1_max); // super_start
  determinize(a, init1, rev1_trans, rev1_ntrans, rev1_eps, rev1_neps, rev1_nstates);
  bitset_del(init1);
  free(rev1_trans);
  free(rev1_eps);

  // Now a has DFA states and transitions from first determinization.
  // Convert DFA back to NFA format for second reverse.
  int dfa1_nstates = a->dfa_nstates;
  int dfa1_ntrans = a->dfa_ntrans;
  NfaTrans* nfa2 = malloc((size_t)dfa1_ntrans * sizeof(NfaTrans));
  for (int i = 0; i < dfa1_ntrans; i++) {
    nfa2[i] = (NfaTrans){
        .from = a->dfa_trans[i].from,
        .to = a->dfa_trans[i].to,
        .cp_start = a->dfa_trans[i].cp_start,
        .cp_end = a->dfa_trans[i].cp_end,
        .action_id = a->dfa_trans[i].action_id,
        .line = a->dfa_trans[i].line,
        .col = a->dfa_trans[i].col,
    };
  }

  // --- Second pass: reverse -> determinize ---
  NfaTrans* rev2_trans = NULL;
  int rev2_ntrans = 0;
  EpsTrans* rev2_eps = NULL;
  int rev2_neps = 0;
  int32_t rev2_max = 0;

  reverse_nfa(&rev2_trans, &rev2_ntrans, &rev2_eps, &rev2_neps, &rev2_max, nfa2, dfa1_ntrans, NULL, 0,
              (int32_t)(dfa1_nstates - 1), 0);
  free(nfa2);

  int rev2_nstates = rev2_max + 1;

  Bitset* init2 = bitset_new();
  bitset_add_bit(init2, (uint32_t)rev2_max); // super_start
  determinize(a, init2, rev2_trans, rev2_ntrans, rev2_eps, rev2_neps, rev2_nstates);
  bitset_del(init2);
  free(rev2_trans);
  free(rev2_eps);

  // Merge adjacent DFA transitions with same (from, to, action_id) and contiguous ranges
  for (int i = 0; i < a->dfa_ntrans; i++) {
    for (int j = i + 1; j < a->dfa_ntrans; j++) {
      if (a->dfa_trans[i].from == a->dfa_trans[j].from && a->dfa_trans[i].to == a->dfa_trans[j].to &&
          a->dfa_trans[i].action_id == a->dfa_trans[j].action_id &&
          a->dfa_trans[i].cp_end + 1 == a->dfa_trans[j].cp_start) {
        a->dfa_trans[i].cp_end = a->dfa_trans[j].cp_end;
        // Remove j
        memmove(&a->dfa_trans[j], &a->dfa_trans[j + 1], (size_t)(a->dfa_ntrans - j - 1) * sizeof(DfaTrans));
        a->dfa_ntrans--;
        j--;
      }
    }
  }

  a->optimized = 1;
}

// --- Simple determinization for when optimize is not called ---

static void simple_determinize(Aut* a) {
  int nstates = a->max_state + 1;
  Bitset* init = bitset_new();
  bitset_add_bit(init, 0);
  determinize(a, init, a->nfa_trans, a->nfa_ntrans, a->eps_trans, a->eps_ntrans, nstates);
  bitset_del(init);
}

// --- IR generation ---

void aut_gen_dfa(Aut* a, IrWriter* w, bool debug_mode) {
  if (!a->optimized) {
    simple_determinize(a);
  }

  if (debug_mode) {
    irwriter_declare(w, "void", "llvm.debugtrap", "");
  }

  const char* ret_ty = "{i32, i32}";
  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, a->function_name, ret_ty, 2, arg_types, arg_names);

  // entry: switch on %state
  irwriter_bb(w, "entry");
  irwriter_switch_start(w, "i32", "%state", "dead");
  for (int s = 0; s < a->dfa_nstates; s++) {
    char label[32];
    snprintf(label, sizeof(label), "s%d", s);
    irwriter_switch_case(w, "i32", s, label);
  }
  irwriter_switch_end(w);

  // For each DFA state, generate codepoint matching
  for (int s = 0; s < a->dfa_nstates; s++) {
    char state_label[32];
    snprintf(state_label, sizeof(state_label), "s%d", s);
    irwriter_bb(w, state_label);

    // Collect transitions from this state
    int first_trans = -1;
    int ntrans_from = 0;
    for (int t = 0; t < a->dfa_ntrans; t++) {
      if (a->dfa_trans[t].from == s) {
        if (first_trans < 0) {
          first_trans = t;
        }
        ntrans_from++;
      }
    }

    if (ntrans_from == 0) {
      // No transitions: return {state, -2}
      char nomatch_label[32];
      snprintf(nomatch_label, sizeof(nomatch_label), "s%d_nomatch", s);
      irwriter_br(w, nomatch_label);
      irwriter_bb(w, nomatch_label);

      const char* v0 = irwriter_insertvalue(w, ret_ty, "undef", "i32", "%state", 0);
      char v0n[32];
      snprintf(v0n, sizeof(v0n), "%s", v0);
      const char* v1 = irwriter_insertvalue_imm(w, ret_ty, v0n, "i32", -2, 1);
      char v1n[32];
      snprintf(v1n, sizeof(v1n), "%s", v1);
      irwriter_ret(w, ret_ty, v1n);
      continue;
    }

    // Set debug location from first transition
    DfaTrans* ft = &a->dfa_trans[first_trans];
    if (ft->line > 0) {
      irwriter_dbg(w, ft->line, ft->col);
    }

    // Separate single-point transitions (for switch) from range transitions
    // Per spec: "flat switch case" for single points, "if (cp >= ... && cp <= ...)" for ranges
    int has_switch = 0;
    int has_range = 0;
    for (int t = 0; t < a->dfa_ntrans; t++) {
      if (a->dfa_trans[t].from != s) {
        continue;
      }
      if (a->dfa_trans[t].cp_start == a->dfa_trans[t].cp_end) {
        has_switch++;
      } else {
        has_range++;
      }
    }

    char nomatch_label[32];
    snprintf(nomatch_label, sizeof(nomatch_label), "s%d_nomatch", s);

    // If we have ranges, chain range checks first, then switch for single points
    // Actually: let's put switch first (if any), then range checks for wider ranges.
    // Both fall through to nomatch.

    char after_switch_label[32];
    snprintf(after_switch_label, sizeof(after_switch_label), "s%d_ranges", s);

    if (has_switch > 0) {
      const char* sw_default = has_range > 0 ? after_switch_label : nomatch_label;
      irwriter_switch_start(w, "i32", "%cp", sw_default);

      for (int t = 0; t < a->dfa_ntrans; t++) {
        if (a->dfa_trans[t].from != s || a->dfa_trans[t].cp_start != a->dfa_trans[t].cp_end) {
          continue;
        }
        char match_label[32];
        snprintf(match_label, sizeof(match_label), "s%d_t%d", s, t);
        irwriter_switch_case(w, "i32", a->dfa_trans[t].cp_start, match_label);
      }
      irwriter_switch_end(w);
    } else if (has_range > 0) {
      irwriter_br(w, after_switch_label);
    }

    // Range checks
    if (has_range > 0) {
      irwriter_bb(w, after_switch_label);

      int range_idx = 0;
      for (int t = 0; t < a->dfa_ntrans; t++) {
        if (a->dfa_trans[t].from != s || a->dfa_trans[t].cp_start == a->dfa_trans[t].cp_end) {
          continue;
        }

        DfaTrans* dt = &a->dfa_trans[t];
        if (dt->line > 0) {
          irwriter_dbg(w, dt->line, dt->col);
        }

        char match_label[32];
        snprintf(match_label, sizeof(match_label), "s%d_t%d", s, t);

        char next_label[32];
        // Find next range
        int next_range = -1;
        for (int t2 = t + 1; t2 < a->dfa_ntrans; t2++) {
          if (a->dfa_trans[t2].from == s && a->dfa_trans[t2].cp_start != a->dfa_trans[t2].cp_end) {
            next_range = t2;
            break;
          }
        }
        if (next_range >= 0) {
          snprintf(next_label, sizeof(next_label), "s%d_rck%d", s, range_idx + 1);
        } else {
          snprintf(next_label, sizeof(next_label), "%s", nomatch_label);
        }

        const char* lo = irwriter_icmp_imm(w, "sge", "i32", "%cp", dt->cp_start);
        char lo_n[32];
        snprintf(lo_n, sizeof(lo_n), "%s", lo);
        const char* hi = irwriter_icmp_imm(w, "sle", "i32", "%cp", dt->cp_end);
        char hi_n[32];
        snprintf(hi_n, sizeof(hi_n), "%s", hi);
        const char* both = irwriter_binop(w, "and", "i1", lo_n, hi_n);
        char both_n[32];
        snprintf(both_n, sizeof(both_n), "%s", both);
        irwriter_br_cond(w, both_n, match_label, next_label);

        if (next_range >= 0) {
          irwriter_bb(w, next_label);
        }

        range_idx++;
      }
    }

    // Emit match blocks for all transitions from this state
    for (int t = 0; t < a->dfa_ntrans; t++) {
      if (a->dfa_trans[t].from != s) {
        continue;
      }
      DfaTrans* dt = &a->dfa_trans[t];

      char match_label[32];
      snprintf(match_label, sizeof(match_label), "s%d_t%d", s, t);
      irwriter_bb(w, match_label);

      if (dt->line > 0) {
        irwriter_dbg(w, dt->line, dt->col);
      }

      const char* v0 = irwriter_insertvalue_imm(w, ret_ty, "undef", "i32", dt->to, 0);
      char v0n[32];
      snprintf(v0n, sizeof(v0n), "%s", v0);
      const char* v1 = irwriter_insertvalue_imm(w, ret_ty, v0n, "i32", dt->action_id, 1);
      char v1n[32];
      snprintf(v1n, sizeof(v1n), "%s", v1);
      irwriter_ret(w, ret_ty, v1n);
    }

    // Nomatch block
    irwriter_bb(w, nomatch_label);
    const char* v0 = irwriter_insertvalue(w, ret_ty, "undef", "i32", "%state", 0);
    char v0n[32];
    snprintf(v0n, sizeof(v0n), "%s", v0);
    const char* v1 = irwriter_insertvalue_imm(w, ret_ty, v0n, "i32", -2, 1);
    char v1n[32];
    snprintf(v1n, sizeof(v1n), "%s", v1);
    irwriter_ret(w, ret_ty, v1n);
  }

  // Dead state block
  irwriter_bb(w, "dead");
  {
    if (debug_mode) {
      irwriter_call_void(w, "llvm.debugtrap");
    }
    const char* v0 = irwriter_insertvalue(w, ret_ty, "undef", "i32", "%state", 0);
    char v0n[32];
    snprintf(v0n, sizeof(v0n), "%s", v0);
    const char* v1 = irwriter_insertvalue_imm(w, ret_ty, v0n, "i32", -2, 1);
    char v1n[32];
    snprintf(v1n, sizeof(v1n), "%s", v1);
    irwriter_ret(w, ret_ty, v1n);
  }

  irwriter_define_end(w);
}
