#pragma once

#include "parse.h"

// compile passes, call them by order

bool pp_inline_macros(ParseState* ps);          // in vpa
bool pp_auto_tag_branches(ParseState* ps);      // in peg
bool pp_check_duplicate_tags(ParseState* ps);   // in peg
bool pp_detect_left_recursions(ParseState* ps); // in peg
bool pp_validate_vpa_scopes(ParseState* ps);    // in vpa
bool pp_validate_peg_rules(ParseState* ps);     // in peg
bool pp_match_scopes(ParseState* ps);           // in vpa & peg

#define pp_all_passes(ps)                                                                                              \
  (pp_inline_macros(ps) && pp_auto_tag_branches(ps) && pp_check_duplicate_tags(ps) && pp_detect_left_recursions(ps) && \
   pp_validate_vpa_scopes(ps) && pp_validate_peg_rules(ps) && pp_match_scopes(ps))
