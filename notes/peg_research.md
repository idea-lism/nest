Peg research ideas

## Performance innovation

- merged memoize table rule slots to reduce table size
    - A: bitset memoize slot
        - 32 rules at a slot at most, may use ILP to limit that
        - also memoize FAIL tests
        - testing: check value match-end, if == -1, no success match yet. check if the rule bit is denied before.
        - alternative: 16 rules at a slot, 2 bits a rule: 00=init, 01=match, 11=fail, 10=in-stack: a second hit will show rule recursion bug.
    - B: memoize success tries only
        - implementation is simpler but memoize hit rate may not be good
    - C: whole result of the slot can be decided by lookahead inspect and switch-case tabled. for: head match but not finished
        - every slot checks a subset of tokens, use a mask then bit test it.
        - all slots can be pre-computed when a token is generated -- so use this structure for token? no, size is non-uniform.
    - D: for each token, only a limited set of rules may apply. instead of merging by first-token exclusiveness, we set different table column layout for different tokens.
    - layout design:   struct { token bits, rule bits, spans }
        - grouped rule bits are near each other, so by checking a range we can have the information of whether the group is cached.
            - last bit for match fail
        - for branch rule, we have to store the branch id.
            - last branch for match fail
- delayed AST action
    - actions no need to be idempotent any more
    - no wasted node allocations
    - actions can have side effects
    - reduces memoize table size
- other common trivial opts (implementation):
    - rule used only once:
        - no need a memoize table slot
        - no need to push / pop stack state
        - can be generated inline
- reduce caching: except for tokens, some rules may no need be memoized, since accessing the rule can cost more than re-running the rule

Delayed AST action enables some improvement on usability.

- (design) re-formalize PEG: split operator instead of `?`, `+` and `*`
    - efficient: can be parsed bottom-up
    - expressive: parser generator not only matches, but also generates structure. the "," operator can also be used to declare binary operator table, and may express left or right associative operators
    - succinct: no need to add operator table for practical parsing, less to learn and easier to implement
    - force fine control on production actions, which can make matching faster and reduce allocations

Unified matching: longest and prioritized can fit in a same framework.

Optimizing can be done by:
if a branch is dominating, use prioritized, if 2 branches happens for almost the same amount, use parallel matching.
This compile optimization is more than simply likely / unlikely paths.

(any paper mentioning this?)

---

## convert to VPA and only parse with closure

- convert to VPA and only parse with closure
    - composable
        - not really, if we have to resolve conflicts, the parser is not composable any more?
    - input-driven
    - can specify skip / space rules
    - can add logic for context sensitive parsing
    - reduced memoize table size a lot
    - can resume at scope boundary for error reporting

Tokens in each VPA scope should have a "distinct" property. But usually we can't ensure that.
Though most languages are designed that tokens are distinct up to some level that a lexer can be used.

pretty printing? --- pp requires ST transform, not something that can be solved by syntax

- 1. terminal rule: no recursion, regexp-like
- 2. VPA rule: starts with non-empty terminals, ( if ends with non-empty terminal, end terminal gets highest priority )
- 3. Branched rule: starts with a branch

We remove terminals and VPAs, leaving only the structural rules.

In the end we build a bottom-up lexer and many top-down parsers.

For 3 rules, check terminal conflicts and specify priorities inside the rule (A & B may overlap, so we specify A > B or B > A)

Foo (B > C): A / B   # in rule body's priority choices, we figure out an order `A > B`. for tokens which don't have orders, the context in matching place will decide which to use.

We can have all the tokens topologically sorted by traversing.

But it is still possible that A > B and B > A both happens. Then we check if A and B are inherently different.
After a naive algorithm, compiler may find 2 terminals overlap, will ask user to refine definition (can be look-aheads or boundaries).
Or user can just specify (B ? C) and let compiler raise error at runtime.

Other benefits for turning PEG into VPA: we may report multiple errors for a single file.
To recover errors.

Top level rule can be inherently a VPA context without end rule.

VPA end detection

2 rules may end with the same "end" token, but both of them can be a VPA rule. A VPA rule is only responsible for the first token when included, so the "end" token can still be unique for many cases.

The worst case: top level, second level rules are all PEGs?
Let's inline expand PEG rules, until all of them are token or VPA. Then we still have a top-level huge VPA and sub-level VPAs.

The top bag may be quite large.

