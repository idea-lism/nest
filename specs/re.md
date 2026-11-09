Regexp building helpers

Some automatic state management on top of [aut](aut.md).

### Interface

```c
typedef struct Re Re;

// Sorted, merged list of codepoint intervals. Codepoints are [0, 0x10FFFF].
typedef struct {
  int32_t start;
  int32_t end;
} ReInterval;

typedef struct {
  ReInterval* ivs; // darray
  bool negated;
} ReRange;

ReRange* re_range_new(void);
void re_range_del(ReRange* range);
void re_range_add(ReRange* range, int32_t start_cp, int32_t end_cp); // kept sorted + merged
void re_range_neg(ReRange* range);                                  // in-place
void re_range_ic(ReRange* range);                                   // ignore case; negates first if needed

// --- Builder ---

Re* re_new(Aut* aut);
void re_del(Re* re);

// Group stack. Top is state 0; re_lparen() pushes, re_rparen() pops.
void re_lparen(Re* re);
void re_fork(Re* re);      // new branch forked at the stack top
void re_rparen(Re* re);    // pop the group (connects branches to the exit)
void re_loop_back(Re* re); // epsilon from current state back to group start (+ / * loops)

// Append a literal codepoint / range. Negative codepoints are allowed for
// internal sentinels. `di` is passed to aut_transition for diagnostics.
void re_append_ch(Re* re, int32_t codepoint, DebugInfo di);
void re_append_ch_ic(Re* re, int32_t codepoint, DebugInfo di);
void re_append_range(Re* re, ReRange* range, DebugInfo di);

void re_action(Re* re, int32_t action_id); // emit this action at the current state
int32_t re_cur_state(Re* re);

// --- Predefined ranges ---

void re_append_group_s(ReRange* range);   // \s
void re_append_group_d(ReRange* range);   // \d
void re_append_group_w(ReRange* range);   // \w
void re_append_group_h(ReRange* range);   // \h
void re_append_group_dot(ReRange* range); // .

int32_t re_c_escape(char symbol);                  // \\[bfnrtv] -> codepoint, -1 if unknown
int32_t re_hex_to_codepoint(const char* h, size_t size);

// --- Simple pattern compiler (one regexp per line) ---

typedef struct ReLex ReLex;

// `mode` flags: i = case-insensitive, b = binary (bytes, not UTF-8 codepoints).
ReLex* re_lex_new(const char* mode);
void re_lex_del(ReLex* l);
int32_t re_lex_add(ReLex* l, const char* pattern, int32_t line, int32_t col, int32_t action_id);
```

Regexps don't handle EOF or other boundaries, because they are zero width (can stack multiple times but still consume 0 input). Other DFA-based libraries don't handle this either.
