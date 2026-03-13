// usr/menubar.c - Enhanced Menu Bar Implementation
#include "menubar.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../common/time.h"
#include "../hal/drivers/serial.h"

MenuBarState g_menu_bar;

// External API
extern kernel_api_t* sys;
extern int net_is_connected;
extern void rtl8139_poll(void);
extern int screen_w;  // From gfx_hal
extern int screen_h;  // From gfx_hal

// ============================================================================
// Drawing Helpers
// ============================================================================

static void draw_gradient_rect(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color) {
    for (int row = 0; row < h; row++) {
        // Simple gradient interpolation
        uint32_t r1 = (top_color >> 16) & 0xFF;
        uint32_t g1 = (top_color >> 8) & 0xFF;
        uint32_t b1 = top_color & 0xFF;
        uint32_t r2 = (bottom_color >> 16) & 0xFF;
        uint32_t g2 = (bottom_color >> 8) & 0xFF;
        uint32_t b2 = bottom_color & 0xFF;
        
        int progress = (row * 256) / h;
        uint32_t r = r1 + ((r2 - r1) * progress) / 256;
        uint32_t g = g1 + ((g2 - g1) * progress) / 256;
        uint32_t b = b1 + ((b2 - b1) * progress) / 256;
        
        uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
        sys->draw_rect(x, y + row, w, 1, color);
    }
}

static void draw_apple_logo(int x, int y) {
    // Simple apple-like shape using circles
    uint32_t color = MENU_BAR_TEXT;
    
    // Apple body (overlapping circles)
    sys->draw_rect(x + 6, y + 4, 4, 8, color);   // Left
    sys->draw_rect(x + 10, y + 4, 4, 8, color);  // Right
    sys->draw_rect(x + 5, y + 6, 10, 6, color);  // Middle
    
    // Leaf
    sys->draw_rect(x + 9, y, 2, 4, color);
    sys->draw_rect(x + 10, y + 1, 2, 2, color);
}

static void draw_wifi_icon(int x, int y, int strength) {
    uint32_t color = strength > 0 ? MENU_BAR_TEXT : MENU_BAR_TEXT_DIM;
    
    // WiFi arcs
    if (strength >= 1) sys->draw_rect(x + 6, y + 12, 4, 2, color);
    if (strength >= 2) sys->draw_rect(x + 4, y + 8, 8, 2, color);
    if (strength >= 3) sys->draw_rect(x + 2, y + 4, 12, 2, color);
    if (strength >= 4) sys->draw_rect(x, y, 16, 2, color);
}

static void draw_volume_icon(int x, int y, int level) {
    uint32_t color = MENU_BAR_TEXT;
    
    // Speaker body
    sys->draw_rect(x + 2, y + 4, 4, 8, color);
    sys->draw_rect(x + 6, y + 2, 2, 12, color);
    sys->draw_rect(x + 8, y, 2, 16, color);
    
    // Sound waves
    if (level > 0) sys->draw_rect(x + 12, y + 4, 2, 8, color);
    if (level > 50) sys->draw_rect(x + 16, y + 2, 2, 12, color);
}

static void draw_battery_icon(int x, int y, int percent) {
    uint32_t color = percent > 20 ? 0xFF34C759 : 0xFFFF3B30; // Green or red
    
    // Battery body
    sys->draw_rect(x, y + 2, 20, 10, MENU_BAR_TEXT);
    sys->draw_rect(x + 1, y + 3, 18, 8, MENU_BAR_BG_TOP);
    
    // Fill level
    int fill = (percent * 16) / 100;
    if (fill > 0) sys->draw_rect(x + 2, y + 4, fill, 6, color);
    
    // Terminal
    sys->draw_rect(x + 20, y + 5, 2, 6, MENU_BAR_TEXT);
}

// ============================================================================
// Initialization
// ============================================================================

void menubar_init(void) {
    memset(&g_menu_bar, 0, sizeof(g_menu_bar));
    strcpy(g_menu_bar.active_app, "Camel OS");
    
    // Add default system tray items
    menubar_add_network_tray();
    menubar_add_volume_tray();
    menubar_add_clock_tray();
}

