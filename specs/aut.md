A automaton builder: NFA construction, determinization, minimization.

Automaton matching is UTF-8 based, it expects inputs codepoint by codepoint (int).

The building model is NFA, output is DFA.

Can define actions (terminal states) on certain states -- because optimization may change state numbering or merge states, action is our stable numbering that remains the same when output.

- Basic interface: `aut_new(function_name, source_file_name)`, `aut_del()`
- `aut_transition(dfa, TransitionDef tdef, DebugInfo di)`
  - arg `tdef`: `{int32_t from_state_id, int32_t to_state_id, int32_t cp_start, int32_t cp_end_inclusive}`
    - init state: 0
  - arg `di`: `{int32_t source_file_line, int32_t source_file_col}`
- `aut_epsilon(dfa, from_state, to_state)`
  - define epsilon transition
- `aut_action(dfa, state, action_id)`
  - marks a terminal state for emitting `action_id` when parsing
  - this functions means just "alias ephemeral state to eternal action_id". action numbering and meaning:
    - -1: in generated code: when this happens, return invalid match, useful for predicates
    - 0: no effect, can continue with more feeds
    - positive values: user defined action trigger, also can continue with more feeds
  - MIN-RULE: when defining different action_id on a same state, the minimal remains.
  - PRESERVING-RULE: optimize / determinize should preserve same action_id for same input
- `aut_optimize(dfa)`
  - (Hopcroft's) determinize then partition-refinement minimization
  - action_ids:
    - when merging states, merge action_ids (min-action_id-rule)
    - test should verify states are reduced
    - after `aut_optimize()`, test should verify PRESERVING-RULE
  - utilize bitset

Automata don't handle EOF or other boundaries, because it is zero width (can stack multiple times but still consumes 0 input).