We build a huge regexp for the token matching machine. And terminals may overlap. We assign preference for each term state by `/` relation from the grammar set. Then most overlapping tokens should be well ordered. If not, the token is ambiguous and waiting PEG to clarify (side effect of this mechanism: tokens cannot have values, only rules can convert values).

If 2 tokens cannot be compared, it is fine.

If both A > B and A < B stand true, we can gently warn user that the 2 tokens may not be consistently ordered. Also listing the rules containing them with partial expansion.

IDEA: a follow set of a token is ordered, so as to make the lexer scan more precisely

---

## tabular parsing

Selective memoize for regexp:
https://fservant.github.io/papers/DavisServantLee-SelectiveMemo-IEEE-SP21.pdf

question on the complexity of PEG: (is stateful packrat parsing linear in practice?)
https://dl.acm.org/doi/pdf/10.1145/3377555.3377898

on call stack optimizations, not very useful since the same opt can be done in bytecode
https://ijcsmc.com/docs/papers/November2013/V2I11201354.pdf

pika parsing: re-formalize packrat parsing as a dynamic programming problem and solves left-recursion & error recovery problems
https://arxiv.org/pdf/2005.06444.pdf
(bottom-up and right-to-left), though the performance degrades as grammar size grows. It also uses precedence climbing.

Chart parsing: Earley parser is n^3 for all CFG, n^2 for unambiguous grammar, n for LR(k). Since PEG is superset of LL(k) / LR(k), an earley parser can be used to model part of it. In this direction, a better way may be LL(*).
LL(finite) is a similar approach like ANTLR's:
https://www.oxford-man.ox.ac.uk/wp-content/uploads/2020/11/The-LLfinite-strategy-for-optimal-LL.pdf

others: sliding window

---

table choices:

1. cache value, require all actions be idempotent (or can be undone) -- this wastes memory allocations
2. delayed AST building: Memoize table stores the end positions for each rule, after parse done, we traverse the table again to build node.

table entries
>0: end
0: matches, end unknown
-1: not match
-2: matching

when the rule is X*, store size of elements

1. we can make choices at token level: put a "cross" mask for a whole column because we know the token. this is lookahead-1 token optimization.

Table optimization: set token columns then transpose?

The table

Token stream after lexing: stores size and kind of each token.

Table slots should exclude tokens, since we already have them in stream.
We must memoize end for most rule, to ensure linear time.
But for branched rule, we must memoize the choice.

branch rule: store the choice sequence rule id
- maybe we can compress the id to a table lookup, but it impacts performance
- a uniform way is just use i48 part
sequence slot which tokens can be estimated (range = upperbound - lowerbound): range-bits/-1/-2
- maybe no need for this complexity, just use i48 for uniformity?
other sequence slot: i64-token-end/-1/-2
- cache the end
shared slots of conflict rules
- i16 choice, i48 token-end

(inline slots if necessary, to reduce number of rules)

Each token yields a state set.

- For token state, the table stores  1 or 0
- Identify rule similarities, if 2 rules are the same, we can reduce cache
- For branch owner rule, store end of the rule position if it is variable length.
- For seq, store end of the rule position only if the seq is of variable size (for linear parsing, we have to skip fast)
- We may guess cost of the rule, if the cost is very low, no need to cache.

To iterate ST actions, we re-run the parser table recursively and take actions.

For each location, there is a node building chain.
When PEG backtracks, the building chain is backtracked.

It is a sequence for building: BCA2D2...  it can be released with the table together.

In this form, our memory usage is still much lesser than traditional PEG parser.
For each position we have ( i64 * vl_rules + i1 * tokens + i32 * 1 ) * input-size.

The ST iterate sequence memory can be pre-allocated.

---

## incremental PEG parsing

https://www.google.com/search?q=PEG+incremental+parsing

an incremental PEG parser in 2021 -- but not having the idea of VPA.
https://people.seas.harvard.edu/~chong/pubs/gpeg_sle21.pdf

a verified VPG parser in 2021 -- provides more insights of visibly pushdown parser
https://arxiv.org/pdf/2109.04258.pdf

layout sensitive parsing -- peg
http://scg.unibe.ch/archive/projects/Sade13a.pdf

bachelor thesis, an efficient machine for peg, compiles simple strategies for optimizing
https://www.cs.ru.nl/bachelors-theses/2018/Jos_Craaijo___4481674___An_efficient_parsing_machine_for_PEGs.pdf

configuration and remainder grammar
http://scg.unibe.ch/archive/masters/Ruef16a.pdf
other recover idea: recover by VPA terminating token and line endings
