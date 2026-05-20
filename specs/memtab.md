Memoize table management for generated PEG parsers.

Files include
- src/memtab.h
- src/memtab.c

### Interface

```c
typedef struct {
  // fields for furthest reach
  int64_t sizeof_col;          // byte size of one PEG memoize column; 0 before PEG table allocation
  int64_t slots_offset_in_i32; // slots_start_ptr = (int32_t*)col + memoize_slots_offset_in_i32
  int64_t slots_size;          // physical slot count in one memoize column

  int32_t has_parse_error;
  int32_t size;
  int32_t t[];
} MemoizeTable;

MemoizeTable* memtab_new(size_t size, int64_t sizeof_col, int64_t slots_offset_in_i32, int64_t slots_size);

int32_t* memtab_col(MemoizeTable* mt, size_t col);

void memtab_del(MemoizeTable* mt);
```
