#include "aut.h"
#include "bitset.h"
#include "darray.h"
#include "irwriter.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct Aut {
  const char* function_name;
  const char* source_file_name;

  NfaTrans* nfa_trans;
  EpsTrans* eps_trans;
  int32_t* state_actions;
  int32_t max_state;
  DfaState* dfa_states;
  DfaTrans* dfa_trans;

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
  darray_del(a->nfa_trans);
  darray_del(a->eps_trans);
  darray_del(a->state_actions);
  for (size_t i = 0; i < darray_size(a->dfa_states); i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  darray_del(a->dfa_states);
  darray_del(a->dfa_trans);
  free(a);
}

static void _track_state(Aut* a, int32_t s) {
  if (s > a->max_state) {
    a->max_state = s;
  }
}

void aut_transition(Aut* a, TransitionDef tdef, DebugInfo di) {
  if (!a->nfa_trans) {
    a->nfa_trans = darray_new(sizeof(NfaTrans), 0);
  }
  darray_push(a->nfa_trans, ((NfaTrans){
                                .from = tdef.from_state_id,
                                .to = tdef.to_state_id,
                                .cp_start = tdef.cp_start,
                                .cp_end = tdef.cp_end_inclusive,
                                .line = di.source_file_line,
                                .col = di.source_file_col,
                            }));
  _track_state(a, tdef.from_state_id);
  _track_state(a, tdef.to_state_id);
}

void aut_epsilon(Aut* a, int32_t from_state, int32_t to_state) {
  if (!a->eps_trans) {
    a->eps_trans = darray_new(sizeof(EpsTrans), 0);
  }
  darray_push(a->eps_trans, ((EpsTrans){from_state, to_state}));
  _track_state(a, from_state);
  _track_state(a, to_state);
}

void aut_action(Aut* a, int32_t state, int32_t action_id) {
  if (action_id == 0) {
    return;
  }
  _track_state(a, state);
  if (!a->state_actions) {
    a->state_actions = darray_new(sizeof(int32_t), 0);
  }
  size_t needed = (size_t)state + 1;
  if (needed > darray_size(a->state_actions)) {
    a->state_actions = darray_grow(a->state_actions, needed);
  }
  if (a->state_actions[state] == 0 || action_id < a->state_actions[state]) {
    a->state_actions[state] = action_id;
  }
}

