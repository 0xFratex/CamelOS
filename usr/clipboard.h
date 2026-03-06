#ifndef CLIPBOARD_H
#define CLIPBOARD_H

// Global Clipboard State
// Defined in bubbleview.c (or main entry), extern here
extern char clipboard_path[128];
extern int clipboard_active; // 0 = Empty, 1 = Has Content
extern int clipboard_op;     // 0 = Copy, 1 = Cut

#endif
