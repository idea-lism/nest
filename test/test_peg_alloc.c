// specs/peg_alloc.md
#include "../src/bitset.h"
#include "../src/darray.h"
#include "../src/peg.h"
#include "../src/symtab.h"
#include "compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s...", #name);                                                                                          \
    name();                                                                                                            \
    printf(" OK\n");                                                                                                   \
  } while (0)

// ============================================================
// Helpers — build a ScopeClosure directly (no parser, no peg_analyze).
// We exercise peg_alloc_scope() in isolation.
// ============================================================

// Build a closure with `rule_size` rules, each: body = @term(token_ids[i]),
// tag_counts[i] tags. Rules are pairwise non-exclusive when the caller wants
// them to share a color (by sharing the same token id).
static ScopeClosure _make_closure(const char* scope_name, int32_t rule_size, const int32_t* token_ids,
                                  const int32_t* tag_counts) {
  ScopeClosure cl = {0};
  cl.scope_name = scope_name;
  cl.scope_id = 0;
  symtab_init(&cl.scoped_rule_names, 0);
  cl.scoped_rules = darray_new(sizeof(ScopedRule), 0);

  for (int32_t i = 0; i < rule_size; i++) {
    char name[64];
    snprintf(name, sizeof(name), "r%d", i);
    symtab_intern(&cl.scoped_rule_names, name);
  }

  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule sr = {0};
    sr.scoped_rule_name = symtab_get(&cl.scoped_rule_names, i);
    sr.body.kind = SCOPED_UNIT_TERM;
    sr.body.as.term_id = token_ids[i];
    sr.body.tag_bit_local_offset = -1;
    sr.original_global_id = i;

    symtab_init(&sr.tags, 0);
    for (int32_t t = 0; t < tag_counts[i]; t++) {
      char tname[32];
      snprintf(tname, sizeof(tname), "t%d", t);
      symtab_intern(&sr.tags, tname);
    }
    sr.call_sites = darray_new(sizeof(CallSite), 0);
    darray_push(cl.scoped_rules, sr);
  }
  return cl;
}

static void _free_closure(ScopeClosure* cl) {
  for (int32_t i = 0; i < (int32_t)darray_size(cl->scoped_rules); i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    symtab_free(&sr->tags);
    if (sr->first_set) {
      bitset_del(sr->first_set);
    }
    if (sr->last_set) {
      bitset_del(sr->last_set);
    }
    darray_del(sr->call_sites);
  }
  darray_del(cl->scoped_rules);
  symtab_free(&cl->scoped_rule_names);
}

// Count used bits per bucket. For each rule: contributes `tag_bit_count` bits
// at `tag_bit_index`; in shared mode also contributes `slot_bit_count` bits at
// `segment_index` (popcount of rule_bit_mask counted per-rule, summed per seg).
typedef struct {
  int32_t* used_in_bucket; // darray<int32_t>, size = bits_bucket_size
} BucketUsage;

static BucketUsage _compute_usage_shared(ScopeClosure* cl) {
  BucketUsage u = {.used_in_bucket = darray_new(sizeof(int32_t), 0)};
  int64_t n = cl->bits_bucket_size;
  u.used_in_bucket = darray_grow2(u.used_in_bucket, (size_t)n, (size_t)n);
  for (int64_t i = 0; i < n; i++) {
    u.used_in_bucket[i] = 0;
  }

  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);

  // Slot bits: one set per segment (use rule_bit_mask popcount summed).
  // Safer: use segment_mask's popcount once per seen segment.
  int32_t seen_size = (int32_t)cl->slots_size;
  bool* seg_seen = calloc((size_t)seen_size, sizeof(bool));
  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    int32_t color = sr->segment_color;
    if (color >= 0 && color < seen_size && !seg_seen[color]) {
      seg_seen[color] = true;
      int32_t slot_bits = __builtin_popcountll(sr->segment_mask);
      u.used_in_bucket[sr->segment_index] += slot_bits;
    }
  }
  free(seg_seen);

  // Tag bits: each rule adds tag_bit_count bits starting at tag_bit_index + tag_bit_offset.
  // Bits may span multiple buckets (big rules).
  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    if (sr->tag_bit_count <= 0) {
      continue;
    }
    int32_t remain = sr->tag_bit_count;
    int32_t bucket = sr->tag_bit_index;
    int32_t offset = sr->tag_bit_offset;
    while (remain > 0) {
      int32_t fit = 64 - offset;
      int32_t take = remain < fit ? remain : fit;
      u.used_in_bucket[bucket] += take;
      remain -= take;
      bucket++;
      offset = 0;
    }
  }
  return u;
}