void menubar_reset(void) {
    g_menu_bar.menu_count = 0;
    g_menu_bar.open_menu_idx = -1;
    g_menu_bar.hover_menu_idx = -1;
}

// ============================================================================
// Menu Management
// ============================================================================

Menu* menubar_add_menu(const char* title) {
    if (g_menu_bar.menu_count >= 8) return NULL;
    
    Menu* menu = &g_menu_bar.menus[g_menu_bar.menu_count++];
    strncpy(menu->title, title, 31);
    menu->title[31] = 0;
    menu->item_count = 0;
    menu->is_open = 0;
    menu->hover_idx = -1;
    
    return menu;
}

void menubar_add_menu_item(Menu* menu, const char* label, const char* shortcut, void (*callback)(void)) {
    if (!menu || menu->item_count >= 16) return;
    
    MenuItem* item = &menu->items[menu->item_count++];
    strncpy(item->label, label, 47);
    item->label[47] = 0;
    if (shortcut) {
        strncpy(item->shortcut, shortcut, 15);
        item->shortcut[15] = 0;
    } else {
        item->shortcut[0] = 0;
    }
    item->type = MENU_ITEM_NORMAL;
    item->enabled = 1;
    item->callback = callback;
    item->submenu = NULL;
}

void menubar_add_separator(Menu* menu) {
    if (!menu || menu->item_count >= 16) return;
    
    MenuItem* item = &menu->items[menu->item_count++];
    item->type = MENU_ITEM_SEPARATOR;
    item->label[0] = 0;
}

// ============================================================================
// System Tray
// ============================================================================

void menubar_add_tray_item(const char* name, uint32_t color, int (*draw_fn)(int,int,int,int), void (*click_fn)(void)) {
    if (g_menu_bar.tray_count >= 8) return;
    
    TrayItem* item = &g_menu_bar.tray_items[g_menu_bar.tray_count++];
    strncpy(item->name, name, 31);
    item->name[31] = 0;
    item->icon_color = color;
    item->draw_icon = draw_fn;
    item->on_click = click_fn;
    item->active = 1;
}

void menubar_remove_tray_item(const char* name) {
    for (int i = 0; i < g_menu_bar.tray_count; i++) {
        if (strcmp(g_menu_bar.tray_items[i].name, name) == 0) {
            // Shift remaining items
            for (int j = i; j < g_menu_bar.tray_count - 1; j++) {
                g_menu_bar.tray_items[j] = g_menu_bar.tray_items[j + 1];
            }
            g_menu_bar.tray_count--;
            return;
        }
    }
}

void menubar_update_tray_item(const char* name, int active) {
    for (int i = 0; i < g_menu_bar.tray_count; i++) {
        if (strcmp(g_menu_bar.tray_items[i].name, name) == 0) {
            g_menu_bar.tray_items[i].active = active;
            return;
        }
    }
}

// Built-in tray items
static int network_draw_fn(int x, int y, int w, int h) {
    int strength = net_is_connected ? 4 : 0;
    draw_wifi_icon(x, y, strength);
    return TRAY_ICON_SIZE;
}

static void network_click_fn(void) {
    // Open network settings
    extern int wrap_exec(const char*);
    wrap_exec("/usr/apps/NetTools.app");
}

void menubar_add_network_tray(void) {
    menubar_add_tray_item("Network", MENU_BAR_ACCENT, network_draw_fn, network_click_fn);
}

static int volume_draw_fn(int x, int y, int w, int h) {
    draw_volume_icon(x, y, 75);
    return TRAY_ICON_SIZE;
}

static void volume_click_fn(void) {
    // Toggle mute or open volume control
}

void menubar_add_volume_tray(void) {
    menubar_add_tray_item("Volume", MENU_BAR_TEXT, volume_draw_fn, volume_click_fn);
}

static int battery_draw_fn(int x, int y, int w, int h) {
    draw_battery_icon(x, y, 85);
    return 24;
}

void menubar_add_battery_tray(void) {
    menubar_add_tray_item("Battery", 0xFF34C759, battery_draw_fn, NULL);
}

void menubar_add_clock_tray(void) {
    // Clock is drawn separately, not as a tray item
}

// ============================================================================
// Updates
// ============================================================================

