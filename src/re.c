#include "re.h"
#include "darray.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNICODE 0x10FFFF

ReRange* re_range_new(void) { return calloc(1, sizeof(ReRange)); }

void re_range_del(ReRange* range) {
  if (!range) {
    return;
  }
  darray_del(range->ivs);
  free(range);
}

void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp) {
  assert(start_cp <= end_cp);
  assert(start_cp >= 0 && end_cp <= MAX_UNICODE);

  if (!range->ivs) {
    range->ivs = darray_new(sizeof(ReInterval), 0);
  }

  int32_t len = (int32_t)darray_size(range->ivs);
  int32_t lo = 0, hi = len;
  while (lo < hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (range->ivs[mid].end < start_cp - 1) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  int32_t first = lo;
  int32_t last = first;
  while (last < len && range->ivs[last].start <= end_cp + 1) {
    last++;
  }

  if (first == last) {
    range->ivs = darray_grow(range->ivs, (size_t)(len + 1));
    memmove(&range->ivs[first + 1], &range->ivs[first], (size_t)(len - first) * sizeof(ReInterval));
    range->ivs[first] = (ReInterval){start_cp, end_cp};
  } else {
    int32_t merged_start = start_cp < range->ivs[first].start ? start_cp : range->ivs[first].start;
    int32_t merged_end = end_cp > range->ivs[last - 1].end ? end_cp : range->ivs[last - 1].end;
    range->ivs[first] = (ReInterval){merged_start, merged_end};
    int32_t removed = last - first - 1;
    if (removed > 0) {
      memmove(&range->ivs[first + 1], &range->ivs[last], (size_t)(len - last) * sizeof(ReInterval));
      range->ivs = darray_grow(range->ivs, (size_t)(len - removed));
    }
  }
}

void re_range_neg(ReRange* range) {
  ReInterval* gaps = darray_new(sizeof(ReInterval), 0);
  int32_t pos = 0;

  for (int32_t i = 0; i < (int32_t)darray_size(range->ivs); i++) {
    if (pos < range->ivs[i].start) {
      darray_push(gaps, ((ReInterval){pos, range->ivs[i].start - 1}));
    }
    pos = range->ivs[i].end + 1;
  }
  if (pos <= MAX_UNICODE) {
    darray_push(gaps, ((ReInterval){pos, MAX_UNICODE}));
  }

  darray_del(range->ivs);
  range->ivs = gaps;
  range->negated = !range->negated;
}

void re_range_ic(ReRange* range) {
  bool was_neg = range->negated;
  if (was_neg) {
    re_range_neg(range);
  }

  int32_t n = (int32_t)darray_size(range->ivs);
  for (int32_t i = 0; i < n; i++) {
    int32_t lo = range->ivs[i].start;
    int32_t hi = range->ivs[i].end;
    // if overlaps a-z, add A-Z counterpart
    if (lo <= 'z' && hi >= 'a') {
      int32_t clo = (lo > 'a' ? lo : 'a') - 32;
      int32_t chi = (hi < 'z' ? hi : 'z') - 32;
      re_range_add(range, clo, chi);
    }
    // if overlaps A-Z, add a-z counterpart
    if (lo <= 'Z' && hi >= 'A') {
      int32_t clo = (lo > 'A' ? lo : 'A') + 32;
      int32_t chi = (hi < 'Z' ? hi : 'Z') + 32;
      re_range_add(range, clo, chi);
    }
  }

  if (was_neg) {
    re_range_neg(range);
  }
}

typedef struct {
  int32_t start_state;
  int32_t cur_state;
  int32_t* branch_ends;
} GroupFrame;

struct Re {
  Aut* aut;
  int32_t next_state;
  GroupFrame* stack;
};

static int32_t _alloc_state(Re* re) { return re->next_state++; }

static GroupFrame* _top(Re* re) {
  size_t sz = darray_size(re->stack);
  assert(sz > 0);
  return &re->stack[sz - 1];
}

static void _push_frame(Re* re, int32_t start, int32_t cur) {
  darray_push(re->stack, ((GroupFrame){
                             .start_state = start,
                             .cur_state = cur,
                             .branch_ends = NULL,
                         }));
}

static void _save_branch_end(GroupFrame* f, int32_t state) {
  if (!f->branch_ends) {
    f->branch_ends = darray_new(sizeof(int32_t), 0);
  }
  darray_push(f->branch_ends, state);
}

Re* re_new(Aut* aut) {
  Re* re = calloc(1, sizeof(Re));
  re->aut = aut;
  re->next_state = 0;
  re->stack = darray_new(sizeof(GroupFrame), 0);
  _alloc_state(re);
  _push_frame(re, 0, 0);
  return re;
}

void re_del(Re* re) {
  if (!re) {
    return;
  }
  size_t sz = darray_size(re->stack);
  for (size_t i = 0; i < sz; i++) {
    darray_del(re->stack[i].branch_ends);
  }
  darray_del(re->stack);
  free(re);
}

void re_append_ch(Re* re, int32_t codepoint, DebugInfo di) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint, codepoint}, di);
  f->cur_state = s;
}

