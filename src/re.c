#include "re.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNICODE 0x10FFFF

// --- ReRange ---

ReRange* re_range_new(void) { return calloc(1, sizeof(ReRange)); }

void re_range_del(ReRange* range) {
  if (!range) {
    return;
  }
  free(range->ivs);
  free(range);
}

void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp) {
  assert(start_cp <= end_cp);
  assert(start_cp >= 0 && end_cp <= MAX_UNICODE);

  // find first interval that overlaps or is adjacent (iv.end >= start_cp - 1)
  int32_t lo = 0, hi = range->len;
  while (lo < hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (range->ivs[mid].end < start_cp - 1) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  // lo = first interval that could merge

  // find last interval that overlaps or is adjacent (iv.start <= end_cp + 1)
  int32_t first = lo;
  int32_t last = first; // exclusive
  while (last < range->len && range->ivs[last].start <= end_cp + 1) {
    last++;
  }

  if (first == last) {
    // no overlap, insert new interval at position first
    if (range->len == range->cap) {
      range->cap = range->cap ? range->cap * 2 : 8;
      range->ivs = realloc(range->ivs, (size_t)range->cap * sizeof(ReInterval));
    }
    memmove(&range->ivs[first + 1], &range->ivs[first], (size_t)(range->len - first) * sizeof(ReInterval));
    range->ivs[first] = (ReInterval){start_cp, end_cp};
    range->len++;
  } else {
    // merge [first, last) into one interval
    int32_t merged_start = start_cp < range->ivs[first].start ? start_cp : range->ivs[first].start;
    int32_t merged_end = end_cp > range->ivs[last - 1].end ? end_cp : range->ivs[last - 1].end;
    range->ivs[first] = (ReInterval){merged_start, merged_end};
    int32_t removed = last - first - 1;
    if (removed > 0) {
      memmove(&range->ivs[first + 1], &range->ivs[last], (size_t)(range->len - last) * sizeof(ReInterval));
      range->len -= removed;
    }
  }
}

void re_range_neg(ReRange* range) {
  // collect gaps in [0, MAX_UNICODE]
  int32_t gap_cap = range->len + 1;
  ReInterval* gaps = malloc((size_t)gap_cap * sizeof(ReInterval));
  int32_t ngaps = 0;
  int32_t pos = 0;

  for (int32_t i = 0; i < range->len; i++) {
    if (pos < range->ivs[i].start) {
      gaps[ngaps++] = (ReInterval){pos, range->ivs[i].start - 1};
    }
    pos = range->ivs[i].end + 1;
  }
  if (pos <= MAX_UNICODE) {
    gaps[ngaps++] = (ReInterval){pos, MAX_UNICODE};
  }

  free(range->ivs);
  range->ivs = gaps;
  range->len = ngaps;
  range->cap = gap_cap;
}

// --- Re builder ---

typedef struct {
  int32_t start_state;
  int32_t cur_state;
  int32_t* branch_ends;
  int32_t nbranch_ends;
  int32_t branch_ends_cap;
} GroupFrame;

struct Re {
  Aut* aut;
  int32_t next_state;
  GroupFrame* stack;
  int32_t stack_sz;
  int32_t stack_cap;
};

static int32_t _alloc_state(Re* re) { return re->next_state++; }

static GroupFrame* _top(Re* re) {
  assert(re->stack_sz > 0);
  return &re->stack[re->stack_sz - 1];
}

static void _push_frame(Re* re, int32_t start, int32_t cur) {
  if (re->stack_sz == re->stack_cap) {
    re->stack_cap = re->stack_cap ? re->stack_cap * 2 : 8;
    re->stack = realloc(re->stack, (size_t)re->stack_cap * sizeof(GroupFrame));
  }
  re->stack[re->stack_sz++] = (GroupFrame){
      .start_state = start,
      .cur_state = cur,
      .branch_ends = NULL,
      .nbranch_ends = 0,
      .branch_ends_cap = 0,
  };
}

static void _save_branch_end(GroupFrame* f, int32_t state) {
  if (f->nbranch_ends == f->branch_ends_cap) {
    f->branch_ends_cap = f->branch_ends_cap ? f->branch_ends_cap * 2 : 4;
    f->branch_ends = realloc(f->branch_ends, (size_t)f->branch_ends_cap * sizeof(int32_t));
  }
  f->branch_ends[f->nbranch_ends++] = state;
}

Re* re_new(Aut* aut) {
  Re* re = calloc(1, sizeof(Re));
  re->aut = aut;
  re->next_state = 0;
  _alloc_state(re);
  _push_frame(re, 0, 0);
  return re;
}

void re_del(Re* re) {
  if (!re) {
    return;
  }
  for (int32_t i = 0; i < re->stack_sz; i++) {
    free(re->stack[i].branch_ends);
  }
  free(re->stack);
  free(re);
}

void re_append_ch(Re* re, int32_t codepoint, DebugInfo di) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint, codepoint}, di);
  f->cur_state = s;
}

void re_append_range(Re* re, ReRange* range, DebugInfo di) {
  assert(range->len > 0);
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  for (int32_t i = 0; i < range->len; i++) {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, range->ivs[i].start, range->ivs[i].end}, di);
  }
  f->cur_state = s;
}

void re_lparen(Re* re) {
  GroupFrame* f = _top(re);
  int32_t start = f->cur_state;
  int32_t branch = _alloc_state(re);
  aut_epsilon(re->aut, start, branch);
  _push_frame(re, start, branch);
}

void re_fork(Re* re) {
  GroupFrame* f = _top(re);
  _save_branch_end(f, f->cur_state);
  int32_t branch = _alloc_state(re);
  aut_epsilon(re->aut, f->start_state, branch);
  f->cur_state = branch;
}

void re_rparen(Re* re) {
  assert(re->stack_sz > 1);
  GroupFrame* f = _top(re);
  _save_branch_end(f, f->cur_state);

  int32_t exit_state = _alloc_state(re);
  for (int32_t i = 0; i < f->nbranch_ends; i++) {
    aut_epsilon(re->aut, f->branch_ends[i], exit_state);
  }

  free(f->branch_ends);
  re->stack_sz--;

  GroupFrame* parent = _top(re);
  parent->cur_state = exit_state;
}

void re_action(Re* re, int32_t action_id) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_epsilon(re->aut, f->cur_state, s);
  aut_action(re->aut, s, action_id);
  f->cur_state = s;
}

int32_t re_cur_state(Re* re) {
  GroupFrame* f = _top(re);
  return f->cur_state;
}
