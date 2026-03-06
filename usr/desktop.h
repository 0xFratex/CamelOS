#ifndef DESKTOP_H
#define DESKTOP_H

typedef unsigned int uint32_t;

// State shared between bubbleview (input) and desktop (render)
extern int desktop_rename_active;
extern int desktop_rename_idx;
extern char desktop_rename_buf[64];
extern int desktop_rename_cursor;

extern void desktop_init();
extern void desktop_draw(uint32_t* buffer);
extern void desktop_on_mouse(int x, int y, int lb, int rb);
// desktop_on_input removed (handled directly in bubbleview logic now)

// Check if context menu is currently open (for modal behavior)
extern int desktop_is_ctx_open();

// Context menu functions (defined in bubbleview.c)
extern void ctx_menu_show(int x, int y, int type, void* target);
extern void desktop_refresh();

#endif