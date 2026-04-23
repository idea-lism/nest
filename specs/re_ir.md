The `ReIr` struct is a flatten VM which can be interpreted with `re.h` methods (and be combined when generating vpa):

```c
typedef enum {
  RE_IR_RANGE_BEGIN, // current_range = re_range_new()
  RE_IR_RANGE_END, // re_append_range(current_range), current_range = NULL
  RE_IR_RANGE_NEG, // re_range_neg(current_range)
  RE_IR_RANGE_IC, // (after re_range_neg) re_range_ic(current_range)
  RE_IR_APPEND_CH, // current_range ? re_range_append(ch) : re_append_ch(ch)
  RE_IR_APPEND_CH_IC, // current_range ? re_range_append(ch) : re_append_ch_ic(ch)
  RE_IR_APPEND_GROUP_S,
  RE_IR_APPEND_GROUP_W,
  RE_IR_APPEND_GROUP_D,
  RE_IR_APPEND_GROUP_H,
  RE_IR_APPEND_GROUP_DOT,
  RE_IR_APPEND_C_ESCAPE,
  RE_IR_APPEND_HEX,
  RE_IR_LPAREN, // re_lparen()
  RE_IR_RPAREN, // re_rparen()
  RE_IR_FORK, // re_fork() on new branches
  RE_IR_LOOP_BACK, // aut_epsilon(cur_state, group_start) for + and * loops
  RE_IR_ACTION, // re_action()
  RE_IR_FRAG_REF, // the fragment to be inlined, start=frag_id
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start; // start cp or frag id
  int32_t end;

  // debug info
  int32_t line; // 1-based
  int32_t col;  // 1-based, by cp
} ReIrOp;

typedef ReIrOp* ReIr; // darray
```

The frag_ref is here because regexp are parsed before `%define` resolves.

We have `re_ir_validate()` to validate the regexp when parsing regexp.

```c
typedef enum {
  RE_IR_OK,
  RE_IR_ERR_RECURSION, // frag_ref recurses
  RE_IR_ERR_MISSING_FRAG_ID, // frags size too small or frags[frag_id] is empty
} ReIrErrKind;

typedef struct {
  ReIrErrKind err_type;
  int frag_id; // the missing or recursed fragment id, the caller can lookup related string
  int line;
  int col;
} ReIrValidateResult;

ReIrValidateResult re_ir_validate(ReIr ir, ReFrags frags);
```

Implement detail for infinite recursion detect: have a ref-stack in VM, and check stack.

We have `re_ir_exec()` to materialize the representation into a `Re` type (in lexer gen, multiple irs can materialize into a single `Re`):

```c
typedef ReIr* ReFrags;

// interpret IR into re.h calls, when met with frag_ref,
// lookup the ReIr in fragment and recursively execute it (frags may ref frags too)
// no validate, we already validated
void re_ir_exec(Re* re, ReIr ir, const char* source_file_name, ReFrags frags);
```

### Tests

Test should be comprehensive, covering:

- all ops
- negated char class (for example, `/[^abc]/` should only match `"xyz"`, not `"xyzabc"`)
- all error types
- error line / col
