A Automaton compiler, generates LLVM IR.

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
- `aut_gen_dfa(dfa, IRWriter writer, debug_mode)`
  - Generates function in LLVM IR
  - C calling convention
  - signature: `(int64_t state, int64_t cp) -> (int64_t new_state, int64_t action_id)`
  - char group range matching:
    - just generate a flat switch case, llvm can optimize this
    - single char check code: `if (cp == ...)`
    - range check code: `if (cp >= ... && cp <= ...)`, don't enumerate chars. llvm can optimize small range checks
  - returns the smallest action_id possible
    - 0 for no action
    - -1 for user defined transition invalid
    - -2 for undefined transition invalid
    - note that we don't need special handling for 0 or -1 because we have the `smallest` rule
  - dead state input:
    - should not happen, when in `debug_mode`, make debugger break at the trap, but still return `{last_state, -2}`
- Moore semantics, Mealy encoding:
  - actions are properties of states (Moore): `aut_action` marks states, MIN-RULE resolves conflicts by state content
  - generated function returns action alongside transition (Mealy): avoids a separate state→action table lookup per character
  - all transitions into the same DFA state carry the same action_id, so no information is lost
- IR also encodes debug information so lldb step-by-step can show the regexp source:
  - utilize dfa's `source_file_name`
  - utilize iterator's `line` and `col`

Automata don't handle EOF or other boundaries, because it is zero width (can stack multiple times but still consumes 0 input).
