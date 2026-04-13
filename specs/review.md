### We don't allow stub/fake/empty tests

In a TDD point-of-view -- if an empty implementation makes the test pass -- then it is considered FAKE.

### We don't appreciate defensive code

Checking same conditions over and over again is a bad smell, it shows there's no design in it, just patching rat nests.

### Clean code

Avoid over-engineering. Only make changes that are directly requested or clearly necessary. Keep solutions
simple and focused.

- Don't add features, refactor code, or make "improvements" beyond what was asked. A bug fix doesn't need surrounding code cleaned up. A simple feature doesn't need extra configurability. Don't add docstrings,
comments, or type annotations to code you didn't change. Only add comments where the logic isn't self-
evident.

- Don't add error handling, fallbacks, or validation for scenarios that can't happen. Trust internal code and
framework guarantees. Only validate at system boundaries (user input, external APIs). Don't use feature flags or backwards-compatibility shims when you can just change the code.

- Don't create helpers, utilities, or abstractions for one-time operations. Don't design for hypothetical
future requirements. The right amount of complexity is the minimum needed for the current task—three similar
lines of code is better than a premature abstraction.

- Avoid backwards-compatibility hacks like renaming unused _vars, re-exporting types, adding // removed
comments for removed code, etc. If you are certain that something is unused, you can delete it completely.

### Idiomatic C: utilize fat pointers

We already have:

- darray.h -- fat pointer dynamic array
- ustr.h -- fat pointer utf-8 string

To distiguish between fat pointer and normal pointers, use typedef in plural form. For example:

```c
typedef VpaRule* VpaRules; // then we know it is a fat pointer darray when seeing VpaRules
```

### Idiomatic C: constant string concat

Prefer justapositioning over snprintf:

```c
const char* s = "foo" SOME_STRING_MACRO;
```

### Idiomatic C: format strings smartly

Bad code:
```c
char* tok_name = parse_sfmt("lit.%s", lit);
int32_t tok_id = symtab_intern(&ps->tokens, tok_name);
free(tok_name);
```

Good code:
```c
int32_t tok_id = symtab_intern_f(&ps->tokens, "lit.%.*s", byte_size, lit);
```

### Idiomatic C: avoid tri-stars

Tri-stars is a bad smell of complex code

Bad code:
```c
char** arr = darray_new(sizeof(char*), 0);
char*** array_of_strings = &arr;
...
```

Good code:
```c
Symtab strings = ...
```

### Idiomatic C: flatten branches

Bad code (introduces unecessary nesting):
```c
if (mode != NONE) {
  if (mode == NAIVE) {
    ...
  } else {
    ...
  }
} else {
  ...
}
```

Good code (flattened, clean):
```c
if (mode == NONE) {
  ...
} else if (mode == NAIVE) {
  ...
} else {
  ...
}
```
