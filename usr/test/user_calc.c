// usr/test/user_calc.c — Ring 3 "Calculator" proof: opens a window and draws a
// UI entirely through syscalls (no kernel function-pointer callbacks).
//
// Phase 1 smoke test for the Ring 3 migration:
//   1. Prints [calc] markers to serial BEFORE and AFTER drawing so an
//      automated QEMU boot log can verify the process actually ran at CPL 3.
//   2. Holds the window on screen for ~2.5 s (polling SYS_GET_TICKS) so a
//      human can see it. NOTE: while a Ring 3 task is READY, the desktop
//      (which shares the idle task slot) is not scheduled — the GUI is
//      briefly frozen by design until Phase 2 moves it into a real task.
//   3. Exits cleanly via SYS_USER_EXIT (process_exit -> ZOMBIE -> reschedule).
#include "../../sys/syscalls.h"

#define ARGB(a,r,g,b) (((a)<<24)|((r)<<16)|((g)<<8)|(b))

static void wait_ticks(unsigned ticks) {
    unsigned start = (unsigned)syscall0(SYS_GET_TICKS);
    for (;;) {
        unsigned now = (unsigned)syscall0(SYS_GET_TICKS);
        if (now - start >= ticks) return;
        __asm__ volatile("pause");
    }
}

void _start(void) {
    syscall1(SYS_PRINT, (int)"[calc] ring3 process entered CPL3, creating window\n");

    int x = 0, y = 0;

    // SYS_USER_WIN_CREATE(title, w, h, &x, &y) -> window id
    // &x / &y are USER pointers — the kernel validates them and writes back
    // via copy_to_user (SYS_USER_WIN_CREATE hardened in this branch).
    int win = syscall5(SYS_USER_WIN_CREATE,
                       (int)"Calculator", 280, 420, (int)&x, (int)&y);
    if (win < 0) {
        syscall1(SYS_PRINT, (int)"[calc] FAIL: window create returned error\n");
        syscall1(SYS_USER_EXIT, 1);
        for (;;) __asm__ volatile("pause");
    }

    // Display area (dark) + "0"
    syscall5(SYS_DRAW_RECT, x, y, 280, 80, ARGB(0xFF,0x22,0x22,0x22));
    syscall4(SYS_DRAW_TEXT, x + 12, y + 20, (int)"0", ARGB(0xFF,0xFF,0xFF,0xFF));

    // 4x4 button grid (light squares)
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

    // Keep the window visible long enough to see it (and for the smoke test
    // to be observable), then exit.
    wait_ticks(125);  /* ~2.5 s at 50 Hz */

    syscall1(SYS_PRINT, (int)"[calc] ring3 process exiting cleanly\n");
    syscall1(SYS_USER_EXIT, 0);
    for (;;) { __asm__ volatile("pause"); }
}
