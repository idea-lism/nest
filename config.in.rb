lib "ustr",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c]

exe "test_ustr",
  srcs: %w[test/test_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_irwriter",
  srcs: %w[test/test_irwriter.c test/compat.c src/irwriter.c src/darray.c]

exe "test_bitset",
  srcs: %w[test/test_bitset.c src/bitset.c]

exe "bench_ustr",
  srcs: %w[test/bench_ustr.c src/ustr_naive.c],
  deps: %w[ustr]

exe "test_aut",
  srcs: %w[test/test_aut.c test/compat.c src/aut.c src/irwriter.c src/bitset.c src/darray.c]

exe "test_re",
  srcs: %w[test/test_re.c test/compat.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c]

exe "test_parse_gen",
  srcs: %w[test/test_parse_gen.c test/compat.c]

exe "test_token_chunk",
  srcs: %w[test/test_token_chunk.c src/token_chunk.c src/darray.c],
  deps: %w[ustr]

exe "parse_gen",
  srcs: %w[src/parse_gen.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c],
  deps: %w[ustr]

combined_lib "re",
  srcs: %w[src/ustr.c src/ustr_neon.c src/ustr_avx.c src/irwriter.c src/bitset.c src/aut.c src/re.c src/darray.c]

amalgamate input: "src/re_rt.h.in",
  output: "out/re_rt.h",
  include_dirs: %w[src]

debug    cflags: "-O0 -g -fsanitize=address -fsanitize=undefined"
release  cflags: "-O2"
coverage cflags: "-O0 -g -fprofile-instr-generate -fcoverage-mapping"

# --- nest_lex code generation pipeline (parse_gen -> .ll -> .o) ---
bd = $builddir_ref
llvm_cc = RUBY_PLATFORM =~ /darwin/ ? "xcrun clang" : (ENV["CC"] || "clang")

ninja_raw <<~NINJA
rule gen_nest_lex
  command = $in $out
  description = GEN $out

rule ll_cc
  command = #{llvm_cc} -c $in -o $out
  description = LLCC $in

build #{bd}/nest_lex.ll: gen_nest_lex #{bd}/parse_gen
build #{bd}/nest_lex.o: ll_cc #{bd}/nest_lex.ll
NINJA

# --- targets using kissat + generated nest_lex ---
kissat = IS_WINDOWS ? [] : %w[build/kissat/build/libkissat.a]
nest_lex = ["#{bd}/nest_lex.o"]

exe "test_coloring",
  srcs: %w[test/test_coloring.c src/coloring.c src/graph.c],
  ext_libs: kissat

exe "test_peg",
  srcs: %w[test/test_peg.c test/compat.c src/peg.c src/peg_ir.c src/header_writer.c src/irwriter.c src/bitset.c src/darray.c src/coloring.c src/graph.c],
  ext_libs: kissat

exe "test_vpa",
  srcs: %w[test/test_vpa.c test/compat.c src/vpa.c src/header_writer.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c],
  deps: %w[ustr]

exe "test_parse",
  srcs: %w[test/test_parse.c test/compat.c src/parse.c src/token_chunk.c src/vpa.c src/peg.c src/peg_ir.c src/header_writer.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c src/coloring.c src/graph.c],
  deps: %w[ustr],
  extra_objs: nest_lex,
  ext_libs: kissat

exe "nest",
  srcs: %w[src/nest.c src/parse.c src/token_chunk.c src/vpa.c src/peg.c src/peg_ir.c src/header_writer.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c src/coloring.c src/graph.c],
  deps: %w[ustr],
  extra_objs: nest_lex,
  ext_libs: kissat
