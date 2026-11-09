An automaton builder: NFA construction, determinization, minimization.

Automaton matching is UTF-8 based; it consumes input codepoint by codepoint (`int32_t`).

The building model is NFA; the output is DFA.

### Interface

```c
typedef struct Aut Aut;

typedef struct {
  int32_t from_state_id;
  int32_t to_state_id;
  int32_t cp_start;
  int32_t cp_end_inclusive; // inclusive range; cp_start == cp_end is a single char
} TransitionDef;

typedef struct {
  int32_t source_file_line;
  int32_t source_file_col;
} DebugInfo; // line 0 means "no location"

Aut* aut_new(void);
void aut_del(Aut* a);

// Initial state is 0. States are created implicitly by transitions.
void aut_transition(Aut* a, TransitionDef tdef, DebugInfo di);
void aut_epsilon(Aut* a, int32_t from_state, int32_t to_state);

// Mark a terminal state with an action ID.
//   -1: invalid match (predicate failed)
//    0: no effect, keep feeding input
//   >0: user action; keep feeding input
// MIN-RULE: when several actions land on the same state, the smallest ID wins.
// PRESERVING-RULE: determinize/minimize must preserve the action for the same input.
void aut_action(Aut* a, int32_t state, int32_t action_id);

// Subset construction, then partition-refinement minimization.
// Merged states keep the minimum action ID. Coalesces adjacent ranges.
void aut_optimize(Aut* a);

// Determinizes on demand if aut_optimize() was not called.
int32_t aut_dfa_nstates(Aut* a);
```

Automata don't handle EOF or other boundaries, because it is zero width (can stack multiple times but still consumes 0 input).
