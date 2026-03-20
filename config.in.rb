lib "ustr",
  srcs: %w[src/ustr.c src/ustr_neon.c]

lib "irwriter",
  srcs: %w[src/irwriter.c]

lib "bitset",
  srcs: %w[src/bitset.c]

lib "aut",
  srcs: %w[src/aut.c]

lib "re",
  srcs: %w[src/re.c]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_irwriter",
  srcs: %w[test/test_irwriter.c],
  deps: %w[irwriter]

exe "test_bitset",
  srcs: %w[test/test_bitset.c],
  deps: %w[bitset]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_aut",
  srcs: %w[test/test_aut.c],
  deps: %w[aut irwriter bitset]

exe "test_re",
  srcs: %w[test/test_re.c],
  deps: %w[re aut irwriter bitset]

debug   cflags: "-O0 -g -fsanitize=address -fsanitize=undefined"
release cflags: "-O2"
