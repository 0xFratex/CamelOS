// usr/menubar.h - Enhanced Menu Bar with System Tray
#ifndef MENUBAR_H
#define MENUBAR_H

#include "../common/gui_types.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

// Menu bar configuration
#define MENU_BAR_HEIGHT     28
#define MENU_BAR_BG_TOP     0xFFF5F5F7
#define MENU_BAR_BG_BOTTOM  0xFFE8E8ED
#define MENU_BAR_TEXT       0xFF1C1C1E
#define MENU_BAR_TEXT_DIM   0xFF8E8E93
#define MENU_BAR_ACCENT     0xFF007AFF

// System tray icons
#define TRAY_ICON_SIZE      18
#define TRAY_SPACING        8
#define TRAY_RIGHT_MARGIN   12

// Menu item types
typedef enum {
    MENU_ITEM_NORMAL,
    MENU_ITEM_SEPARATOR,
    MENU_ITEM_SUBMENU,
    MENU_ITEM_CHECKBOX,
    MENU_ITEM_DISABLED
} MenuItemType;

// Menu item structure
typedef struct {
    char label[48];
    char shortcut[16];
    MenuItemType type;
    int checked;
    int enabled;
    void (*callback)(void);
    struct Menu* submenu;
} MenuItem;

// Menu structure
typedef struct Menu {
    char title[32];
    MenuItem items[16];
    int item_count;
    int is_open;
    int hover_idx;
} Menu;

// System tray item
typedef struct {
    char name[32];
    uint32_t icon_color;
    int (*draw_icon)(int x, int y, int w, int h);
    void (*on_click)(void);
    char tooltip[64];
    int active;
} TrayItem;

// Menu bar state
typedef struct {
    Menu menus[8];
    int menu_count;
    int open_menu_idx;
    int hover_menu_idx;
    
    // System tray
    TrayItem tray_items[8];
    int tray_count;
    
    // Apple menu (system menu)
    int apple_menu_open;
    
    // Clock
    char clock_text[16];
    int clock_width;
    
    // Active app name
    char active_app[64];
} MenuBarState;

// Global menu bar instance
extern MenuBarState g_menu_bar;

// Initialization
void menubar_init(void);
void menubar_reset(void);

// Drawing
void menubar_draw(void);
void menubar_draw_menu(Menu* menu, int x, int y);
void menubar_draw_tray(int x, int y);
void menubar_draw_clock(int x, int y);

// Input handling
int menubar_handle_mouse(int mx, int my, int click, int pressed);
void menubar_handle_key(int key);

// Menu management
Menu* menubar_add_menu(const char* title);
void menubar_add_menu_item(Menu* menu, const char* label, const char* shortcut, void (*callback)(void));
void menubar_add_separator(Menu* menu);

// System tray
void menubar_add_tray_item(const char* name, uint32_t color, int (*draw_fn)(int,int,int,int), void (*click_fn)(void));
void menubar_remove_tray_item(const char* name);
void menubar_update_tray_item(const char* name, int active);

// System tray built-in items
void menubar_add_clock_tray(void);
void menubar_add_network_tray(void);
void menubar_add_volume_tray(void);
void menubar_add_battery_tray(void);

// Updates
void menubar_update_clock(void);
void menubar_set_active_app(const char* app_name);
void menubar_refresh(void);

#endif
