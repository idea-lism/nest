// for amalgamated header / runtime

typedef struct __attribute__((packed, aligned(8))) {
  TokenChunk* tc;
  int64_t col;
  int64_t row;
} PegRef;

typedef struct __attribute__((packed, aligned(8))) {
  TokenChunk* tc;
  int64_t col;
  int64_t col_size_in_i32;
  int64_t lhs_bit_index;
  int64_t lhs_bit_mask;
  int64_t lhs_row;
  int64_t lhs_term_id;
  int64_t rhs_bit_index;
  int64_t rhs_bit_mask;
  int64_t rhs_row;
} PegLink;

typedef enum {
  PARSE_ERROR_INVALID_HOOK,
  PARSE_ERROR_REQUIRE_MORE_INPUT,
  PARSE_ERROR_TOKEN_ERR,
  PARSE_ERROR_INVALID_SYNTAX,
} ParseErrorType;

typedef struct __attribute__((packed, aligned(8))) {
  const char* message;
  ParseErrorType type;
  int32_t cp_offset;
  int32_t cp_size;
} ParseError;
typedef ParseError* ParseErrors;

typedef struct __attribute__((packed, aligned(8))) {
  PegRef main;
  TokenTree* tt;
  ParseErrors errors;
  int64_t parse_end_col;
} ParseResult;
