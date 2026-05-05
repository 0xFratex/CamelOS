// usr/spotlight.c - Spotlight-like Search Implementation
// A macOS-inspired global search overlay for CamelOS
// Activated by Cmd+Space (or Ctrl+Space)

#include "spotlight.h"
#include "lib/camel_framework.h"
#include "desktop.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../hal/drivers/serial.h"
#include "../include/input_defs.h"
#include "../sys/cdl_defs.h"

// External references
extern kernel_api_t* sys;
extern int screen_w;
extern int screen_h;

// Global spotlight state
SpotlightState g_spotlight;

// Registered items for searching (static pool)
#define MAX_REGISTERED 64
static SpotlightResult registered_items[MAX_REGISTERED];
static int registered_count = 0;

// Spotlight layout constants
#define SPOTLIGHT_WIDTH        520
#define SPOTLIGHT_INPUT_H      44
#define SPOTLIGHT_RESULT_H     36
#define SPOTLIGHT_MAX_VISIBLE  7
#define SPOTLIGHT_BORDER_R     12
#define SPOTLIGHT_PADDING      16

// Colors (macOS Spotlight-inspired)
#define COLOR_OVERLAY          0x40000000
#define COLOR_SEARCH_BG        0xFFE8E8ED
#define COLOR_SEARCH_BORDER    0xFFC7C7CC
#define COLOR_SEARCH_FOCUS     0xFF007AFF
#define COLOR_TEXT_PRIMARY     0xFF1C1C1E
#define COLOR_TEXT_SECONDARY   0xFF8E8E93
#define COLOR_TEXT_PLACEHOLDER 0xFFAEAEB2
#define COLOR_SELECTED_BG      0xFF007AFF
#define COLOR_SELECTED_TEXT    0xFFFFFFFF
#define COLOR_ICON_APP         0xFF007AFF
#define COLOR_ICON_FILE        0xFF34C759
#define COLOR_ICON_COMMAND     0xFFFF9500
#define COLOR_ICON_SETTING     0xFF5856D6
#define COLOR_ICON_CONTACT     0xFFFF2D55
#define COLOR_SEARCH_ICON      0xFF8E8E93

// Forward declarations for app launch helpers
static void launch_terminal(void);
static void launch_files(void);
static void launch_textedit(void);
static void launch_browser(void);
static void launch_calculator(void);
static void launch_settings(void);
static void launch_console(void);
static void launch_sysmon(void);

// ============================================================================
// Initialization
// ============================================================================

void spotlight_init(void) {
    g_spotlight.active = 0;
    g_spotlight.query[0] = '\0';
    g_spotlight.query_len = 0;
    g_spotlight.cursor_pos = 0;
    g_spotlight.result_count = 0;
    g_spotlight.selected_idx = 0;
    g_spotlight.scroll_offset = 0;
    registered_count = 0;

    spotlight_register_defaults();

    s_printf("[SPOTLIGHT] Initialized\n");
}

