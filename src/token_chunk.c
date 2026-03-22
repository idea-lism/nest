#include "token_chunk.h"
#include "darray.h"

#include <string.h>

void tc_init(TokenChunk* c) { *c = NULL; }

void tc_add(TokenChunk* c, Token t) {
  if (!*c) {
    *c = darray_new(sizeof(Token), 0);
  }
  darray_push(*c, t);
}

int32_t tc_size(TokenChunk c) { return (int32_t)darray_size(c); }

void tc_free(TokenChunk* c) {
  darray_del(*c);
  *c = NULL;
}