void menubar_update_clock(void) {
    // Get current time
    int hour, minute, second;
    sys_get_time(&hour, &minute, &second);
    
    // Format time string
    char hours[8], minutes[8];
    extern void int_to_str(int, char*);
    int_to_str(hour, hours);
    int_to_str(minute, minutes);
    
    // Pad single digits
    if (hour < 10) {
        g_menu_bar.clock_text[0] = '0';
        g_menu_bar.clock_text[1] = '0' + hour;
        g_menu_bar.clock_text[2] = 0;
    } else {
        g_menu_bar.clock_text[0] = hours[0];
        g_menu_bar.clock_text[1] = hours[1] ? hours[1] : hours[0];
        g_menu_bar.clock_text[2] = 0;
    }
    
    strcat(g_menu_bar.clock_text, ":");
    
    if (minute < 10) {
        int len = strlen(g_menu_bar.clock_text);
        g_menu_bar.clock_text[len] = '0';
        g_menu_bar.clock_text[len + 1] = '0' + minute;
        g_menu_bar.clock_text[len + 2] = 0;
    } else {
        strcat(g_menu_bar.clock_text, minutes);
    }
    
    g_menu_bar.clock_width = strlen(g_menu_bar.clock_text) * 8;
}

void menubar_set_active_app(const char* app_name) {
    strncpy(g_menu_bar.active_app, app_name, 63);
    g_menu_bar.active_app[63] = 0;
}

void menubar_refresh(void) {
    menubar_update_clock();
}

// ============================================================================
// Drawing
// ============================================================================

void menubar_draw(void) {
    // Draw gradient background
    int sw = screen_w ? screen_w : 1024;
    draw_gradient_rect(0, 0, sw, MENU_BAR_HEIGHT, MENU_BAR_BG_TOP, MENU_BAR_BG_BOTTOM);
    
    // Draw bottom border
    sys->draw_rect(0, MENU_BAR_HEIGHT - 1, sw, 1, 0xFFC6C6C8);
    
    int x = 12;
    
    // Draw Apple logo
    draw_apple_logo(x, 6);
    x += 24;
    
    // Draw active app name (bold)
    sys->draw_text(x, 8, g_menu_bar.active_app, MENU_BAR_TEXT);
    x += strlen(g_menu_bar.active_app) * 8 + 24;
    
    // Draw menus
    for (int i = 0; i < g_menu_bar.menu_count; i++) {
        Menu* menu = &g_menu_bar.menus[i];
        int w = strlen(menu->title) * 8 + 16;
        
        // Highlight if open or hover
        if (i == g_menu_bar.open_menu_idx || i == g_menu_bar.hover_menu_idx) {
            sys->draw_rect_rounded(x, 4, w, 20, 4, MENU_BAR_ACCENT);
            sys->draw_text(x + 8, 8, menu->title, 0xFFFFFFFF);
        } else {
            sys->draw_text(x + 8, 8, menu->title, MENU_BAR_TEXT);
        }
        
        // Store position for menu drawing
        menu->is_open = (i == g_menu_bar.open_menu_idx);
        
        x += w;
    }
    
    // Draw system tray (right-aligned)
    int tray_x = sw - TRAY_RIGHT_MARGIN;
    
    // Clock
    menubar_update_clock();
    tray_x -= g_menu_bar.clock_width + TRAY_SPACING;
    sys->draw_text(tray_x, 8, g_menu_bar.clock_text, MENU_BAR_TEXT);
    
    // Tray icons (right to left)
    for (int i = g_menu_bar.tray_count - 1; i >= 0; i--) {
        TrayItem* item = &g_menu_bar.tray_items[i];
        if (!item->active) continue;
        
        tray_x -= TRAY_ICON_SIZE + TRAY_SPACING;
        if (item->draw_icon) {
            item->draw_icon(tray_x, 5, TRAY_ICON_SIZE, TRAY_ICON_SIZE);
        }
    }
    
    // Draw open menu
    if (g_menu_bar.open_menu_idx >= 0 && g_menu_bar.open_menu_idx < g_menu_bar.menu_count) {
        // Calculate menu position
        int menu_x = 12 + 24 + strlen(g_menu_bar.active_app) * 8 + 24;
        for (int i = 0; i < g_menu_bar.open_menu_idx; i++) {
            menu_x += strlen(g_menu_bar.menus[i].title) * 8 + 16;
        }
        menubar_draw_menu(&g_menu_bar.menus[g_menu_bar.open_menu_idx], menu_x, MENU_BAR_HEIGHT);
    }
}

