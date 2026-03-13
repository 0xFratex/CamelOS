#include "framework.h"
#include "../core/window_server.h"
#include "../core/string.h"
#include "dock.h" // FIX: Include dock header

// --- Window Management (Now using Window Server) ---
Window* fw_create_window(const char* title, int w, int h,
                        void(*pf)(int,int,int,int),
                        void(*inf)(int),
                        void(*mf)(int,int,int))
{
    // Now delegate to Window Server
    window_t* win = ws_create_window(title, w, h, pf, inf, mf);
    
    if (win) {
        // FIX: Use Dock API instead of accessing arrays directly
        dock_bind_window((Window*)win);
        return (Window*)win;
    }
    
    return 0;
}

void fw_register_dock(const char* label, int color, Window* win) {
    // FIX: Forward to Dock API
    dock_register(label, color, win);
}