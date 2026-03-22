#pragma once

#include <stdio.h>
#include <stdlib.h>

FILE* compat_open_memstream(char** buf, size_t* size);
void compat_close_memstream(FILE* f, char** buf, size_t* size);
FILE* compat_devnull_w(void);
FILE* compat_popen(const char* cmd, const char* mode);
int compat_pclose(FILE* f);
const char* compat_llvm_cc(void);
