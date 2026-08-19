// core/user_exec.h - Launch raw binaries as Ring 3 user processes
#ifndef USER_EXEC_H
#define USER_EXEC_H

#include "../include/types.h"

// Map `code`/`size` into a new user address space at USER_CODE_START and
// schedule it as a Ring 3 task. Returns 0 on success, -1 on failure.
int user_exec_raw(const char* name, const void* code, uint32_t size);

#endif
