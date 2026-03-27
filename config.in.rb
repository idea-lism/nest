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

exe "test_re_ast",
  srcs: %w[test/test_re_ast.c src/re_ast.c src/darray.c],
  deps: %w[ustr]

exe "test_parse_gen",
  srcs: %w[test/test_parse_gen.c test/compat.c]

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

# parse_gen -> nest_lex.ll -> nest_lex.o -> link into test_parse
llvm_cc = RUBY_PLATFORM =~ /darwin/ ? "xcrun clang" : (ENV["CC"] || "clang")
cc = ENV["CC"] || "/usr/bin/clang"
cflags = "-std=c23 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fvisibility=hidden #{$mode_cflags.fetch(MODE, "")}"
bd = $builddir_ref
ninja_raw <<~NINJA
rule gen_nest_lex
  command = $in $out
  description = GEN $out

rule ll_cc
  command = #{llvm_cc} -Wno-override-module -c $in -o $out
  description = LLCC $in

build #{bd}/nest_lex.ll: gen_nest_lex #{bd}/parse_gen
build #{bd}/nest_lex.o: ll_cc #{bd}/nest_lex.ll
NINJA

ninja_raw "build #{bd}/test/test_coloring.o: cc_test test/test_coloring.c\n"
ninja_raw "build #{bd}/src/coloring.o: cc src/coloring.c\n"
ninja_raw "build #{bd}/src/graph.o: cc src/graph.c\n"

ninja_raw <<~NINJA
build #{bd}/test_coloring: link #{bd}/test/test_coloring.o #{bd}/src/coloring.o #{bd}/src/graph.o build/kissat/build/libkissat.a
NINJA

$extra_defaults << "#{bd}/test_coloring"

ninja_raw "build #{bd}/test/test_peg.o: cc_test test/test_peg.c\n"
ninja_raw <<~NINJA
build #{bd}/test_peg: link #{bd}/test/test_peg.o #{bd}/test/compat.o #{bd}/src/peg.o #{bd}/src/peg_ir.o #{bd}/src/header_writer.o #{bd}/src/irwriter.o #{bd}/src/bitset.o #{bd}/src/darray.o #{bd}/src/coloring.o #{bd}/src/graph.o build/kissat/build/libkissat.a
NINJA

$extra_defaults << "#{bd}/test_peg"

ninja_raw "build #{bd}/test/test_vpa.o: cc_test test/test_vpa.c\n"
ninja_raw <<~NINJA
build #{bd}/test_vpa: link #{bd}/test/test_vpa.o #{bd}/test/compat.o #{bd}/src/vpa.o #{bd}/src/re_ast.o #{bd}/src/header_writer.o #{bd}/src/re.o #{bd}/src/aut.o #{bd}/src/irwriter.o #{bd}/src/bitset.o #{bd}/src/darray.o #{bd}/libustr.a
NINJA

$extra_defaults << "#{bd}/test_vpa"

test_parse_new_srcs = %w[test/test_parse.c src/parse.c src/token_chunk.c src/vpa.c src/peg.c src/peg_ir.c src/header_writer.c]
test_parse_new_srcs.each do |src|
  obj = "#{bd}/#{src.sub(/\.c$/, '.o')}"
  rule = src.start_with?("test/") ? "cc_test" : "cc"
  ninja_raw "build #{obj}: #{rule} #{src}\n"
end

test_parse_all_srcs = %w[test/test_parse.c test/compat.c src/parse.c src/re_ast.c src/token_chunk.c src/vpa.c src/peg.c src/peg_ir.c src/header_writer.c src/re.c src/aut.c src/irwriter.c src/bitset.c src/darray.c src/coloring.c src/graph.c]
test_parse_objs = test_parse_all_srcs.map { |s| "#{bd}/#{s.sub(/\.c$/, '.o')}" }
test_parse_all = (test_parse_objs + ["#{bd}/nest_lex.o", "#{bd}/libustr.a", "build/kissat/build/libkissat.a"]).join(" ")

ninja_raw <<~NINJA
build #{bd}/test_parse: link #{test_parse_all}
NINJA

$extra_defaults << "#{bd}/test_parse"
