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

#define _PP_VERBOSE_STEP(ps, step_fn, label) (fprintf(stderr, "  [pp] %s\n", label), step_fn(ps))

#define pp_all_passes_verbose(ps)                                                                                      \
  (_PP_VERBOSE_STEP(ps, pp_inline_macros, "inline_macros") &&                                                          \
   _PP_VERBOSE_STEP(ps, pp_auto_tag_branches, "auto_tag_branches") &&                                                  \
   _PP_VERBOSE_STEP(ps, pp_check_duplicate_tags, "check_duplicate_tags") &&                                            \
   _PP_VERBOSE_STEP(ps, pp_detect_left_recursions, "detect_left_recursions") &&                                        \
   _PP_VERBOSE_STEP(ps, pp_validate_vpa_scopes, "validate_vpa_scopes") &&                                              \
   _PP_VERBOSE_STEP(ps, pp_validate_peg_rules, "validate_peg_rules") &&                                                \
   _PP_VERBOSE_STEP(ps, pp_match_scopes, "match_scopes"))
