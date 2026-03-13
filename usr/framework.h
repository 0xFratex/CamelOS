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
#define MAX_DOCK_APPS 8

typedef window_t Window;

typedef struct {
    char label[16];
    char exec_path[64]; 
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
                        void(*pf)(int,int,int,int), 
                        void(*inf)(int),
                        void(*mf)(int,int,int));

void fw_register_dock(const char* label, int color, Window* win);

#endif