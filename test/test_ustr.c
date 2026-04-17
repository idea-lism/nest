#include "../src/ustr.h"
#include "../src/ustr_intern.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Basic creation / deletion ---

TEST(test_ascii) {
  const char* input = "hello";
  char* s = ustr_new(5, input);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 5);
  assert(memcmp(s, "hello", 5) == 0);
  assert(s[5] == '\0');
  assert(ustr_size(s) == 5);
  ustr_del(s);
}

TEST(test_empty) {
  char* s = ustr_new(0, "");
  assert(s != NULL);
  assert(ustr_bytesize(s) == 0);
  assert(ustr_size(s) == 0);
  assert(s[0] == '\0');
  ustr_del(s);
}

// --- Multi-byte sequences ---

TEST(test_2byte) {
  // "café" = 63 61 66 C3 A9
  const char input[] = "caf\xC3\xA9";
  char* s = ustr_new(5, input);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 5);
  assert(ustr_size(s) == 4);
  ustr_del(s);
}

TEST(test_3byte) {
  // "€" = E2 82 AC
  const char input[] = "\xE2\x82\xAC";
  char* s = ustr_new(3, input);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 3);
  assert(ustr_size(s) == 1);
  ustr_del(s);
}

TEST(test_4byte) {
  // U+1F600 = F0 9F 98 80
  const char input[] = "\xF0\x9F\x98\x80";
  char* s = ustr_new(4, input);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 4);
  assert(ustr_size(s) == 1);
  ustr_del(s);
}

