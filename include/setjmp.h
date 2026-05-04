#ifndef _SETJMP_H
#define _SETJMP_H

// Minimal setjmp/longjmp for MuJS exception handling
// We provide a simple implementation using GCC's builtins

typedef int jmp_buf[6];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