void spotlight_register_defaults(void) {
    // Register built-in applications
    spotlight_register_result("Terminal",     "Command-line interface",    RESULT_APP, 'T', COLOR_ICON_APP,    launch_terminal);
    spotlight_register_result("Files",        "File manager",              RESULT_APP, 'F', COLOR_ICON_APP,    launch_files);
    spotlight_register_result("TextEdit",     "Text editor",               RESULT_APP, 'T', COLOR_ICON_APP,    launch_textedit);
    spotlight_register_result("Browser",      "Web browser",               RESULT_APP, 'B', COLOR_ICON_APP,    launch_browser);
    spotlight_register_result("Calculator",   "Basic calculator",          RESULT_APP, 'C', COLOR_ICON_APP,    launch_calculator);
    spotlight_register_result("Settings",     "System preferences",        RESULT_APP, 'S', COLOR_ICON_SETTING, launch_settings);
    spotlight_register_result("Console",      "System log viewer",         RESULT_APP, 'C', COLOR_ICON_APP,    launch_console);
    spotlight_register_result("Activity Monitor", "Process and system monitor", RESULT_APP, 'A', COLOR_ICON_APP, launch_sysmon);

    // Register commands
    spotlight_register_result("Lock Screen",     "Lock the display",          RESULT_COMMAND, 'L', COLOR_ICON_COMMAND, NULL);
    spotlight_register_result("About CamelOS",   "System information",       RESULT_COMMAND, 'A', COLOR_ICON_COMMAND, NULL);
    spotlight_register_result("Empty Trash",     "Delete all trashed items",  RESULT_COMMAND, 'E', COLOR_ICON_COMMAND, NULL);
    spotlight_register_result("Force Quit",      "Close unresponsive apps",  RESULT_COMMAND, 'F', COLOR_ICON_COMMAND, NULL);
    spotlight_register_result("Sleep",           "Put system to sleep",      RESULT_COMMAND, 'S', COLOR_ICON_COMMAND, NULL);
    spotlight_register_result("Restart",         "Restart CamelOS",          RESULT_COMMAND, 'R', COLOR_ICON_COMMAND, NULL);

    // Register common files/settings
    spotlight_register_result("Desktop",         "~/Desktop folder",          RESULT_FILE, 'D', COLOR_ICON_FILE, NULL);
    spotlight_register_result("Documents",       "~/Documents folder",        RESULT_FILE, 'D', COLOR_ICON_FILE, NULL);
    spotlight_register_result("Downloads",       "~/Downloads folder",        RESULT_FILE, 'D', COLOR_ICON_FILE, NULL);
    spotlight_register_result("Network",         "Network configuration",     RESULT_SETTING, 'N', COLOR_ICON_SETTING, NULL);
    spotlight_register_result("Display",         "Display settings",          RESULT_SETTING, 'D', COLOR_ICON_SETTING, NULL);
    spotlight_register_result("Sound",           "Sound settings",            RESULT_SETTING, 'S', COLOR_ICON_SETTING, NULL);
}

void spotlight_register_result(const char* title, const char* subtitle,
                                SpotlightResultType type, char icon_char,
                                uint32_t icon_color, void (*action)(void)) {
    if (registered_count >= MAX_REGISTERED) return;

    SpotlightResult* r = &registered_items[registered_count++];
    strncpy(r->title, title, sizeof(r->title) - 1);
    r->title[sizeof(r->title) - 1] = '\0';
    strncpy(r->subtitle, subtitle, sizeof(r->subtitle) - 1);
    r->subtitle[sizeof(r->subtitle) - 1] = '\0';
    r->type = type;
    r->icon_char = icon_char;
    r->icon_color = icon_color;
    r->action = action;
}

// ============================================================================
// Show/Hide
// ============================================================================

void spotlight_toggle(void) {
    if (g_spotlight.active) {
        spotlight_hide();
    } else {
        spotlight_show();
    }
}

void spotlight_show(void) {
    g_spotlight.active = 1;
    g_spotlight.query[0] = '\0';
    g_spotlight.query_len = 0;
    g_spotlight.cursor_pos = 0;
    g_spotlight.selected_idx = 0;
    g_spotlight.scroll_offset = 0;
    g_spotlight.result_count = 0;
    spotlight_search();
}

void spotlight_hide(void) {
    g_spotlight.active = 0;
}

// ============================================================================
// Search
// ============================================================================

