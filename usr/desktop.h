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
extern void desktop_draw_icons(uint32_t* buffer);
extern void desktop_fill_wallpaper_region(uint32_t* buffer, int rx, int ry, int rw, int rh);
extern void desktop_on_mouse(int x, int y, int lb, int rb);
// desktop_on_input removed (handled directly in bubbleview logic now)

// Dynamic desktop path - resolved from user config at init time
extern char g_desktop_path[128];

// Check if context menu is currently open (for modal behavior)
extern int desktop_is_ctx_open();

// Check if the desktop rubber-band selection is currently being dragged
// (used by bubbleview.c to keep dispatching mouse events during the drag)
extern int desktop_selbox_active();

// Cancel the desktop selbox (e.g., when a double-click opens an item)
extern void desktop_cancel_selbox();

// Context menu functions (defined in bubbleview.c)
extern void ctx_menu_show(int x, int y, int type, void* target);
extern void desktop_refresh();

// Drag-to-Applications install support
// Called when a .app bundle or .dmg is dragged to the dock/Applications area
extern void desktop_install_app(const char* source_path);
extern int desktop_is_droppable(int x, int y); // Check if position is the Applications drop zone

#endif