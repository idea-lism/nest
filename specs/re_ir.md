The `ReIr` struct is a flat VM which can be interpreted with `re.h` methods (and combined when generating VPA scopes).

```c
typedef enum {
  RE_IR_RANGE_BEGIN,      // current_range = re_range_new()
  RE_IR_RANGE_END,        // re_append_range(current_range), current_range = NULL
  RE_IR_RANGE_NEG,        // re_range_neg(current_range)
  RE_IR_RANGE_IC,         // (before RANGE_END) re_range_ic(current_range)
  RE_IR_APPEND_CH,        // current_range ? re_range_add(ch) : re_append_ch(ch)
  RE_IR_APPEND_CH_IC,     // current_range ? re_range_add(ch) : re_append_ch_ic(ch)
  RE_IR_APPEND_GROUP_S,   // \s
  RE_IR_APPEND_GROUP_W,   // \w
  RE_IR_APPEND_GROUP_D,   // \d
  RE_IR_APPEND_GROUP_H,   // \h
  RE_IR_APPEND_GROUP_DOT, // .
  RE_IR_APPEND_C_ESCAPE,  // start = escape symbol char (b/f/n/r/t/v)
  RE_IR_APPEND_HEX,       // start/end = codepoint
  RE_IR_LPAREN,           // re_lparen()
  RE_IR_RPAREN,           // re_rparen()
  RE_IR_FORK,             // re_fork()
  RE_IR_LOOP_BACK,        // aut_epsilon(cur_state, group_start) for + and * loops
  RE_IR_ACTION,           // re_action()
  RE_IR_FRAG_REF,         // start = frag_id; resolved after %define
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start; // start codepoint or frag id
  int32_t end;

  // debug info
  int32_t line; // 1-based
  int32_t col;  // 1-based, by codepoint
} ReIrOp;

typedef ReIrOp* ReIr;  // darray
typedef ReIr* ReFrags; // darray indexed by frag_id

// --- Lifecycle ---

ReIr re_ir_new(void);
void re_ir_free(ReIr ir);
ReIr re_ir_clone(ReIr src);

// Emit one op. line/col may be 0 when unknown.
ReIr re_ir_emit(ReIr ir, ReIrKind kind, int32_t start, int32_t end, int32_t line, int32_t col);
// Shorthand for RE_IR_APPEND_CH with cp as both start and end.
ReIr re_ir_emit_ch(ReIr ir, int32_t cp);
// Build IR for a literal UTF-8 source slice.
ReIr re_ir_build_literal(const char* src, int32_t cp_off, int32_t cp_len);

// --- Validation ---

typedef enum {
  RE_IR_OK,
  RE_IR_ERR_RECURSION,       // frag_ref recurses
  RE_IR_ERR_MISSING_FRAG_ID, // frags size too small or frags[frag_id] is empty
} ReIrErrKind;

typedef struct {
  ReIrErrKind err_type;
  int32_t frag_id; // the missing or recursed fragment id; caller can look up its string
  int32_t line;
  int32_t col;
} ReIrValidateResult;

// Validate fragment references: missing or recursive frag_id.
// Empty and NULL IR are valid; paren balance is not checked here.
ReIrValidateResult re_ir_validate(ReIr ir, ReFrags frags);

// --- Execution ---

// Interpret IR into re.h calls. On FRAG_REF, look up the ReIr in frags and
// recursively execute it (fragments may reference fragments).
// `frags` may be NULL when no fragments are defined.
// No validation here: call re_ir_validate() first (it only checks frag refs).
void re_ir_exec(Re* re, ReIr ir, ReFrags frags);
```

`frag_ref` exists because regexps are parsed before `%define` resolves.

Infinite recursion is detected by keeping a ref-stack in the VM and checking whether the fragment is already on it.

### Tests

Test should be comprehensive, covering:

- all ops
- negated char class (for example, `/[^abc]/` should only match `"xyz"`, not `"xyzabc"`)
- all error types
- error line / col