void menubar_draw_menu(Menu* menu, int x, int y) {
    if (!menu || menu->item_count == 0) return;
    
    int w = 200;
    int h = menu->item_count * 24 + 8;
    
    // Shadow
    sys->draw_rect_rounded(x + 4, y + 4, w, h, 8, 0x40000000);
    
    // Background
    sys->draw_rect_rounded(x, y, w, h, 8, 0xFFF2F2F7);
    sys->draw_rect(x + 1, y + 1, w - 2, h - 2, 0xFFFFFFFF);
    
    // Items
    int iy = y + 4;
    for (int i = 0; i < menu->item_count; i++) {
        MenuItem* item = &menu->items[i];
        
        if (item->type == MENU_ITEM_SEPARATOR) {
            sys->draw_rect(x + 12, iy + 11, w - 24, 1, 0xFFE5E5EA);
        } else {
            // Highlight
            if (i == menu->hover_idx) {
                sys->draw_rect_rounded(x + 4, iy, w - 8, 22, 4, MENU_BAR_ACCENT);
                sys->draw_text(x + 12, iy + 6, item->label, 0xFFFFFFFF);
            } else {
                uint32_t color = item->enabled ? MENU_BAR_TEXT : MENU_BAR_TEXT_DIM;
                sys->draw_text(x + 12, iy + 6, item->label, color);
            }
            
            // Shortcut
            if (item->shortcut[0]) {
                sys->draw_text(x + w - 60, iy + 6, item->shortcut, MENU_BAR_TEXT_DIM);
            }
        }
        
        iy += 24;
    }
}

// ============================================================================
// Input Handling
// ============================================================================

int menubar_handle_mouse(int mx, int my, int click, int pressed) {
    // Check if in menu bar
    if (my >= MENU_BAR_HEIGHT) {
        // Close any open menu
        if (g_menu_bar.open_menu_idx >= 0 && !pressed) {
            g_menu_bar.open_menu_idx = -1;
        }
        g_menu_bar.hover_menu_idx = -1;
        return 0;
    }
    
    // Check Apple logo
    if (mx < 36) {
        if (click) {
            g_menu_bar.apple_menu_open = !g_menu_bar.apple_menu_open;
        }
        return 1;
    }
    
    // Check active app area
    int x = 36;
    int app_w = strlen(g_menu_bar.active_app) * 8 + 24;
    if (mx < x + app_w) {
        return 1;
    }
    x += app_w;
    
    // Check menus
    for (int i = 0; i < g_menu_bar.menu_count; i++) {
        int w = strlen(g_menu_bar.menus[i].title) * 8 + 16;
        if (mx >= x && mx < x + w) {
            g_menu_bar.hover_menu_idx = i;
            
            if (click && pressed) {
                g_menu_bar.open_menu_idx = (g_menu_bar.open_menu_idx == i) ? -1 : i;
            }
            return 1;
        }
        x += w;
    }
    
    // Check system tray
    int tray_x = screen_w ? screen_w : 1024;
    tray_x -= TRAY_RIGHT_MARGIN;
    tray_x -= g_menu_bar.clock_width + TRAY_SPACING;
    
    for (int i = g_menu_bar.tray_count - 1; i >= 0; i--) {
        TrayItem* item = &g_menu_bar.tray_items[i];
        if (!item->active) continue;
        
        tray_x -= TRAY_ICON_SIZE + TRAY_SPACING;
        if (mx >= tray_x && mx < tray_x + TRAY_ICON_SIZE) {
            if (click && item->on_click) {
                item->on_click();
            }
            return 1;
        }
    }
    
    // Click on empty area closes menu
    if (click && !pressed) {
        g_menu_bar.open_menu_idx = -1;
    }
    
    g_menu_bar.hover_menu_idx = -1;
    return 0;
}

void menubar_handle_key(int key) {
    // Handle keyboard shortcuts
    // TODO: Implement keyboard navigation
}
