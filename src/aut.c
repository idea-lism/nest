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

  int32_t* state_actions;
  int32_t state_actions_cap;

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
  free(a->state_actions);
  for (int i = 0; i < a->dfa_nstates; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  free(a->dfa_states);
  free(a->dfa_trans);
  free(a);
}

static void _track_state(Aut* a, int32_t s) {
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
      .line = di.source_file_line,
      .col = di.source_file_col,
  };
  _track_state(a, tdef.from_state_id);
  _track_state(a, tdef.to_state_id);
}

void aut_epsilon(Aut* a, int32_t from_state, int32_t to_state) {
  if (a->eps_ntrans == a->eps_trans_cap) {
    a->eps_trans_cap = a->eps_trans_cap ? a->eps_trans_cap * 2 : 64;
    a->eps_trans = realloc(a->eps_trans, (size_t)a->eps_trans_cap * sizeof(EpsTrans));
  }
  a->eps_trans[a->eps_ntrans++] = (EpsTrans){from_state, to_state};
  _track_state(a, from_state);
  _track_state(a, to_state);
}

void aut_action(Aut* a, int32_t state, int32_t action_id) {
  if (action_id == 0) {
    return;
  }
  _track_state(a, state);
  if (state >= a->state_actions_cap) {
    int32_t new_cap = a->state_actions_cap ? a->state_actions_cap : 64;
    while (new_cap <= state) {
      new_cap *= 2;
    }
    a->state_actions = realloc(a->state_actions, (size_t)new_cap * sizeof(int32_t));
    memset(a->state_actions + a->state_actions_cap, 0, (size_t)(new_cap - a->state_actions_cap) * sizeof(int32_t));
    a->state_actions_cap = new_cap;
  }
  // MIN-RULE: keep the smallest non-zero action_id
  if (a->state_actions[state] == 0 || action_id < a->state_actions[state]) {
    a->state_actions[state] = action_id;
  }
}

// --- Epsilon closure ---
// Returns the closure bitset and writes the min non-zero action_id from states in the closure.

static Bitset* _epsilon_closure(Bitset* states, EpsTrans* eps, int neps, int nstates, int32_t* sa, int32_t sa_n,
                                int32_t* out_action) {
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
  if (out_action) {
    int32_t min_action = 0;
    for (int32_t i = 0; i < sa_n && i < nstates; i++) {
      if (bitset_contains(result, (uint32_t)i) && sa[i] != 0) {
        if (min_action == 0 || sa[i] < min_action) {
          min_action = sa[i];
        }
      }
    }
    *out_action = min_action;
  }
  return result;
}

// --- Interval splitting for subset construction ---

static int _cmp_int32(const void* a, const void* b) {
  int32_t x = *(const int32_t*)a;
  int32_t y = *(const int32_t*)b;
  return (x > y) - (x < y);
}

// --- Subset construction ---

