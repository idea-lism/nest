#include "../src/lex.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

#define TARGET "arm64-apple-macosx14.0.0"

static char* _gen(void (*fn)(Lex*)) {
  char* buf = NULL;
  size_t sz = 0;
  FILE* f = open_memstream(&buf, &sz);
  assert(f);
  Lex* l = lex_new("test.rules");
  fn(l);
  lex_gen(l, f, TARGET);
  lex_del(l);
  fclose(f);
  return buf;
}

// --- Lifecycle ---

TEST(test_lifecycle) {
  Lex* l = lex_new("test.rules");
  assert(l);
  lex_del(l);
}

// --- Action IDs ---

TEST(test_action_ids) {
  Lex* l = lex_new("test.rules");
  int32_t a1 = lex_add(l, "", "abc", 0);
  int32_t a2 = lex_add(l, "", "xyz", 0);
  int32_t a3 = lex_add(l, "", "123", 0);
  assert(a1 == 1);
  assert(a2 == 2);
  assert(a3 == 3);
  lex_del(l);
}

// --- Simple literal ---

static void _build_literal(Lex* l) { lex_add(l, "", "Hi", 0); }

TEST(test_literal) {
  char* out = _gen(_build_literal);
  assert(strstr(out, "i32 72"));  // H
  assert(strstr(out, "i32 105")); // i
  free(out);
}

// --- Char class ---

static void _build_charclass(Lex* l) { lex_add(l, "", "[a-z]", 0); }

TEST(test_charclass) {
  char* out = _gen(_build_charclass);
  assert(strstr(out, "icmp sge i32 %cp, 97"));
  assert(strstr(out, "icmp sle i32 %cp, 122"));
  free(out);
}

// --- Negative char class ---

static void _build_neg_charclass(Lex* l) { lex_add(l, "", "[^a]", 0); }

TEST(test_neg_charclass) {
  char* out = _gen(_build_neg_charclass);
  // [^a] = [0..96, 98..0x10FFFF]
  assert(strstr(out, "icmp sle i32 %cp, 96"));
  assert(strstr(out, "icmp sge i32 %cp, 98"));
  free(out);
}

// --- C-escapes ---

static void _build_c_escape(Lex* l) { lex_add(l, "", "\\n\\t", 0); }

TEST(test_c_escape) {
  char* out = _gen(_build_c_escape);
  assert(strstr(out, "i32 10")); // \n
  assert(strstr(out, "i32 9"));  // \t
  free(out);
}

// --- Unicode escape ---

static void _build_unicode_escape(Lex* l) { lex_add(l, "", "\\u{41}", 0); }

TEST(test_unicode_escape) {
  char* out = _gen(_build_unicode_escape);
  assert(strstr(out, "i32 65")); // 'A' = 0x41
  free(out);
}

// --- Char escape in class ---

static void _build_escape_in_class(Lex* l) { lex_add(l, "", "[\\-]", 0); }

TEST(test_escape_in_class) {
  char* out = _gen(_build_escape_in_class);
  assert(strstr(out, "i32 45")); // '-'
  free(out);
}

// --- Special class \d ---

static void _build_digit(Lex* l) { lex_add(l, "", "\\d", 0); }

TEST(test_digit_class) {
  char* out = _gen(_build_digit);
  assert(strstr(out, "i32 %cp, 48"));
  assert(strstr(out, "i32 %cp, 57"));
  free(out);
}

// --- Special class \w ---

static void _build_word(Lex* l) { lex_add(l, "", "\\w", 0); }

TEST(test_word_class) {
  char* out = _gen(_build_word);
  assert(strstr(out, "i32 %cp, 48"));  // '0'
  assert(strstr(out, "i32 %cp, 57"));  // '9'
  assert(strstr(out, "i32 %cp, 65"));  // 'A'
  assert(strstr(out, "i32 %cp, 90"));  // 'Z'
  assert(strstr(out, "i32 95"));       // '_' (switch case)
  assert(strstr(out, "i32 %cp, 97"));  // 'a'
  assert(strstr(out, "i32 %cp, 122")); // 'z'
  free(out);
}

// --- Special class \s ---

static void _build_space(Lex* l) { lex_add(l, "", "\\s", 0); }

TEST(test_space_class) {
  char* out = _gen(_build_space);
  assert(strstr(out, "i32 %cp, 9"));  // \t (range start)
  assert(strstr(out, "i32 %cp, 13")); // \r (range end)
  assert(strstr(out, "i32 32"));      // space (switch case)
  free(out);
}

// --- Special class \h ---

static void _build_hex(Lex* l) { lex_add(l, "", "\\h", 0); }