TEST(test_mixed_multibyte) {
  const char input[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                       "b";
  size_t len = sizeof(input) - 1;
  char* s = ustr_new(len, input);
  assert(s != NULL);
  assert(ustr_bytesize(s) == (int32_t)len);
  assert(ustr_size(s) == 5);
  ustr_del(s);
}

// --- Invalid sequences ---

TEST(test_invalid_continuation) {
  char* s = ustr_new(1, "\x80");
  assert(s == NULL);
}

TEST(test_invalid_overlong_2byte) {
  char* s = ustr_new(2, "\xC0\x80");
  assert(s == NULL);
}

TEST(test_invalid_overlong_c1) {
  char* s = ustr_new(2, "\xC1\xBF");
  assert(s == NULL);
}

TEST(test_invalid_surrogate) {
  // ED A0 80 = U+D800
  char* s = ustr_new(3, "\xED\xA0\x80");
  assert(s == NULL);
}

TEST(test_invalid_truncated_2byte) {
  char* s = ustr_new(1, "\xC3");
  assert(s == NULL);
}

TEST(test_invalid_truncated_3byte) {
  char* s = ustr_new(2, "\xE2\x82");
  assert(s == NULL);
}

TEST(test_invalid_truncated_4byte) {
  char* s = ustr_new(3, "\xF0\x9F\x98");
  assert(s == NULL);
}

TEST(test_invalid_too_large) {
  // F4 90 80 80 = U+110000
  char* s = ustr_new(4, "\xF4\x90\x80\x80");
  assert(s == NULL);
}

TEST(test_invalid_f5) {
  char* s = ustr_new(1, "\xF5");
  assert(s == NULL);
}

TEST(test_invalid_fe) {
  char* s = ustr_new(1, "\xFE");
  assert(s == NULL);
}

TEST(test_invalid_ff) {
  char* s = ustr_new(1, "\xFF");
  assert(s == NULL);
}

// --- Iterator ---

TEST(test_iter_ascii) {
  char* s = ustr_new(5, "hello");
  assert(s != NULL);

  UstrIter it;
  ustr_iter_init(&it, s, 0);
  assert(ustr_iter_next(&it) == 'h');
  assert(ustr_iter_next(&it) == 'e');
  assert(ustr_iter_next(&it) == 'l');
  assert(ustr_iter_next(&it) == 'l');
  assert(ustr_iter_next(&it) == 'o');
  assert(it.cp_idx == 5);
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

TEST(test_iter_multibyte) {
  const char input[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                       "b";
  char* s = ustr_new(sizeof(input) - 1, input);
  assert(s != NULL);

  UstrIter it;
  ustr_iter_init(&it, s, 0);
  assert(ustr_iter_next(&it) == 'a');
  assert(ustr_iter_next(&it) == 0xE9);
  assert(ustr_iter_next(&it) == 0x20AC);
  assert(ustr_iter_next(&it) == 0x1F600);
  assert(ustr_iter_next(&it) == 'b');
  assert(ustr_iter_next(&it) == -1);
  assert(it.cp_idx == 5);
  ustr_del(s);
}

TEST(test_iter_from_middle) {
  const char* input = "abcdef";
  char* s = ustr_new(6, input);
  assert(s != NULL);

  UstrIter it;
  ustr_iter_init(&it, s, 3);
  assert(it.cp_idx == 3);
  assert(ustr_iter_next(&it) == 'd');
  assert(ustr_iter_next(&it) == 'e');
  assert(ustr_iter_next(&it) == 'f');
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

TEST(test_iter_from_middle_multibyte) {
  const char input[] = "a\xC3\xA9\xE2\x82\xAC"
                       "b";
  char* s = ustr_new(7, input);
  assert(s != NULL);

  UstrIter it;
  ustr_iter_init(&it, s, 2);
  assert(it.cp_idx == 2);
  assert(ustr_iter_next(&it) == 0x20AC);
  assert(ustr_iter_next(&it) == 'b');
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

// --- iter_seek ---

TEST(test_iter_seek_ascii_close) {
  // close backtrack within 64 cps
  char* s = ustr_new(6, "abcdef");
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  assert(ustr_iter_next(&it) == 'a');
  assert(ustr_iter_next(&it) == 'b');
  assert(ustr_iter_next(&it) == 'c');
  assert(ustr_iter_next(&it) == 'd');
  assert(it.cp_idx == 4);
  // seek back to cp 1
  ustr_iter_seek(&it, 1);
  assert(it.cp_idx == 1);
  assert(ustr_iter_next(&it) == 'b');
  assert(ustr_iter_next(&it) == 'c');
  ustr_del(s);
}

TEST(test_iter_seek_ascii_to_zero) {
  char* s = ustr_new(5, "hello");
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  ustr_iter_next(&it);
  ustr_iter_next(&it);
  ustr_iter_seek(&it, 0);
  assert(it.cp_idx == 0);
  assert(ustr_iter_next(&it) == 'h');
  ustr_del(s);
}

TEST(test_iter_seek_multibyte_close) {
  // a(1) e\u0301(2) \u20ac(3) \U0001f600(4) b(1) = 5 cps, 11 bytes
  const char input[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                       "b";
  char* s = ustr_new(sizeof(input) - 1, input);
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  assert(ustr_iter_next(&it) == 'a');
  assert(ustr_iter_next(&it) == 0xE9);
  assert(ustr_iter_next(&it) == 0x20AC);
  assert(ustr_iter_next(&it) == 0x1F600);
  assert(it.cp_idx == 4);
  // seek back to cp 1 (e\u0301)
  ustr_iter_seek(&it, 1);
  assert(it.cp_idx == 1);
  assert(ustr_iter_next(&it) == 0xE9);
  assert(ustr_iter_next(&it) == 0x20AC);
  ustr_del(s);
}

TEST(test_iter_seek_forward) {
  char* s = ustr_new(6, "abcdef");
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  ustr_iter_next(&it); // a
  assert(it.cp_idx == 1);
  // seek forward to cp 4
  ustr_iter_seek(&it, 4);
  assert(it.cp_idx == 4);
  assert(ustr_iter_next(&it) == 'e');
  assert(ustr_iter_next(&it) == 'f');
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

TEST(test_iter_seek_same_pos) {
  char* s = ustr_new(3, "abc");
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  ustr_iter_next(&it); // a
  ustr_iter_next(&it); // b
  ustr_iter_seek(&it, 2);
  assert(it.cp_idx == 2);
  assert(ustr_iter_next(&it) == 'c');
  ustr_del(s);
}

TEST(test_iter_seek_far_back) {
  // build string > 64 cps to test far backtrack path
  char buf[128];
  for (int i = 0; i < 100; i++) {
    buf[i] = 'a' + (char)(i % 26);
  }
  char* s = ustr_new(100, buf);
  UstrIter it;
  ustr_iter_init(&it, s, 0);
  // advance to cp 80
  for (int i = 0; i < 80; i++) {
    ustr_iter_next(&it);
  }
  assert(it.cp_idx == 80);
  // seek to cp 5 -- far back (5 + 64 = 69 < 80), takes re-init path
  ustr_iter_seek(&it, 5);
  assert(it.cp_idx == 5);
  assert(ustr_iter_next(&it) == 'a' + 5);
  ustr_del(s);
}

// --- find_error ---

TEST(test_find_error_valid) {
  size_t pos;
  assert(ustr_find_error(5, "hello", &pos) == USTR_ERR_NONE);
  assert(ustr_find_error(0, "", &pos) == USTR_ERR_NONE);
  assert(ustr_find_error(5, "caf\xC3\xA9", &pos) == USTR_ERR_NONE);
}

TEST(test_find_error_invalid_continuation) {
  size_t pos;
  assert(ustr_find_error(1, "\x80", &pos) == USTR_ERR_INVALID);
  assert(pos == 0);
}

TEST(test_find_error_invalid_overlong) {
  size_t pos;
  assert(ustr_find_error(2, "\xC0\x80", &pos) == USTR_ERR_INVALID);
  assert(pos == 0);
}

TEST(test_find_error_invalid_surrogate) {
  size_t pos;
  assert(ustr_find_error(3, "\xED\xA0\x80", &pos) == USTR_ERR_INVALID);
  assert(pos == 0);
}

TEST(test_find_error_invalid_mid_string) {
  size_t pos;
  assert(ustr_find_error(4, "abc\xFF", &pos) == USTR_ERR_INVALID);
  assert(pos == 3);
}

TEST(test_find_error_truncated_2byte) {
  size_t pos;
  assert(ustr_find_error(1, "\xC3", &pos) == USTR_ERR_TRUNCATED);
  assert(pos == 0);
}

TEST(test_find_error_truncated_3byte) {
  size_t pos;
  assert(ustr_find_error(2, "\xE2\x82", &pos) == USTR_ERR_TRUNCATED);
  assert(pos == 0);
}

TEST(test_find_error_truncated_4byte) {
  size_t pos;
  assert(ustr_find_error(3, "\xF0\x9F\x98", &pos) == USTR_ERR_TRUNCATED);
  assert(pos == 0);
}

TEST(test_find_error_truncated_mid_string) {
  size_t pos;
  assert(ustr_find_error(6, "hello\xC3", &pos) == USTR_ERR_TRUNCATED);
  assert(pos == 5);
}

// --- Slicing ---

TEST(test_slice_ascii) {
  char* s = ustr_new(5, "hello");
  assert(s != NULL);

  char* sl = ustr_slice(s, 1, 4);
  assert(sl != NULL);
  assert(ustr_bytesize(sl) == 3);
  assert(memcmp(sl, "ell", 3) == 0);
  ustr_del(sl);
  ustr_del(s);
}

TEST(test_slice_multibyte) {
  const char input[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                       "b";
  char* s = ustr_new(sizeof(input) - 1, input);
  assert(s != NULL);

  char* sl = ustr_slice(s, 1, 4);
  assert(sl != NULL);
  assert(ustr_bytesize(sl) == 9);
  assert(ustr_size(sl) == 3);
  ustr_del(sl);
  ustr_del(s);
}

TEST(test_slice_negative) {
  char* s = ustr_new(5, "hello");
  assert(s != NULL);

  char* sl = ustr_slice(s, -2, 5);
  assert(sl != NULL);
  assert(ustr_bytesize(sl) == 2);
  assert(memcmp(sl, "lo", 2) == 0);
  ustr_del(sl);
  ustr_del(s);
}

TEST(test_slice_empty) {
  char* s = ustr_new(5, "hello");
  assert(s != NULL);

  char* sl = ustr_slice(s, 3, 3);
  assert(sl != NULL);
  assert(ustr_bytesize(sl) == 0);
  ustr_del(sl);
  ustr_del(s);
}

// --- Large input (exercises NEON 16-byte chunks + tail) ---

TEST(test_large_ascii) {
  char buf[1024];
  memset(buf, 'x', sizeof(buf));
  char* s = ustr_new(sizeof(buf), buf);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 1024);
  assert(ustr_size(s) == 1024);
  ustr_del(s);
}

TEST(test_large_multibyte) {
  char buf[512];
  for (int i = 0; i < 256; i++) {
    buf[i * 2 + 0] = '\xC3';
    buf[i * 2 + 1] = '\xA9';
  }
  char* s = ustr_new(512, buf);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 512);
  assert(ustr_size(s) == 256);

  UstrIter it;
  ustr_iter_init(&it, s, 0);
  for (int i = 0; i < 256; i++) {
    int32_t cp = ustr_iter_next(&it);
    assert(cp == 0xE9);
  }
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

TEST(test_large_mixed) {
  const char unit[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  size_t unit_len = sizeof(unit) - 1;
  int reps = 20;
  char buf[200];
  for (int i = 0; i < reps; i++) {
    memcpy(buf + i * unit_len, unit, unit_len);
  }

  char* s = ustr_new(200, buf);
  assert(s != NULL);
  assert(ustr_bytesize(s) == 200);
  assert(ustr_size(s) == 4 * reps);

  UstrIter it;
  ustr_iter_init(&it, s, 0);
  for (int i = 0; i < reps; i++) {
    assert(ustr_iter_next(&it) == 'a');
    assert(ustr_iter_next(&it) == 0xE9);
    assert(ustr_iter_next(&it) == 0x20AC);
    assert(ustr_iter_next(&it) == 0x1F600);
  }
  assert(ustr_iter_next(&it) == -1);
  ustr_del(s);
}

// --- Scalar vs NEON consistency ---

TEST(test_scalar_neon_consistency) {
  const char input[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                       "hello world!\xC3\xA9\xE2\x82\xAC";
  size_t len = sizeof(input) - 1;

  size_t mark_sz = (len + 7) / 8;
  uint8_t marks_s[8] = {0};
  uint8_t marks_n[8] = {0};

  int rs = ustr_validate_scalar((const uint8_t*)input, len, marks_s);
  int rn = ustr_validate((const uint8_t*)input, len, marks_n);
  assert(rs == 0);
  assert(rn == 0);
  assert(memcmp(marks_s, marks_n, mark_sz) == 0);
}

int main(void) {
  printf("test_ustr:\n");

  RUN(test_ascii);
  RUN(test_empty);
  RUN(test_2byte);
  RUN(test_3byte);
  RUN(test_4byte);
  RUN(test_mixed_multibyte);

  RUN(test_invalid_continuation);
  RUN(test_invalid_overlong_2byte);
  RUN(test_invalid_overlong_c1);
  RUN(test_invalid_surrogate);
  RUN(test_invalid_truncated_2byte);
  RUN(test_invalid_truncated_3byte);
  RUN(test_invalid_truncated_4byte);
  RUN(test_invalid_too_large);
  RUN(test_invalid_f5);
  RUN(test_invalid_fe);
  RUN(test_invalid_ff);

  RUN(test_iter_ascii);
  RUN(test_iter_multibyte);
  RUN(test_iter_from_middle);
  RUN(test_iter_from_middle_multibyte);

  RUN(test_iter_seek_ascii_close);
  RUN(test_iter_seek_ascii_to_zero);
  RUN(test_iter_seek_multibyte_close);
  RUN(test_iter_seek_forward);
  RUN(test_iter_seek_same_pos);
  RUN(test_iter_seek_far_back);

  RUN(test_find_error_valid);
  RUN(test_find_error_invalid_continuation);
  RUN(test_find_error_invalid_overlong);
  RUN(test_find_error_invalid_surrogate);
  RUN(test_find_error_invalid_mid_string);
  RUN(test_find_error_truncated_2byte);
  RUN(test_find_error_truncated_3byte);
  RUN(test_find_error_truncated_4byte);
  RUN(test_find_error_truncated_mid_string);

  RUN(test_slice_ascii);
  RUN(test_slice_multibyte);
  RUN(test_slice_negative);
  RUN(test_slice_empty);

  RUN(test_large_ascii);
  RUN(test_large_multibyte);
  RUN(test_large_mixed);

  RUN(test_scalar_neon_consistency);

  printf("all ok\n");
  return 0;
}
