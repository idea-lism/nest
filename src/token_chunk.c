#include "token_chunk.h"
#include "darray.h"

#include <string.h>

void tc_init(TokenChunk* c, int32_t scope_id, int32_t parent_id) {
  c->scope_id = scope_id;
  c->parent_id = parent_id;
  c->tokens = darray_new(sizeof(Token), 0);
}

void tc_add(TokenChunk* c, Token t) {
  darray_push(c->tokens, t);
}

int32_t tc_size(TokenChunk* c) {
  return (int32_t)darray_size(c->tokens);
}

void tc_free(TokenChunk* c) {
  darray_del(c->tokens);
  c->tokens = NULL;
}