TEST(test_hex_class) {
  char* out = _gen(_build_hex);
  assert(strstr(out, "i32 %cp, 48"));  // '0'
  assert(strstr(out, "i32 %cp, 57"));  // '9'
  assert(strstr(out, "i32 %cp, 65"));  // 'A'
  assert(strstr(out, "i32 %cp, 70"));  // 'F'
  assert(strstr(out, "i32 %cp, 97"));  // 'a'
  assert(strstr(out, "i32 %cp, 102")); // 'f'
  free(out);
}

// --- Dot ---

static void _build_dot(Lex* l) { lex_add(l, "", ".", 0); }

TEST(test_dot) {
  char* out = _gen(_build_dot);
  // dot matches everything except \n (10)
  assert(strstr(out, "i32 %cp, 9"));  // up to 9
  assert(strstr(out, "i32 %cp, 11")); // from 11
  free(out);
}

// --- Boundaries ---

static void _build_bof(Lex* l) { lex_add(l, "", "\\afoo", 0); }

TEST(test_bof) {
  char* out = _gen(_build_bof);
  assert(strstr(out, "i32 -1")); // LEX_CP_BOF
  free(out);
}

static void _build_eof(Lex* l) { lex_add(l, "", "foo\\z", 0); }

TEST(test_eof) {
  char* out = _gen(_build_eof);
  assert(strstr(out, "i32 -2")); // LEX_CP_EOF
  free(out);
}

// --- Alternation ---

static void _build_alt(Lex* l) { lex_add(l, "", "a|b", 0); }

TEST(test_alt) {
  char* out = _gen(_build_alt);
  assert(strstr(out, "i32 97")); // a
  assert(strstr(out, "i32 98")); // b
  free(out);
}

// --- Grouping ---

static void _build_group(Lex* l) { lex_add(l, "", "(ab|cd)e", 0); }

TEST(test_group) {
  char* out = _gen(_build_group);
  assert(strstr(out, "i32 97"));  // a
  assert(strstr(out, "i32 98"));  // b
  assert(strstr(out, "i32 99"));  // c
  assert(strstr(out, "i32 100")); // d
  assert(strstr(out, "i32 101")); // e
  free(out);
}

// --- Quantifier ? ---

static void _build_optional(Lex* l) { lex_add(l, "", "ab?c", 0); }

TEST(test_optional) {
  char* out = _gen(_build_optional);
  // Should compile: matches "ac" or "abc"
  assert(strstr(out, "i32 97")); // a
  assert(strstr(out, "i32 99")); // c
  assert(strstr(out, "i32 98")); // b
  free(out);
}

// --- Quantifier + ---

static void _build_plus(Lex* l) { lex_add(l, "", "a+", 0); }

TEST(test_plus) {
  char* out = _gen(_build_plus);
  assert(strstr(out, "i32 97")); // a
  free(out);
}

// --- Quantifier * ---

static void _build_star(Lex* l) { lex_add(l, "", "a*b", 0); }

TEST(test_star) {
  char* out = _gen(_build_star);
  assert(strstr(out, "i32 97")); // a
  assert(strstr(out, "i32 98")); // b
  free(out);
}

// --- Ignore case ---

static void _build_icase(Lex* l) { lex_add(l, "i", "abc", 0); }

TEST(test_icase) {
  char* out = _gen(_build_icase);
  // Each letter should match both cases
  assert(strstr(out, "i32 65") || strstr(out, "i32 %cp, 65")); // A
  assert(strstr(out, "i32 97") || strstr(out, "i32 %cp, 97")); // a
  free(out);
}

// --- Ignore case in char class ---

static void _build_icase_class(Lex* l) { lex_add(l, "i", "[a-c]", 0); }

TEST(test_icase_class) {
  char* out = _gen(_build_icase_class);
  // Should have both a-c and A-C
  assert(strstr(out, "i32 %cp, 65") || strstr(out, "i32 65")); // A
  assert(strstr(out, "i32 %cp, 97") || strstr(out, "i32 97")); // a
  free(out);
}

// --- Binary mode ---

static void _build_binary(Lex* l) { lex_add(l, "b", "AB", 0); }

TEST(test_binary) {
  char* out = _gen(_build_binary);
  assert(strstr(out, "i32 65")); // A
  assert(strstr(out, "i32 66")); // B
  free(out);
}

// --- Binary mode dot ---

static void _build_binary_dot(Lex* l) { lex_add(l, "b", ".", 0); }

TEST(test_binary_dot) {
  char* out = _gen(_build_binary_dot);
  // dot in binary matches [0..9, 11..255]
  assert(strstr(out, "i32 %cp, 255"));
  free(out);
}

// --- Multiple patterns ---

static void _build_multi(Lex* l) {
  lex_add(l, "", "if", 0);
  lex_add(l, "", "else", 0);
  lex_add(l, "", "[a-z]+", 0);
}

TEST(test_multi) {
  char* out = _gen(_build_multi);
  assert(strstr(out, "define {i32, i32} @lex"));
  free(out);
}

// --- Error: unmatched paren ---