static Bitset* _epsilon_closure(Bitset* states, EpsTrans* eps, int neps, int nstates, int32_t* sa, int32_t sa_n,
                                int32_t* out_action) {
  Bitset* result = bitset_or(states, states);
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

static int _cmp_int32(const void* a, const void* b) {
  int32_t x = *(const int32_t*)a;
  int32_t y = *(const int32_t*)b;
  return (x > y) - (x < y);
}

static void _determinize(Aut* a, Bitset* initial, NfaTrans* nfa, int nnfa, EpsTrans* eps, int neps, int nstates,
                         int32_t* sa, int32_t sa_n) {
  for (size_t i = 0; i < darray_size(a->dfa_states); i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  if (a->dfa_states) {
    a->dfa_states = darray_grow(a->dfa_states, 0);
  } else {
    a->dfa_states = darray_new(sizeof(DfaState), 0);
  }
  if (a->dfa_trans) {
    a->dfa_trans = darray_grow(a->dfa_trans, 0);
  } else {
    a->dfa_trans = darray_new(sizeof(DfaTrans), 0);
  }

  Bitset* start = _epsilon_closure(initial, eps, neps, nstates, sa, sa_n, NULL);
  darray_push(a->dfa_states, ((DfaState){.nfa_states = start}));

  int32_t* splits = darray_new(sizeof(int32_t), 0);
  for (int i = 0; i < nnfa; i++) {
    darray_push(splits, nfa[i].cp_start);
    darray_push(splits, (nfa[i].cp_end + 1));
  }
  int nsplits = (int)darray_size(splits);
  qsort(splits, (size_t)nsplits, sizeof(int32_t), _cmp_int32);
  int dedup = 0;
  for (int i = 0; i < nsplits; i++) {
    if (dedup == 0 || splits[dedup - 1] != splits[i]) {
      splits[dedup++] = splits[i];
    }
  }
  nsplits = dedup;

  int worklist_head = 0;
  while (worklist_head < (int)darray_size(a->dfa_states)) {
    int cur_dfa = worklist_head++;
    Bitset* cur_set = a->dfa_states[cur_dfa].nfa_states;

    for (int si = 0; si + 1 < nsplits; si++) {
      int32_t lo = splits[si];
      int32_t hi = splits[si + 1] - 1;

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

      int found = -1;
      for (int d = 0; d < (int)darray_size(a->dfa_states); d++) {
        if (bitset_equal(a->dfa_states[d].nfa_states, closed)) {
          found = d;
          break;
        }
      }
      if (found < 0) {
        found = (int)darray_size(a->dfa_states);
        darray_push(a->dfa_states, ((DfaState){.nfa_states = closed}));
      } else {
        bitset_del(closed);
      }

      darray_push(a->dfa_trans, ((DfaTrans){
                                    .from = cur_dfa,
                                    .to = found,
                                    .cp_start = lo,
                                    .cp_end = hi,
                                    .action_id = action,
                                    .line = best_line,
                                    .col = best_col,
                                }));
    }
  }

  darray_del(splits);
}

static void _simple_determinize(Aut* a) {
  int nstates = a->max_state + 1;
  Bitset* init = bitset_new();
  bitset_add_bit(init, 0);
  _determinize(a, init, a->nfa_trans, (int)darray_size(a->nfa_trans), a->eps_trans, (int)darray_size(a->eps_trans),
               nstates, a->state_actions, (int32_t)darray_size(a->state_actions));
  bitset_del(init);
}

void aut_optimize(Aut* a) {
  if (a->max_state < 0) {
    a->optimized = 1;
    return;
  }

  _simple_determinize(a);

  int n = (int)darray_size(a->dfa_states);
  if (n <= 1) {
    a->optimized = 1;
    return;
  }

  int dfa_ntrans = (int)darray_size(a->dfa_trans);

  int32_t* splits = darray_new(sizeof(int32_t), 0);
  for (int t = 0; t < dfa_ntrans; t++) {
    darray_push(splits, a->dfa_trans[t].cp_start);
    darray_push(splits, (a->dfa_trans[t].cp_end + 1));
  }
  int nsplits = (int)darray_size(splits);
  qsort(splits, (size_t)nsplits, sizeof(int32_t), _cmp_int32);
  int dedup = 0;
  for (int i = 0; i < nsplits; i++) {
    if (dedup == 0 || splits[dedup - 1] != splits[i]) {
      splits[dedup++] = splits[i];
    }
  }
  nsplits = dedup;
  int nintervals = nsplits > 1 ? nsplits - 1 : 0;

  int32_t* tgt = calloc((size_t)(n * nintervals), sizeof(int32_t));
  int32_t* act = calloc((size_t)(n * nintervals), sizeof(int32_t));
  for (int i = 0; i < n * nintervals; i++) {
    tgt[i] = -1;
  }

  for (int t = 0; t < dfa_ntrans; t++) {
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

  int32_t* partition = calloc((size_t)n, sizeof(int32_t));
  int npartitions = 0;

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

  int changed = 1;
  while (changed) {
    changed = 0;
    for (int p = 0; p < npartitions; p++) {
      int pcount = 0;
      for (int s = 0; s < n; s++) {
        if (partition[s] == p) {
          pcount++;
        }
      }
      if (pcount <= 1) {
        continue;
      }
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
          int found_part = -1;
          for (int s2 = first + 1; s2 < s; s2++) {
            if (partition[s2] < p || partition[s2] == p) {
              continue;
            }
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
  int32_t start_repr = repr[partition[0]];
  new_id[start_repr] = new_nstates++;
  for (int p = 0; p < npartitions; p++) {
    if (repr[p] >= 0 && new_id[repr[p]] < 0) {
      new_id[repr[p]] = new_nstates++;
    }
  }

  int new_ntrans = 0;
  DfaTrans* new_trans = malloc((size_t)(dfa_ntrans > 0 ? dfa_ntrans : 1) * sizeof(DfaTrans));
  for (int t = 0; t < dfa_ntrans; t++) {
    int old_from = a->dfa_trans[t].from;
    int old_to = a->dfa_trans[t].to;
    int32_t nf = new_id[repr[partition[old_from]]];
    int32_t nt = new_id[repr[partition[old_to]]];
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

  for (int i = 0; i < n; i++) {
    bitset_del(a->dfa_states[i].nfa_states);
  }
  a->dfa_states = darray_grow(a->dfa_states, (size_t)new_nstates);
  for (int i = 0; i < new_nstates; i++) {
    a->dfa_states[i] = (DfaState){.nfa_states = bitset_new()};
  }

  darray_del(a->dfa_trans);
  a->dfa_trans = darray_new(sizeof(DfaTrans), (size_t)new_ntrans);
  memcpy(a->dfa_trans, new_trans, (size_t)new_ntrans * sizeof(DfaTrans));
  free(new_trans);

  free(partition);
  free(repr);
  free(new_id);
  free(tgt);
  free(act);
  darray_del(splits);

  int final_ntrans = (int)darray_size(a->dfa_trans);
  for (int i = 0; i < final_ntrans; i++) {
    for (int j = i + 1; j < final_ntrans; j++) {
      if (a->dfa_trans[i].from == a->dfa_trans[j].from && a->dfa_trans[i].to == a->dfa_trans[j].to &&
          a->dfa_trans[i].action_id == a->dfa_trans[j].action_id &&
          a->dfa_trans[i].cp_end + 1 == a->dfa_trans[j].cp_start) {
        a->dfa_trans[i].cp_end = a->dfa_trans[j].cp_end;
        memmove(&a->dfa_trans[j], &a->dfa_trans[j + 1], (size_t)(final_ntrans - j - 1) * sizeof(DfaTrans));
        final_ntrans--;
        j--;
      }
    }
  }
  a->dfa_trans = darray_grow(a->dfa_trans, (size_t)final_ntrans);

  a->optimized = 1;
}

int32_t aut_dfa_nstates(Aut* a) {
  if (!a->optimized) {
    _simple_determinize(a);
  }
  return (int32_t)darray_size(a->dfa_states);
}

void aut_gen_dfa(Aut* a, IrWriter* w, bool debug_mode) {
  if (!a->optimized) {
    _simple_determinize(a);
  }

  int dfa_nstates = (int)darray_size(a->dfa_states);
  int dfa_ntrans = (int)darray_size(a->dfa_trans);

  if (debug_mode) {
    irwriter_declare(w, "void", "llvm.debugtrap", "");
  }

  const char* ret_ty = "{i32, i32}";
  const char* arg_types[] = {"i32", "i32"};
  const char* arg_names[] = {"state", "cp"};
  irwriter_define_start(w, a->function_name, ret_ty, 2, arg_types, arg_names);

  // entry BB must be emitted first
  irwriter_bb(w);
  int32_t dead_bb = irwriter_label(w);
  int32_t* state_bbs = malloc((size_t)dfa_nstates * sizeof(int32_t));
  for (int s = 0; s < dfa_nstates; s++) {
    state_bbs[s] = irwriter_label(w);
  }

  irwriter_switch_start(w, "i32", irwriter_imm(w, "%state"), dead_bb);
  for (int s = 0; s < dfa_nstates; s++) {
    irwriter_switch_case(w, "i32", s, state_bbs[s]);
  }
  irwriter_switch_end(w);

  for (int s = 0; s < dfa_nstates; s++) {
    int first_trans = -1;
    int ntrans_from = 0;
    for (int t = 0; t < dfa_ntrans; t++) {
      if (a->dfa_trans[t].from == s) {
        if (first_trans < 0) {
          first_trans = t;
        }
        ntrans_from++;
      }
    }

    int has_switch = 0;
    int has_range = 0;
    for (int t = 0; t < dfa_ntrans; t++) {
      if (a->dfa_trans[t].from != s) {
        continue;
      }
      if (a->dfa_trans[t].cp_start == a->dfa_trans[t].cp_end) {
        has_switch++;
      } else {
        has_range++;
      }
    }

    // Reserve per-state labels
    int32_t nomatch_bb = irwriter_label(w);
    int32_t ranges_bb = has_range > 0 ? irwriter_label(w) : -1;

    int32_t* trans_bbs = malloc((size_t)dfa_ntrans * sizeof(int32_t));
    for (int t = 0; t < dfa_ntrans; t++) {
      if (a->dfa_trans[t].from == s) {
        trans_bbs[t] = irwriter_label(w);
      } else {
        trans_bbs[t] = -1;
      }
    }

    int nranges = 0;
    for (int t = 0; t < dfa_ntrans; t++) {
      if (a->dfa_trans[t].from == s && a->dfa_trans[t].cp_start != a->dfa_trans[t].cp_end) {
        nranges++;
      }
    }
    int32_t* rck_bbs = NULL;
    if (nranges > 1) {
      rck_bbs = malloc((size_t)nranges * sizeof(int32_t));
      for (int i = 0; i < nranges; i++) {
        rck_bbs[i] = irwriter_label(w);
      }
    }

    // Emit state BB
    irwriter_bb_at(w, state_bbs[s]);

    if (ntrans_from == 0) {
      irwriter_br(w, nomatch_bb);
      irwriter_bb_at(w, nomatch_bb);

      IrVal state_val = irwriter_imm(w, "%state");
      IrVal v0 = irwriter_insertvalue(w, ret_ty, -1, "i32", state_val, 0);
      IrVal v1 = irwriter_insertvalue(w, ret_ty, v0, "i32", irwriter_imm(w, "-2"), 1);
      irwriter_ret(w, ret_ty, v1);
      free(trans_bbs);
      free(rck_bbs);
      continue;
    }

    DfaTrans* ft = &a->dfa_trans[first_trans];
    if (ft->line > 0) {
      irwriter_dbg(w, ft->line, ft->col);
    }

    if (has_switch > 0) {
      int32_t sw_default = has_range > 0 ? ranges_bb : nomatch_bb;
      irwriter_switch_start(w, "i32", irwriter_imm(w, "%cp"), sw_default);

      for (int t = 0; t < dfa_ntrans; t++) {
        if (a->dfa_trans[t].from != s || a->dfa_trans[t].cp_start != a->dfa_trans[t].cp_end) {
          continue;
        }
        irwriter_switch_case(w, "i32", a->dfa_trans[t].cp_start, trans_bbs[t]);
      }
      irwriter_switch_end(w);
    } else if (has_range > 0) {
      irwriter_br(w, ranges_bb);
    }

    if (has_range > 0) {
      irwriter_bb_at(w, ranges_bb);
      IrVal cp_val = irwriter_imm(w, "%cp");

      int range_idx = 0;
      for (int t = 0; t < dfa_ntrans; t++) {
        if (a->dfa_trans[t].from != s || a->dfa_trans[t].cp_start == a->dfa_trans[t].cp_end) {
          continue;
        }

        DfaTrans* dt = &a->dfa_trans[t];
        if (dt->line > 0) {
          irwriter_dbg(w, dt->line, dt->col);
        }

        int has_next_range = (range_idx + 1 < nranges);

        IrVal lo_n = irwriter_icmp(w, "sge", "i32", cp_val, irwriter_imm_int(w, dt->cp_start));
        IrVal hi_n = irwriter_icmp(w, "sle", "i32", cp_val, irwriter_imm_int(w, dt->cp_end));
        if (has_next_range) {
          irwriter_br_cond(w, irwriter_binop(w, "and", "i1", lo_n, hi_n), trans_bbs[t], rck_bbs[range_idx + 1]);
          irwriter_bb_at(w, rck_bbs[range_idx + 1]);
        } else {
          irwriter_br_cond(w, irwriter_binop(w, "and", "i1", lo_n, hi_n), trans_bbs[t], nomatch_bb);
        }

        range_idx++;
      }
    }

    for (int t = 0; t < dfa_ntrans; t++) {
      if (a->dfa_trans[t].from != s) {
        continue;
      }
      DfaTrans* dt = &a->dfa_trans[t];

      irwriter_bb_at(w, trans_bbs[t]);

      if (dt->line > 0) {
        irwriter_dbg(w, dt->line, dt->col);
      }

      IrVal v0 = irwriter_insertvalue(w, ret_ty, -1, "i32", irwriter_imm_int(w, dt->to), 0);
      IrVal v1 = irwriter_insertvalue(w, ret_ty, v0, "i32", irwriter_imm_int(w, dt->action_id), 1);
      irwriter_ret(w, ret_ty, v1);
    }

    // nomatch BB
    irwriter_bb_at(w, nomatch_bb);
    IrVal state_val = irwriter_imm(w, "%state");
    IrVal v0 = irwriter_insertvalue(w, ret_ty, -1, "i32", state_val, 0);
    IrVal v1 = irwriter_insertvalue(w, ret_ty, v0, "i32", irwriter_imm(w, "-2"), 1);
    irwriter_ret(w, ret_ty, v1);

    free(trans_bbs);
    free(rck_bbs);
  }

  // dead BB
  irwriter_bb_at(w, dead_bb);
  {
    if (debug_mode) {
      irwriter_call_void_fmtf(w, "llvm.debugtrap", "");
    }
    IrVal state_val = irwriter_imm(w, "%state");
    IrVal v0 = irwriter_insertvalue(w, ret_ty, -1, "i32", state_val, 0);
    IrVal v1 = irwriter_insertvalue(w, ret_ty, v0, "i32", irwriter_imm(w, "-2"), 1);
    irwriter_ret(w, ret_ty, v1);
  }

  free(state_bbs);
  irwriter_define_end(w);
}
