#ifndef FRAMEWORK_H
#define FRAMEWORK_H

#include "../sys/api.h"

#include "../common/gui_types.h"
#include "../core/window_server.h"

// Context Menu Types
#define CTX_DESKTOP 0
#define CTX_WINDOW_HEADER 1
#define CTX_FILE_ICON 2 

// === FIX: MATCH KERNEL WINDOW SERVER LIMIT ===
#define MAX_WINDOWS 32 
#define MAX_DOCK_APPS 12

typedef window_t Window;

typedef struct {
    char label[16];
    char exec_path[64]; 
    char icon_res[16];  // Icon resource name (e.g. "folder", "terminal", "browser")
    int color;
    Window* window_ref; 
} DockIcon;

extern Window windows[MAX_WINDOWS];
extern DockIcon dock_icons[MAX_DOCK_APPS];
extern int win_count;
extern int dock_count;
extern window_t* active_win;

void fw_open_context_menu(int x, int y, int type);

Window* fw_create_window(const char* title, int w, int h, 
                        void(*pf)(window_t*,int,int,int,int), 
                        void(*inf)(window_t*,int),
                        void(*mf)(window_t*,int,int,int));

void fw_register_dock(const char* label, int color, Window* win);

#endif