static int str_match_ci(const char* haystack, const char* needle) {
    if (!needle[0]) return 1;

    int hlen = strlen(haystack);
    int nlen = strlen(needle);

    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char hc = haystack[i + j];
            char nc = needle[j];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

void spotlight_search(void) {
    g_spotlight.result_count = 0;

    for (int i = 0; i < registered_count && g_spotlight.result_count < 16; i++) {
        SpotlightResult* src = &registered_items[i];
        if (str_match_ci(src->title, g_spotlight.query) ||
            str_match_ci(src->subtitle, g_spotlight.query)) {
            SpotlightResult* dst = &g_spotlight.results[g_spotlight.result_count++];
            *dst = *src;
        }
    }

    if (g_spotlight.selected_idx >= g_spotlight.result_count) {
        g_spotlight.selected_idx = g_spotlight.result_count > 0 ? 0 : -1;
    }
}

// ============================================================================
// Input Handling
// ============================================================================

int spotlight_handle_key(int key) {
    if (!g_spotlight.active) return 0;

    if (key == KEY_ESC) {
        spotlight_hide();
        return 1;
    }

    if (key == KEY_ENTER) {
        spotlight_execute_selected();
        return 1;
    }

    if (key == KEY_UP) {
        if (g_spotlight.selected_idx > 0) {
            g_spotlight.selected_idx--;
            if (g_spotlight.selected_idx < g_spotlight.scroll_offset) {
                g_spotlight.scroll_offset = g_spotlight.selected_idx;
            }
        }
        return 1;
    }
    if (key == KEY_DOWN) {
        if (g_spotlight.selected_idx < g_spotlight.result_count - 1) {
            g_spotlight.selected_idx++;
            if (g_spotlight.selected_idx >= g_spotlight.scroll_offset + SPOTLIGHT_MAX_VISIBLE) {
                g_spotlight.scroll_offset = g_spotlight.selected_idx - SPOTLIGHT_MAX_VISIBLE + 1;
            }
        }
        return 1;
    }

    if (key == KEY_BACKSPACE) {
        if (g_spotlight.query_len > 0 && g_spotlight.cursor_pos > 0) {
            for (int i = g_spotlight.cursor_pos - 1; i < g_spotlight.query_len; i++) {
                g_spotlight.query[i] = g_spotlight.query[i + 1];
            }
            g_spotlight.query_len--;
            g_spotlight.cursor_pos--;
            g_spotlight.query[g_spotlight.query_len] = '\0';
            spotlight_search();
        }
        return 1;
    }

    if (key >= 32 && key < 127 && g_spotlight.query_len < sizeof(g_spotlight.query) - 1) {
        for (int i = g_spotlight.query_len; i > g_spotlight.cursor_pos; i--) {
            g_spotlight.query[i] = g_spotlight.query[i - 1];
        }
        g_spotlight.query[g_spotlight.cursor_pos] = (char)key;
        g_spotlight.cursor_pos++;
        g_spotlight.query_len++;
        g_spotlight.query[g_spotlight.query_len] = '\0';
        spotlight_search();
        return 1;
    }

    return 1;
}

int spotlight_handle_mouse(int mx, int my, int click) {
    if (!g_spotlight.active) return 0;

    int cx = (screen_w - SPOTLIGHT_WIDTH) / 2;
    int cy = screen_h / 4;
    int total_h = SPOTLIGHT_INPUT_H;

    if (g_spotlight.result_count > 0) {
        int visible = g_spotlight.result_count > SPOTLIGHT_MAX_VISIBLE ?
                      SPOTLIGHT_MAX_VISIBLE : g_spotlight.result_count;
        total_h += 1 + visible * SPOTLIGHT_RESULT_H;
    }

    if (mx >= cx && mx < cx + SPOTLIGHT_WIDTH && my >= cy && my < cy + total_h) {
        if (click) {
            int result_y = cy + SPOTLIGHT_INPUT_H + 1;
            if (my >= result_y) {
                int idx = (my - result_y) / SPOTLIGHT_RESULT_H + g_spotlight.scroll_offset;
                if (idx >= 0 && idx < g_spotlight.result_count) {
                    g_spotlight.selected_idx = idx;
                    spotlight_execute_selected();
                }
            }
        }
        return 1;
    } else if (click) {
        spotlight_hide();
        return 1;
    }

    return 0;
}

// ============================================================================
// Execute
// ============================================================================

void spotlight_execute_selected(void) {
    if (g_spotlight.selected_idx < 0 || g_spotlight.selected_idx >= g_spotlight.result_count) {
        spotlight_hide();
        return;
    }

    SpotlightResult* r = &g_spotlight.results[g_spotlight.selected_idx];
    spotlight_hide();

    if (r->action) {
        r->action();
    } else {
        s_printf("[SPOTLIGHT] No action for: ");
        s_printf(r->title);
        s_printf("\n");
    }
}

// ============================================================================
// App Launch Helpers
// ============================================================================

static void launch_app(const char* path) {
    s_printf("[SPOTLIGHT] Launching: ");
    s_printf(path);
    s_printf("\n");
    if (sys && sys->cdl_load) {
        sys->cdl_load(path);
    } else if (sys && sys->exec) {
        sys->exec(path);
    }
}

static void launch_terminal(void)   { launch_app("/usr/apps/terminal.cdl"); }
static void launch_files(void)      { launch_app("/usr/apps/files.cdl"); }
static void launch_textedit(void)   { launch_app("/usr/apps/textedit.cdl"); }
static void launch_browser(void)    { launch_app("/usr/apps/browser.cdl"); }
static void launch_calculator(void) { launch_app("/usr/apps/calculator.cdl"); }
static void launch_settings(void)   { launch_app("/usr/apps/settings.cdl"); }
static void launch_console(void)    { launch_app("/usr/apps/console.cdl"); }
static void launch_sysmon(void)     { launch_app("/usr/apps/proc.cdl"); }

// ============================================================================
// Drawing
// ============================================================================

static void draw_text(int x, int y, const char* str, uint32_t color) {
    if (sys && sys->draw_text) {
        sys->draw_text(x, y, str, color);
    }
}

void spotlight_draw(void) {
    if (!g_spotlight.active) return;

    int cx = (screen_w - SPOTLIGHT_WIDTH) / 2;
    int cy = screen_h / 4;

    // Draw semi-transparent backdrop (skip for performance - only darken edges)
    // Using a simple pattern-based approach instead of per-pixel
    for (int y = 0; y < cy; y++) {
        if (y % 4 == 0) {
            for (int x = 0; x < screen_w; x++) {
                if (x % 3 == 0) sys->draw_rect(x, y, 1, 1, COLOR_OVERLAY);
            }
        }
    }

    // Calculate total height
    int result_area_h = 0;
    if (g_spotlight.result_count > 0) {
        int visible = g_spotlight.result_count > SPOTLIGHT_MAX_VISIBLE ?
                      SPOTLIGHT_MAX_VISIBLE : g_spotlight.result_count;
        result_area_h = visible * SPOTLIGHT_RESULT_H;
    }
    int total_h = SPOTLIGHT_INPUT_H + (result_area_h > 0 ? 1 + result_area_h : 0);

    // Draw search input background (rounded rect)
    if (sys && sys->draw_rect_rounded) {
        sys->draw_rect_rounded(cx, cy, SPOTLIGHT_WIDTH, total_h, COLOR_SEARCH_BG, SPOTLIGHT_BORDER_R);
    } else {
        sys->draw_rect(cx, cy, SPOTLIGHT_WIDTH, total_h, COLOR_SEARCH_BG);
    }

    // Draw border
    if (sys && sys->draw_rect_rounded) {
        // Draw a slightly larger border rect behind (hack for border effect)
        sys->draw_rect(cx - 1, cy - 1, SPOTLIGHT_WIDTH + 2, total_h + 2, COLOR_SEARCH_FOCUS);
        if (sys->draw_rect_rounded)
            sys->draw_rect_rounded(cx, cy, SPOTLIGHT_WIDTH, total_h, COLOR_SEARCH_BG, SPOTLIGHT_BORDER_R);
    }

    // Draw magnifying glass icon
    int icon_x = cx + SPOTLIGHT_PADDING;
    int icon_y = cy + SPOTLIGHT_INPUT_H / 2;
    sys->draw_rect(icon_x, icon_y - 6, 12, 12, COLOR_SEARCH_ICON);
    sys->draw_rect(icon_x + 2, icon_y - 4, 8, 8, COLOR_SEARCH_BG);
    sys->draw_rect(icon_x + 10, icon_y + 2, 5, 2, COLOR_SEARCH_ICON);

    // Draw query text or placeholder
    int text_x = cx + SPOTLIGHT_PADDING + 24;
    int text_y = cy + SPOTLIGHT_INPUT_H / 2 - 6;

    if (g_spotlight.query_len > 0) {
        draw_text(text_x, text_y, g_spotlight.query, COLOR_TEXT_PRIMARY);

        // Draw blinking cursor
        int cursor_x = text_x + g_spotlight.cursor_pos * 8;
        uint32_t ticks = sys && sys->get_ticks ? sys->get_ticks() : 0;
        if (ticks % 60 < 30) {
            sys->draw_rect(cursor_x, text_y, 2, 14, COLOR_SEARCH_FOCUS);
        }
    } else {
        draw_text(text_x, text_y, "Spotlight Search", COLOR_TEXT_PLACEHOLDER);
    }

    // Draw separator line
    if (g_spotlight.result_count > 0) {
        sys->draw_rect(cx + 8, cy + SPOTLIGHT_INPUT_H, SPOTLIGHT_WIDTH - 16, 1, COLOR_SEARCH_BORDER);
    }

    // Draw results
    int result_y_start = cy + SPOTLIGHT_INPUT_H + 1;
    int visible_count = g_spotlight.result_count > SPOTLIGHT_MAX_VISIBLE ?
                        SPOTLIGHT_MAX_VISIBLE : g_spotlight.result_count;

    for (int i = 0; i < visible_count; i++) {
        int idx = i + g_spotlight.scroll_offset;
        if (idx >= g_spotlight.result_count) break;

        SpotlightResult* r = &g_spotlight.results[idx];
        int ry = result_y_start + i * SPOTLIGHT_RESULT_H;
        int is_selected = (idx == g_spotlight.selected_idx);

        // Draw selected background
        if (is_selected) {
            if (sys && sys->draw_rect_rounded) {
                sys->draw_rect_rounded(cx + 4, ry + 2, SPOTLIGHT_WIDTH - 8,
                                       SPOTLIGHT_RESULT_H - 4, COLOR_SELECTED_BG, 6);
            } else {
                sys->draw_rect(cx + 4, ry + 2, SPOTLIGHT_WIDTH - 8,
                              SPOTLIGHT_RESULT_H - 4, COLOR_SELECTED_BG);
            }
        }

        // Draw icon
        int icon_cx = cx + SPOTLIGHT_PADDING + 10;
        int icon_cy = ry + SPOTLIGHT_RESULT_H / 2;
        uint32_t icon_bg = is_selected ? 0x40FFFFFF : r->icon_color;
        sys->draw_rect(icon_cx - 8, icon_cy - 8, 16, 16, icon_bg);

        // Draw title
        int title_x = cx + SPOTLIGHT_PADDING + 28;
        uint32_t title_color = is_selected ? COLOR_SELECTED_TEXT : COLOR_TEXT_PRIMARY;
        draw_text(title_x, icon_cy - 8, r->title, title_color);

        // Draw subtitle
        uint32_t sub_color = is_selected ? 0x80FFFFFF : COLOR_TEXT_SECONDARY;
        draw_text(title_x, icon_cy + 2, r->subtitle, sub_color);

        // Draw type badge
        int badge_x = cx + SPOTLIGHT_WIDTH - SPOTLIGHT_PADDING - 40;
        const char* type_str = "";
        switch (r->type) {
            case RESULT_APP:     type_str = "App"; break;
            case RESULT_FILE:    type_str = "File"; break;
            case RESULT_COMMAND: type_str = "Cmd"; break;
            case RESULT_SETTING: type_str = "Pref"; break;
            case RESULT_CONTACT: type_str = "Contact"; break;
        }
        draw_text(badge_x, icon_cy - 4, type_str, is_selected ? 0x80FFFFFF : COLOR_TEXT_SECONDARY);
    }

    // Draw keyboard shortcut hints
    int hint_y = cy + total_h + 8;
    draw_text(cx, hint_y, "ESC close | Enter open | Up/Down navigate", COLOR_TEXT_SECONDARY);
}
