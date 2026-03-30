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
  RE_FORK, // re_fork() on new branches
  RE_IR_ACTION, // re_action()
} ReIrKind;

typedef struct {
  ReIrKind kind;
  int32_t start;
  int32_t end;
} ReIrOp;

typedef ReIrOp* ReIr; // darray
```
