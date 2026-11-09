lib "ustr",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c src/xmalloc.c]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_xmalloc",
  srcs: %w[test/test_xmalloc.c src/xmalloc.c]

exe "test_bitset",
  srcs: %w[test/test_bitset.c src/bitset.c src/xmalloc.c]

exe "test_symtab",
  srcs: %w[test/test_symtab.c src/symtab.c src/darray.c src/xmalloc.c]

exe "test_aut",
  srcs: %w[test/test_aut.c src/aut.c src/bitset.c src/darray.c src/xmalloc.c]

exe "test_re",
  srcs: %w[test/test_re.c src/re.c src/aut.c src/bitset.c src/darray.c src/xmalloc.c],
  deps: %w[ustr]

exe "test_re_ir",
  srcs: %w[test/test_re_ir.c src/re.c src/re_ir.c src/aut.c src/bitset.c src/darray.c src/xmalloc.c],
  deps: %w[ustr]

debug    cflags: "-O0 -g -fsanitize=address -fsanitize=undefined -DXMALLOC_TRACE"
release  cflags: "-O2"
coverage cflags: "-O0 -g -fprofile-instr-generate -fcoverage-mapping"
