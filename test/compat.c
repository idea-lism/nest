#include "compat.h"

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>

FILE* compat_open_memstream(char** buf, size_t* size) {
  *buf = NULL;
  *size = 0;
  return tmpfile();
}

void compat_close_memstream(FILE* f, char** buf, size_t* size) {
  long len = ftell(f);
  if (len < 0) {
    len = 0;
  }
  *buf = (char*)malloc((size_t)len + 1);
  rewind(f);
  *size = fread(*buf, 1, (size_t)len, f);
  (*buf)[*size] = '\0';
  fclose(f);
}

FILE* compat_devnull_w(void) { return fopen("NUL", "w"); }

FILE* compat_popen(const char* cmd, const char* mode) { return _popen(cmd, mode); }

int compat_pclose(FILE* f) { return _pclose(f); }

#else

#include <stdio.h>
#include <stdlib.h>

FILE* compat_open_memstream(char** buf, size_t* size) { return open_memstream(buf, size); }

void compat_close_memstream(FILE* f, char** buf, size_t* size) {
  (void)buf;
  (void)size;
  fclose(f);
}

FILE* compat_devnull_w(void) { return fopen("/dev/null", "w"); }

FILE* compat_popen(const char* cmd, const char* mode) { return popen(cmd, mode); }

int compat_pclose(FILE* f) { return pclose(f); }

#endif

const char* compat_llvm_cc(void) {
  const char* cc = getenv("LLVM_CC");
  if (cc) {
    return cc;
  }
#ifdef __APPLE__
  return "xcrun clang";
#else
  return "clang";
#endif
}
