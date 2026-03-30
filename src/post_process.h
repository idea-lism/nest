#pragma once

#include "parse.h"

// compile passes, call them by order

bool pp_inline_macros(ParseState* ps);        // in vpa
bool pp_auto_tag_branches(ParseState* ps);    // in peg
bool pp_check_duplicate_tags(ParseState* ps); // in peg
bool pp_expand_keywords(ParseState* ps);      // in peg
bool pp_validate(ParseState* ps);

static bool pp_all_passes(ParseState* ps) {
  return pp_inline_macros(ps) && pp_auto_tag_branches(ps) && pp_check_duplicate_tags(ps) && pp_expand_keywords(ps) &&
         pp_validate(ps);
}
