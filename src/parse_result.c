#include "token_tree.h"
#include "parse_result.inc"

void parse_result_del(ParseResult* res) {
  if (res->tt) {
    tt_tree_del(res->tt, true);
    res->tt = 0;
  }
}
