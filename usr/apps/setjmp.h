#ifndef SETJMP_H
#define SETJMP_H

typedef int jmp_buf[6];

extern int setjmp(jmp_buf env);
extern void longjmp(jmp_buf env, int val);

#endif
