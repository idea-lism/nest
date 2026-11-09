A bitset tool for DFA building.

Internal storage is `uint64_t` chunks.

```c
typedef struct Bitset Bitset;

Bitset* bitset_new(void);
void bitset_del(Bitset* bs);

void bitset_add_bit(Bitset* bs, uint32_t offset);   // grow as needed
void bitset_clear_bit(Bitset* bs, uint32_t offset);
bool bitset_contains(Bitset* bs, uint32_t offset);

// Return a newly allocated union / intersection.
Bitset* bitset_or(Bitset* s1, Bitset* s2);
Bitset* bitset_and(Bitset* s1, Bitset* s2);

// In-place union; grows `dest` as needed.
void bitset_or_into(Bitset* dest, Bitset* src);

// Popcount.
uint32_t bitset_size(Bitset* bs);

// Same logical bits, regardless of allocated capacity.
bool bitset_equal(Bitset* a, Bitset* b);
```

Add more operations only when automata construction needs them.
