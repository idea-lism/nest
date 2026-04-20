// specs/rt_ir.md — LLVM IR lexer generator for runtime IR processing.
// Similar to parse_gen.c: build-time tool that creates a lexer to analyze LLVM IR.
// Generates build/llir_lex.ll with DFA lexer functions used by irwriter_gen_rt.c.

#include "irwriter.h"
#include "re.h"

#include <stdio.h>
#include <stdlib.h>

// Single scope: top-level LLVM IR lines.
// Tokens are line-start keywords/patterns that the state machine cares about.
enum {
  LLIR_ACTION_IGNORE = 0,
  LLIR_TOK_SOURCE_FILENAME = 1,
  LLIR_TOK_DEFINE = 2,
  LLIR_TOK_DEFINE_INTERNAL = 3,
  LLIR_TOK_DECLARE = 4,
  LLIR_TOK_META_MODULE_FLAGS = 5,
  LLIR_TOK_META_LLVM_IDENT = 6,
  LLIR_TOK_META_DIFILE = 7,
  LLIR_TOK_META_NUMBERED = 8,
  LLIR_TOK_BRACE_CLOSE = 9,
  LLIR_TOK_OTHER = 10,
};

static ReLex* _build_llir_scope(void) {
  ReLex* l = re_lex_new("lex_llir", "llir", "");

  // Line-start patterns for LLVM IR.
  // The lexer is called per-line, matching the first token of each line.

  // source_filename
  re_lex_add(l, "source_filename", __LINE__, 15, LLIR_TOK_SOURCE_FILENAME);

  // define internal — must come before define
  re_lex_add(l, "define internal ", __LINE__, 15, LLIR_TOK_DEFINE_INTERNAL);

  // define (not internal)
  re_lex_add(l, "define ", __LINE__, 15, LLIR_TOK_DEFINE);

  // declare
  re_lex_add(l, "declare ", __LINE__, 15, LLIR_TOK_DECLARE);

  // !llvm.module.flags
  re_lex_add(l, "!llvm\\.module\\.flags", __LINE__, 15, LLIR_TOK_META_MODULE_FLAGS);

  // !llvm.ident
  re_lex_add(l, "!llvm\\.ident", __LINE__, 15, LLIR_TOK_META_LLVM_IDENT);

  // !DIFile — metadata containing DIFile
  // This won't be at line start; DIFile appears in !NN = ... lines.
  // We handle DIFile detection in the state machine, not lexer.

  // !<number> = ... (metadata numbered entry)
  re_lex_add(l, "![0-9]+", __LINE__, 15, LLIR_TOK_META_NUMBERED);

  // } alone on a line (function end)
  re_lex_add(l, "}", __LINE__, 15, LLIR_TOK_BRACE_CLOSE);

  return l;
}

int main(int argc, char** argv) {
  const char* output = "build/llir_lex.ll";
  if (argc > 1) {
    output = argv[1];
  }

  FILE* f = fopen(output, "w");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", output);
    return 1;
  }

  IrWriter* w = irwriter_new(f);
  // Write minimal source_filename — target triple comes from clang at compile time
  fprintf(f, "source_filename = \"llir_lex.ll\"\n\n");
  irwriter_start(w, 0, "llir", ".");

  ReLex* l = _build_llir_scope();
  re_lex_gen(l, w, false);
  re_lex_del(l);

  irwriter_end(w);
  irwriter_del(w);
  fclose(f);

  printf("generated %s\n", output);
  return 0;
}