void re_append_ch_ic(Re* re, int32_t codepoint, DebugInfo di) {
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint, codepoint}, di);
  if (codepoint >= 'a' && codepoint <= 'z') {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint - 32, codepoint - 32}, di);
  } else if (codepoint >= 'A' && codepoint <= 'Z') {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, codepoint + 32, codepoint + 32}, di);
  }
  f->cur_state = s;
}

void re_append_range(Re* re, ReRange* range, DebugInfo di) {
  assert(darray_size(range->ivs) > 0);
  GroupFrame* f = _top(re);
  int32_t s = _alloc_state(re);
  for (int32_t i = 0; i < (int32_t)darray_size(range->ivs); i++) {
    aut_transition(re->aut, (TransitionDef){f->cur_state, s, range->ivs[i].start, range->ivs[i].end}, di);
  }
  f->cur_state = s;
}

void re_append_group_s(Re* re, ReRange* range) {
  (void)re;
  re_range_add(range, '\t', '\r');
  re_range_add(range, ' ', ' ');
}

void re_append_group_d(Re* re, ReRange* range) {
  (void)re;
  re_range_add(range, '0', '9');
}

void re_append_group_w(Re* re, ReRange* range) {
  (void)re;
  re_range_add(range, '0', '9');
  re_range_add(range, 'A', 'Z');
  re_range_add(range, '_', '_');
  re_range_add(range, 'a', 'z');
}

void re_append_group_h(Re* re, ReRange* range) {
  (void)re;
  re_range_add(range, '0', '9');
  re_range_add(range, 'A', 'F');
  re_range_add(range, 'a', 'f');
}

void re_append_group_dot(Re* re, ReRange* range) {
  (void)re;
  re_range_add(range, 0, 9);
  re_range_add(range, 11, MAX_UNICODE);
}

int32_t re_c_escape(char symbol) {
  switch (symbol) {
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  case 'v':
    return '\v';
  default:
    return -1;
  }
}

int32_t re_hex_to_codepoint(const char* h, size_t size) {
  int32_t cp = 0;
  for (size_t i = 0; i < size; i++) {
    cp <<= 4;
    char c = h[i];
    if (c >= '0' && c <= '9') {
      cp |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      cp |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      cp |= c - 'A' + 10;
    }
  }
  return cp;
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
  size_t sz = darray_size(re->stack);
  assert(sz > 1);
  GroupFrame* f = &re->stack[sz - 1];
  _save_branch_end(f, f->cur_state);

  int32_t exit_state = _alloc_state(re);
  for (int32_t i = 0; i < (int32_t)darray_size(f->branch_ends); i++) {
    aut_epsilon(re->aut, f->branch_ends[i], exit_state);
  }

  darray_del(f->branch_ends);
  re->stack = darray_grow(re->stack, sz - 1);

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
