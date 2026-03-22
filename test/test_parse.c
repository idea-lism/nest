#include "../src/header_writer.h"
#include "../src/irwriter.h"
#include "../src/parse.h"
#include "compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// Forward declare parse_nest
void parse_nest(const char* src, HeaderWriter* header_writer, IrWriter* ir_writer);

#define TARGET "arm64-apple-macosx14.0.0"

static const char* BOOTSTRAP_SRC = "[[vpa]]\n"
                                   "\n"
                                   "%ignore @comment @space\n"
                                   "\n"
                                   "%keyword directives \"%state\" \"%ignore\" \"%effect\" \"%keyword\"\n"
                                   "\n"
                                   "%keyword ops \"=\" \"|\"\n"
                                   "\n"
                                   "main = {\n"
                                   "  vpa\n"
                                   "  peg\n"
                                   "  ignores\n"
                                   "}\n"
                                   "\n"
                                   "ignores = {\n"
                                   "  /#[^\\n]*/ @comment\n"
                                   "  /[ \\t\\n]+/ @space\n"
                                   "  /\\n+/ @nl\n"
                                   "}\n"
                                   "\n"
                                   "vpa = /\\s*\\[\\[vpa\\]\\]\\n/ .begin {\n"
                                   "  /\\[\\[peg\\]\\]/ .unparse .end\n"
                                   "  id @vpa_id\n"
                                   "  /=/ @assign\n"
                                   "  ignores\n"
                                   "}\n"
                                   "\n"
                                   "id = /[a-z_]\\w*/\n"
                                   "\n"
                                   "[[peg]]\n"
                                   "\n"
                                   "main = vpa peg\n"
                                   "\n"
                                   "vpa = @nl* rule+<@nl> @nl*\n"
                                   "\n"
                                   "rule = [\n"
                                   "  vpa_rule\n"
                                   "]\n"
                                   "\n"
                                   "vpa_rule = id \"=\" @vpa_id\n"
                                   "\n";

TEST(test_parse_basic) {
  char* hdr_buf = NULL;
  size_t hdr_sz = 0;
  FILE* hdr_f = compat_open_memstream(&hdr_buf, &hdr_sz);
  assert(hdr_f);

  char* ir_buf = NULL;
  size_t ir_sz = 0;
  FILE* ir_f = compat_open_memstream(&ir_buf, &ir_sz);
  assert(ir_f);

  HeaderWriter* hw = hw_new(hdr_f);
  IrWriter* w = irwriter_new(ir_f, TARGET);
  irwriter_start(w, "test.nest", ".");

  parse_nest(BOOTSTRAP_SRC, hw, w);

  irwriter_end(w);
  irwriter_del(w);
  hw_del(hw);

  compat_close_memstream(hdr_f, &hdr_buf, &hdr_sz);
  compat_close_memstream(ir_f, &ir_buf, &ir_sz);

  // Verify header has some token defines
  assert(hdr_buf != NULL);
  assert(ir_buf != NULL);

  free(hdr_buf);
  free(ir_buf);
}

int main(void) {
  printf("test_parse:\n");

  RUN(test_parse_basic);

  printf("all ok\n");
  return 0;
}