TEST(test_err_paren) {
  Lex* l = lex_new("test.rules");
  int32_t r = lex_add(l, "", "(abc", 0);
  assert(r == LEX_ERR_PAREN);
  lex_del(l);
}

// --- Error: unclosed bracket ---

TEST(test_err_bracket) {
  Lex* l = lex_new("test.rules");
  int32_t r = lex_add(l, "", "[abc", 0);
  assert(r == LEX_ERR_BRACKET);
  lex_del(l);
}

// --- Complex pattern: identifier ---

static void _build_ident(Lex* l) { lex_add(l, "", "[a-zA-Z_][a-zA-Z0-9_]*", 0); }

TEST(test_ident) {
  char* out = _gen(_build_ident);
  assert(strstr(out, "define {i32, i32} @lex"));
  free(out);
}

// --- Special class in char class ---

static void _build_class_with_special(Lex* l) { lex_add(l, "", "[\\d\\s]", 0); }

TEST(test_class_with_special) {
  char* out = _gen(_build_class_with_special);
  assert(strstr(out, "i32 %cp, 48")); // '0'
  assert(strstr(out, "i32 32"));      // space (switch case)
  free(out);
}

// --- Clang compilation ---

static void _write_and_compile(void (*fn)(Lex*), const char* test_name) {
  char ll_path[128], obj_path[128];
  snprintf(ll_path, sizeof(ll_path), "/tmp/test_lex_%s.ll", test_name);
  snprintf(obj_path, sizeof(obj_path), "/tmp/test_lex_%s.o", test_name);

  FILE* f = fopen(ll_path, "w");
  assert(f);

  Lex* l = lex_new("test.rules");
  fn(l);
  lex_gen(l, f, TARGET);
  lex_del(l);
  fclose(f);

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "xcrun clang -c %s -o %s 2>&1", ll_path, obj_path);
  FILE* p = popen(cmd, "r");
  assert(p);
  char output[4096] = {0};
  size_t n = fread(output, 1, sizeof(output) - 1, p);
  output[n] = '\0';
  int32_t status = pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nclang failed for %s:\n%s\n", test_name, output);
    FILE* ll = fopen(ll_path, "r");
    if (ll) {
      char line[512];
      while (fgets(line, sizeof(line), ll)) {
        fputs(line, stderr);
      }
      fclose(ll);
    }
  }
  assert(status == 0);
  remove(obj_path);
  remove(ll_path);
}

TEST(test_compile_literal) { _write_and_compile(_build_literal, "literal"); }
TEST(test_compile_charclass) { _write_and_compile(_build_charclass, "charclass"); }
TEST(test_compile_neg_charclass) { _write_and_compile(_build_neg_charclass, "neg_charclass"); }
TEST(test_compile_alt) { _write_and_compile(_build_alt, "alt"); }
TEST(test_compile_group) { _write_and_compile(_build_group, "group"); }
TEST(test_compile_optional) { _write_and_compile(_build_optional, "optional"); }
TEST(test_compile_plus) { _write_and_compile(_build_plus, "plus"); }
TEST(test_compile_star) { _write_and_compile(_build_star, "star"); }
TEST(test_compile_icase) { _write_and_compile(_build_icase, "icase"); }
TEST(test_compile_multi) { _write_and_compile(_build_multi, "multi"); }
TEST(test_compile_ident) { _write_and_compile(_build_ident, "ident"); }
TEST(test_compile_dot) { _write_and_compile(_build_dot, "dot"); }

int main(void) {
  printf("test_lex:\n");

  RUN(test_lifecycle);
  RUN(test_action_ids);

  RUN(test_literal);
  RUN(test_charclass);
  RUN(test_neg_charclass);
  RUN(test_c_escape);
  RUN(test_unicode_escape);
  RUN(test_escape_in_class);

  RUN(test_digit_class);
  RUN(test_word_class);
  RUN(test_space_class);
  RUN(test_hex_class);
  RUN(test_dot);

  RUN(test_bof);
  RUN(test_eof);

  RUN(test_alt);
  RUN(test_group);
  RUN(test_optional);
  RUN(test_plus);
  RUN(test_star);

  RUN(test_icase);
  RUN(test_icase_class);
  RUN(test_binary);
  RUN(test_binary_dot);

  RUN(test_multi);
  RUN(test_class_with_special);

  RUN(test_err_paren);
  RUN(test_err_bracket);

  RUN(test_ident);

  RUN(test_compile_literal);
  RUN(test_compile_charclass);
  RUN(test_compile_neg_charclass);
  RUN(test_compile_alt);
  RUN(test_compile_group);
  RUN(test_compile_optional);
  RUN(test_compile_plus);
  RUN(test_compile_star);
  RUN(test_compile_icase);
  RUN(test_compile_multi);
  RUN(test_compile_ident);
  RUN(test_compile_dot);

  printf("all ok\n");
  return 0;
}
