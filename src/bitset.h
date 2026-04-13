#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct Bitset Bitset;

Bitset* bitset_new(void);
void bitset_del(Bitset* bs);
void bitset_add_bit(Bitset* bs, uint32_t offset);
void bitset_clear_bit(Bitset* bs, uint32_t offset);
bool bitset_contains(Bitset* bs, uint32_t offset);
Bitset* bitset_or(Bitset* s1, Bitset* s2);
void bitset_or_into(Bitset* dest, Bitset* src);
Bitset* bitset_and(Bitset* s1, Bitset* s2);
uint32_t bitset_size(Bitset* bs);
bool bitset_equal(Bitset* a, Bitset* b);