static BucketUsage _compute_usage_naive(ScopeClosure* cl) {
  BucketUsage u = {.used_in_bucket = darray_new(sizeof(int32_t), 0)};
  int64_t n = cl->bits_bucket_size;
  u.used_in_bucket = darray_grow2(u.used_in_bucket, (size_t)n, (size_t)n);
  for (int64_t i = 0; i < n; i++) {
    u.used_in_bucket[i] = 0;
  }

  int32_t rule_size = (int32_t)darray_size(cl->scoped_rules);
  for (int32_t i = 0; i < rule_size; i++) {
    ScopedRule* sr = &cl->scoped_rules[i];
    if (sr->tag_bit_count <= 0) {
      continue;
    }
    int32_t remain = sr->tag_bit_count;
    int32_t bucket = sr->tag_bit_index;
    int32_t offset = sr->tag_bit_offset;
    while (remain > 0) {
      int32_t fit = 64 - offset;
      int32_t take = remain < fit ? remain : fit;
      u.used_in_bucket[bucket] += take;
      remain -= take;
      bucket++;
      offset = 0;
    }
  }
  return u;
}

// The greedy-knapsack invariant: for every bucket b with < 64 used bits, its
// free space must not be enough to absorb the *smallest* non-full contribution
// that was placed into another bucket. In practice: if two buckets each have
// free space, and their used bits together fit in 64, packing is suboptimal.
static void _assert_no_large_gap(BucketUsage* u) {
  int64_t n = (int64_t)darray_size(u->used_in_bucket);
  for (int64_t i = 0; i < n; i++) {
    for (int64_t j = i + 1; j < n; j++) {
      int32_t ui = u->used_in_bucket[i];
      int32_t uj = u->used_in_bucket[j];
      if (ui + uj <= 64) {
        fprintf(stderr,
                "\n    bucket[%lld] used=%d, bucket[%lld] used=%d; sum=%d <= 64 "
                "-- these could be packed into one bucket\n",
                (long long)i, ui, (long long)j, uj, ui + uj);
        assert(0 && "packing gap: two buckets could have been merged");
      }
    }
  }
}

static void _free_usage(BucketUsage* u) { darray_del(u->used_in_bucket); }

// ============================================================
// Tests
// ============================================================

// Two non-exclusive rules, no tags. Each ends up in its own color segment
// (slot_bits=1 per seg). Currently layout uses 2 buckets, but both segments'
// 1-bit slot data together only need 2 bits -> should fit in 1 bucket.
TEST(test_shared_two_segments_pack_into_one_bucket) {
  int32_t token_ids[] = {1, 1}; // overlapping first_set -> not exclusive
  int32_t tag_counts[] = {0, 0};
  ScopeClosure cl = _make_closure("s", 2, token_ids, tag_counts);

  peg_alloc_scope(&cl, MEMOIZE_SHARED);

  // 2 segments expected (rules interfere).
  assert(cl.slots_size == 2);

  BucketUsage u = _compute_usage_shared(&cl);
  _assert_no_large_gap(&u);

  _free_usage(&u);
  _free_closure(&cl);
}

// Four segments each with 1 slot bit and no tags. Together 4 bits -> 1 bucket.
TEST(test_shared_four_tiny_segments_pack_into_one_bucket) {
  int32_t token_ids[] = {1, 1, 1, 1}; // all interfere -> 4 colors
  int32_t tag_counts[] = {0, 0, 0, 0};
  ScopeClosure cl = _make_closure("s", 4, token_ids, tag_counts);

  peg_alloc_scope(&cl, MEMOIZE_SHARED);
  assert(cl.slots_size == 4);

  BucketUsage u = _compute_usage_shared(&cl);
  _assert_no_large_gap(&u);

  _free_usage(&u);
  _free_closure(&cl);
}

// Naive mode: multiple small tag rules already knapsack-packed — sanity check.
TEST(test_naive_small_tags_pack_tight) {
  int32_t token_ids[] = {1, 2, 3};
  int32_t tag_counts[] = {10, 10, 10}; // 30 bits total -> 1 bucket
  ScopeClosure cl = _make_closure("s", 3, token_ids, tag_counts);

  peg_alloc_scope(&cl, MEMOIZE_NAIVE);

  BucketUsage u = _compute_usage_naive(&cl);
  _assert_no_large_gap(&u);

  _free_usage(&u);
  _free_closure(&cl);
}

int main(void) {
  RUN(test_shared_two_segments_pack_into_one_bucket);
  RUN(test_shared_four_tiny_segments_pack_into_one_bucket);
  RUN(test_naive_small_tags_pack_tight);
  printf("all ok\n");
  return 0;
}
