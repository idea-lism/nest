base = (Dir.glob "src/*.c") - ["src/nest.c", "src/parse_gen.c", "src/ustr.c", "src/ustr_neon.c", "src/ustr_avx.c"]
base_lean = base - %w[src/parse.c src/post_process.c]
kissat = IS_WINDOWS ? [] : %w[build/kissat/build/libkissat.a]
nest_lex = ["#{BUILDDIR}/nest_lex.o"]

lib "ustr",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_irwriter",
  srcs: %w[test/test_irwriter.c test/compat.c src/irwriter.c src/darray.c]

exe "test_bitset",
  srcs: %w[test/test_bitset.c src/bitset.c]

exe "test_aut",
  srcs: %w[test/test_aut.c test/compat.c src/aut.c src/irwriter.c src/bitset.c src/darray.c]

exe "test_re",
  srcs: %w[test/test_re.c test/compat.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c]

exe "test_parse_gen",
  srcs: %w[test/test_parse_gen.c test/compat.c]

exe "test_token_chunk",
  srcs: %w[test/test_token_chunk.c src/token_chunk.c src/darray.c],
  deps: %w[ustr]

exe "test_coloring",
  srcs: %w[test/test_coloring.c src/coloring.c src/graph.c] - %w[src/parse.c],
  ext_libs: kissat

exe "test_peg",
  srcs: base_lean + %w[test/test_peg.c test/compat.c],
  deps: %w[ustr],
  ext_libs: kissat

exe "test_vpa",
  srcs: base_lean + %w[test/test_vpa.c test/compat.c],
  deps: %w[ustr],
  ext_libs: kissat

exe "test_parse",
  srcs: base + %w[test/test_parse.c test/compat.c],
  deps: %w[ustr],
  extra_objs: nest_lex,
  ext_libs: kissat

exe "nest",
  srcs: base + %w[src/nest.c],
  deps: %w[ustr],
  extra_objs: nest_lex,
  ext_libs: kissat

exe "parse_gen",
  srcs: %w[src/parse_gen.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c],
  deps: %w[ustr]

amalgamate input: "src/re_rt.h.in",
  output: "out/re_rt.h",
  include_dirs: %w[src]

debug    cflags: "-O0 -g -fsanitize=address -fsanitize=undefined"
release  cflags: "-O2"
coverage cflags: "-O0 -g -fprofile-instr-generate -fcoverage-mapping"
