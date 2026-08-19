// usr/test/user_calc.c — Ring 3 "Calculator" proof: opens a window and draws a
// UI entirely through syscalls (no kernel function-pointer callbacks).
#include "../../sys/syscalls.h"

#define ARGB(a,r,g,b) (((a)<<24)|((r)<<16)|((g)<<8)|(b))

void _start(void) {
    int x = 0, y = 0;

    // SYS_USER_WIN_CREATE(title, w, h, &x, &y) -> window id
    int win = syscall5(SYS_USER_WIN_CREATE,
                       (int)"Calculator", 280, 420, (int)&x, (int)&y);

    // Display area (dark) + "0"
    syscall5(SYS_DRAW_RECT, x, y, 280, 80, ARGB(0xFF,0x22,0x22,0x22));
    syscall4(SYS_DRAW_TEXT, x + 12, y + 20, (int)"0", ARGB(0xFF,0xFF,0xFF,0xFF));

    // 4x4 button grid (light rounded-ish squares)
    int bw = 60, bh = 55, gap = 8;
    int bx = x + gap;
    int by = y + 96;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int px = bx + c * (bw + gap);
            int py = by + r * (bh + gap);
            syscall5(SYS_DRAW_RECT, px, py, bw, bh, ARGB(0xFF,0xE8,0xE8,0xE8));
        }
    }

    syscall1(SYS_PRINT, (int)"[calc] ring3 window drawn\n");
    // Exit cleanly: a busy-loop at a normal priority would starve the GUI,
    // because the desktop main context currently shares the idle task slot.
    syscall1(SYS_USER_EXIT, 0);
    for (;;) { __asm__ volatile("pause"); }
}
