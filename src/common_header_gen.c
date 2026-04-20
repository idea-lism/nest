// Common header generation — runtime types + PegRef/PegLink shared by peg_gen and vpa_gen.
#include "common_header_gen.h"

#include "../build/nest_rt.inc"

void common_header_gen(HeaderWriter* hw) {
  hdwriter_puts(hw, "#pragma once\n\n");
  hdwriter_puts(hw, "#include <stdbool.h>\n\n");

  // Amalgamated runtime header (ustr, darray, token_tree, etc.)
  hdwriter_puts(hw, (const char*)NEST_RT);
  hdwriter_putc(hw, '\n');

  // PegRef — used by both peg_gen and vpa_gen
  hdwriter_puts(hw, "typedef struct { TokenChunk* tc; int64_t col; int64_t row; } PegRef;\n");

  // PegLink — used by peg_gen loaders
  hdwriter_puts(hw, "typedef struct { TokenChunk* tc; int64_t col; int64_t col_size_in_i32;"
                    " int64_t lhs_bit_index; int64_t lhs_bit_mask; int64_t lhs_row;"
                    " int64_t lhs_term_id;"
                    " int64_t rhs_bit_index; int64_t rhs_bit_mask; int64_t rhs_row; } PegLink;\n");
  hdwriter_putc(hw, '\n');
}
