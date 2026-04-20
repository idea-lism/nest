// Common header generation — runtime types + PegRef/PegLink shared by peg_gen and vpa_gen.
#pragma once

#include "header_writer.h"

// Emits: #pragma once, #include <stdbool.h>, amalgamated runtime header, PegRef, PegLink.
void common_header_gen(HeaderWriter* hw);
