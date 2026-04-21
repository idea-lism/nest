// Common header generation — runtime types + PegRef/PegLink shared by peg_gen and vpa_gen.
#include "common_header_gen.h"

#include "../build/nest_rt.inc"

void common_header_gen(HeaderWriter* hw) {
  hdwriter_puts(hw, "#pragma once\n\n");
  hdwriter_puts(hw, "#include <stdbool.h>\n\n");

  // Amalgamated runtime header (ustr, darray, token_tree, etc.)
  hdwriter_puts(hw, (const char*)NEST_RT);
  hdwriter_putc(hw, '\n');
}