static void _determinize(Aut* a, Bitset* initial, NfaTrans* nfa, int nnfa, EpsTrans* eps, int neps, int nstates,
                         int32_t* sa, int32_t sa_n) {
  // Clear existing DFA
  for (int i = 0; i < a->dfa_nstates; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  a->dfa_nstates = 0;
  a->dfa_ntrans = 0;

  // DFA state 0 = epsilon closure of initial
  Bitset* start = _epsilon_closure(initial, eps, neps, nstates, sa, sa_n, NULL);

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
  qsort(splits, (size_t)nsplits, sizeof(int32_t), _cmp_int32);
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

    for (int si = 0; si + 1 < nsplits; si++) {
      int32_t lo = splits[si];
      int32_t hi = splits[si + 1] - 1;

      // Find all NFA transitions that fire on any codepoint in [lo, hi]
      Bitset* target = bitset_new();
      int32_t best_line = 0, best_col = 0;
      int has_line = 0;

      for (int t = 0; t < nnfa; t++) {
        if (nfa[t].cp_start <= lo && nfa[t].cp_end >= hi) {
          uint32_t from = (uint32_t)nfa[t].from;
          if (from < (uint32_t)nstates && bitset_contains(cur_set, from)) {
            bitset_add_bit(target, (uint32_t)nfa[t].to);
            if (!has_line) {
              best_line = nfa[t].line;
              best_col = nfa[t].col;
              has_line = 1;
            }
          }
        }
      }

      if (bitset_size(target) == 0) {
        bitset_del(target);
        continue;
      }

      int32_t action = 0;
      Bitset* closed = _epsilon_closure(target, eps, neps, nstates, sa, sa_n, &action);
      bitset_del(target);

      // Find or create DFA state for closed
      int found = -1;
      for (int d = 0; d < a->dfa_nstates; d++) {
        if (bitset_equal(a->dfa_states[d].nfa_states, closed)) {
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
          .action_id = action,
          .line = best_line,
          .col = best_col,
      };
    }
  }

  free(splits);
}

// --- Simple determinization for when optimize is not called ---

static void _simple_determinize(Aut* a) {
  int nstates = a->max_state + 1;
  Bitset* init = bitset_new();
  bitset_add_bit(init, 0);
  _determinize(a, init, a->nfa_trans, a->nfa_ntrans, a->eps_trans, a->eps_ntrans, nstates, a->state_actions,
               a->state_actions_cap);
  bitset_del(init);
}

// --- Partition refinement (Hopcroft-style) minimization ---
// Determinize first, then merge equivalent DFA states.
// Two states are equivalent if they have the same transitions
// (same target partition and same action_id) for every codepoint interval.

void aut_optimize(Aut* a) {
  if (a->max_state < 0) {
    a->optimized = 1;
    return;
  }

  // First determinize the NFA into a DFA
  _simple_determinize(a);

  int n = a->dfa_nstates;
  if (n <= 1) {
    a->optimized = 1;
    return;
  }

  // Collect all split points from DFA transitions
  int32_t* splits = NULL;
  int nsplits = 0;
  int splits_cap = 0;
  for (int t = 0; t < a->dfa_ntrans; t++) {
    if (nsplits + 2 > splits_cap) {
      splits_cap = splits_cap ? splits_cap * 2 : 128;
      splits = realloc(splits, (size_t)splits_cap * sizeof(int32_t));
    }
    splits[nsplits++] = a->dfa_trans[t].cp_start;
    splits[nsplits++] = a->dfa_trans[t].cp_end + 1;
  }
  qsort(splits, (size_t)nsplits, sizeof(int32_t), _cmp_int32);
  int dedup = 0;
  for (int i = 0; i < nsplits; i++) {
    if (dedup == 0 || splits[dedup - 1] != splits[i]) {
      splits[dedup++] = splits[i];
    }
  }
  nsplits = dedup;
  int nintervals = nsplits > 1 ? nsplits - 1 : 0;

  // For each state and interval, precompute (target_state, action_id).
  // -1 means no transition.
  int32_t* tgt = calloc((size_t)(n * nintervals), sizeof(int32_t));
  int32_t* act = calloc((size_t)(n * nintervals), sizeof(int32_t));
  for (int i = 0; i < n * nintervals; i++) {
    tgt[i] = -1;
  }

  for (int t = 0; t < a->dfa_ntrans; t++) {
    int s = a->dfa_trans[t].from;
    for (int si = 0; si < nintervals; si++) {
      int32_t lo = splits[si];
      int32_t hi = splits[si + 1] - 1;
      if (a->dfa_trans[t].cp_start <= lo && a->dfa_trans[t].cp_end >= hi) {
        tgt[s * nintervals + si] = a->dfa_trans[t].to;
        act[s * nintervals + si] = a->dfa_trans[t].action_id;
      }
    }
  }

  // Partition: partition[s] = partition id for state s
  int32_t* partition = calloc((size_t)n, sizeof(int32_t));
  int npartitions = 0;

  // Initial partition: group by action signature (action_ids on all intervals)
  // Two states with different action vectors must be in different partitions.
  // Use a simple O(n^2) grouping.
  for (int i = 0; i < n; i++) {
    partition[i] = -1;
  }
  for (int i = 0; i < n; i++) {
    if (partition[i] >= 0) {
      continue;
    }
    partition[i] = npartitions;
    for (int j = i + 1; j < n; j++) {
      if (partition[j] >= 0) {
        continue;
      }
      int same = 1;
      for (int si = 0; si < nintervals; si++) {
        if (act[i * nintervals + si] != act[j * nintervals + si]) {
          same = 0;
          break;
        }
      }
      if (same) {
        partition[j] = npartitions;
      }
    }
    npartitions++;
  }

  // Iterative refinement: split partitions where states differ in target partitions
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int p = 0; p < npartitions; p++) {
      // Collect states in this partition
      int pcount = 0;
      for (int s = 0; s < n; s++) {
        if (partition[s] == p) {
          pcount++;
        }
      }
      if (pcount <= 1) {
        continue;
      }
      // Group states by their target-partition vector
      // Compare each pair; states matching the first state stay, others form new groups
      int first = -1;
      for (int s = 0; s < n; s++) {
        if (partition[s] == p) {
          first = s;
          break;
        }
      }
      for (int s = first + 1; s < n; s++) {
        if (partition[s] != p) {
          continue;
        }
        int differs = 0;
        for (int si = 0; si < nintervals; si++) {
          int32_t t1 = tgt[first * nintervals + si];
          int32_t t2 = tgt[s * nintervals + si];
          int32_t p1 = (t1 >= 0) ? partition[t1] : -1;
          int32_t p2 = (t2 >= 0) ? partition[t2] : -1;
          if (p1 != p2) {
            differs = 1;
            break;
          }
        }
        if (differs) {
          // Find or create a new partition for this signature
          // Check if any already-split state from this partition has the same signature
          int found_part = -1;
          for (int s2 = first + 1; s2 < s; s2++) {
            if (partition[s2] < p || partition[s2] == p) {
              continue;
            }
            // s2 was split from p; check if s matches s2
            int match = 1;
            for (int si = 0; si < nintervals; si++) {
              int32_t t1 = tgt[s * nintervals + si];
              int32_t t2 = tgt[s2 * nintervals + si];
              int32_t p1 = (t1 >= 0) ? partition[t1] : -1;
              int32_t p2 = (t2 >= 0) ? partition[t2] : -1;
              if (p1 != p2) {
                match = 0;
                break;
              }
            }
            if (match) {
              found_part = partition[s2];
              break;
            }
          }
          if (found_part >= 0) {
            partition[s] = found_part;
          } else {
            partition[s] = npartitions++;
          }
          changed = 1;
        }
      }
    }
  }

  // Build minimized DFA
  // Map old state -> representative (smallest state in same partition)
  int32_t* repr = calloc((size_t)n, sizeof(int32_t));
  int32_t* new_id = calloc((size_t)n, sizeof(int32_t));
  for (int i = 0; i < n; i++) {
    repr[i] = -1;
    new_id[i] = -1;
  }
  int new_nstates = 0;
  for (int s = 0; s < n; s++) {
    if (repr[partition[s]] < 0) {
      repr[partition[s]] = s;
    }
  }
  // Assign new state ids: ensure state 0 maps to new state 0
  int32_t start_repr = repr[partition[0]];
  new_id[start_repr] = new_nstates++;
  for (int p = 0; p < npartitions; p++) {
    if (repr[p] >= 0 && new_id[repr[p]] < 0) {
      new_id[repr[p]] = new_nstates++;
    }
  }

  // Rebuild transitions: for each transition, remap from/to to new ids.
  // Skip duplicate transitions (same new_from, new_to, cp range, action).
  int new_ntrans = 0;
  DfaTrans* new_trans = malloc((size_t)(a->dfa_ntrans > 0 ? a->dfa_ntrans : 1) * sizeof(DfaTrans));
  for (int t = 0; t < a->dfa_ntrans; t++) {
    int old_from = a->dfa_trans[t].from;
    int old_to = a->dfa_trans[t].to;
    int32_t nf = new_id[repr[partition[old_from]]];
    int32_t nt = new_id[repr[partition[old_to]]];
    // Only emit from the representative state
    if (repr[partition[old_from]] != old_from) {
      continue;
    }
    new_trans[new_ntrans++] = (DfaTrans){
        .from = nf,
        .to = nt,
        .cp_start = a->dfa_trans[t].cp_start,
        .cp_end = a->dfa_trans[t].cp_end,
        .action_id = a->dfa_trans[t].action_id,
        .line = a->dfa_trans[t].line,
        .col = a->dfa_trans[t].col,
    };
  }

  // Replace DFA states
  for (int i = 0; i < a->dfa_nstates; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  a->dfa_nstates = new_nstates;
  if (new_nstates > a->dfa_states_cap) {
    a->dfa_states_cap = new_nstates;
    a->dfa_states = realloc(a->dfa_states, (size_t)a->dfa_states_cap * sizeof(DfaState));
  }
  for (int i = 0; i < new_nstates; i++) {
    a->dfa_states[i] = (DfaState){.nfa_states = bitset_new()};
  }

  // Replace DFA transitions
  free(a->dfa_trans);
  a->dfa_trans = new_trans;
  a->dfa_ntrans = new_ntrans;
  a->dfa_trans_cap = a->dfa_ntrans;

  free(partition);
  free(repr);
  free(new_id);
  free(tgt);
  free(act);
  free(splits);

  // Merge adjacent transitions with same (from, to, action_id) and contiguous ranges
  for (int i = 0; i < a->dfa_ntrans; i++) {
    for (int j = i + 1; j < a->dfa_ntrans; j++) {
      if (a->dfa_trans[i].from == a->dfa_trans[j].from && a->dfa_trans[i].to == a->dfa_trans[j].to &&
          a->dfa_trans[i].action_id == a->dfa_trans[j].action_id &&
          a->dfa_trans[i].cp_end + 1 == a->dfa_trans[j].cp_start) {
        a->dfa_trans[i].cp_end = a->dfa_trans[j].cp_end;
        memmove(&a->dfa_trans[j], &a->dfa_trans[j + 1], (size_t)(a->dfa_ntrans - j - 1) * sizeof(DfaTrans));
        a->dfa_ntrans--;
        j--;
      }
    }
  }

  a->optimized = 1;
}

int32_t aut_dfa_nstates(Aut* a) {
  if (!a->optimized) {
    _simple_determinize(a);
  }
  return a->dfa_nstates;
}

// --- IR generation ---

void aut_gen_dfa(Aut* a, IrWriter* w, bool debug_mode) {
  if (!a->optimized) {
    _simple_determinize(a);
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

    DfaTrans* ft = &a->dfa_trans[first_trans];
    if (ft->line > 0) {
      irwriter_dbg(w, ft->line, ft->col);
    }

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
