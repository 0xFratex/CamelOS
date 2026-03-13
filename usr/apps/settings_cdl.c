// usr/apps/settings_cdl.c - System Settings/Configuration Application
#include "../../sys/cdl_defs.h"
#include "../../include/types.h"

static kernel_api_t* sys = 0;

// ============================================================================
// CONFIGURATION STATE
// ============================================================================
#define MAX_SETTINGS 32
#define CONFIG_PATH "/etc/system.conf"

typedef struct {
    char key[32];
    char value[64];
    char display_name[48];
    char category[24];
    int type;  // 0=string, 1=int, 2=bool, 3=enum
    char enum_options[128];  // For enum types, comma-separated
} Setting;

static Setting settings[MAX_SETTINGS];
static int setting_count = 0;
static int selected_setting = 0;
static int scroll_offset = 0;
static int editing_value = 0;
static char edit_buffer[64];
static int edit_cursor = 0;

// Categories
static const char* categories[] = {
    "General", "Display", "Network", "Security", "About"
};
static int current_category = 0;
static int category_count = 5;

// Theme colors
static uint32_t theme_colors[] = {
    0xFF007AFF,  // Aqua (Blue)
    0xFF8E8E93,  // Graphite
    0xFFFF9500,  // Sunset
    0xFF00C7BE,  // Ocean
    0xFF34C759   // Forest
};
static const char* theme_names[] = {
    "Aqua", "Graphite", "Sunset", "Ocean", "Forest"
};
static int current_theme = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

static void load_defaults(void) {
    // General Settings
    sys->strcpy(settings[setting_count].key, "username");
    sys->strcpy(settings[setting_count].value, "User");
    sys->strcpy(settings[setting_count].display_name, "User Name");
    sys->strcpy(settings[setting_count].category, "General");
    settings[setting_count].type = 0;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "computer_name");
    sys->strcpy(settings[setting_count].value, "CamelOS");
    sys->strcpy(settings[setting_count].display_name, "Computer Name");
    sys->strcpy(settings[setting_count].category, "General");
    settings[setting_count].type = 0;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "theme");
    sys->strcpy(settings[setting_count].value, "0");
    sys->strcpy(settings[setting_count].display_name, "Theme");
    sys->strcpy(settings[setting_count].category, "Display");
    settings[setting_count].type = 3;
    sys->strcpy(settings[setting_count].enum_options, "Aqua,Graphite,Sunset,Ocean,Forest");
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "timezone");
    sys->strcpy(settings[setting_count].value, "UTC");
    sys->strcpy(settings[setting_count].display_name, "Time Zone");
    sys->strcpy(settings[setting_count].category, "General");
    settings[setting_count].type = 3;
    sys->strcpy(settings[setting_count].enum_options, 
           "UTC,GMT,EST,PST,CST,MST,CET,EET,JST,IST,AEST,NZST");
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "auto_lock");
    sys->strcpy(settings[setting_count].value, "1");
    sys->strcpy(settings[setting_count].display_name, "Auto Lock Screen");
    sys->strcpy(settings[setting_count].category, "Security");
    settings[setting_count].type = 2;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "lock_timeout");
    sys->strcpy(settings[setting_count].value, "10");
    sys->strcpy(settings[setting_count].display_name, "Lock Timeout (minutes)");
    sys->strcpy(settings[setting_count].category, "Security");
    settings[setting_count].type = 1;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "dock_size");
    sys->strcpy(settings[setting_count].value, "54");
    sys->strcpy(settings[setting_count].display_name, "Dock Size");
    sys->strcpy(settings[setting_count].category, "Display");
    settings[setting_count].type = 1;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "show_clock");
    sys->strcpy(settings[setting_count].value, "1");
    sys->strcpy(settings[setting_count].display_name, "Show Clock in Menu Bar");
    sys->strcpy(settings[setting_count].category, "Display");
    settings[setting_count].type = 2;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "enable_wifi");
    sys->strcpy(settings[setting_count].value, "1");
    sys->strcpy(settings[setting_count].display_name, "Enable WiFi");
    sys->strcpy(settings[setting_count].category, "Network");
    settings[setting_count].type = 2;
    setting_count++;
    
    sys->strcpy(settings[setting_count].key, "enable_ethernet");
    sys->strcpy(settings[setting_count].value, "1");
    sys->strcpy(settings[setting_count].display_name, "Enable Ethernet");
    sys->strcpy(settings[setting_count].category, "Network");
    settings[setting_count].type = 2;
    setting_count++;
}

static void load_config(void) {
    char buffer[512];
    int result = sys->fs_read(CONFIG_PATH, buffer, sizeof(buffer) - 1);
    
    if (result > 0) {
        buffer[result] = 0;
        
        char* line = buffer;
        while (line && *line) {
            char* next = line;
            while (*next && *next != '\n') next++;
            if (*next) *next++ = 0;
            
            // Skip comments
            if (*line == '#') {
                line = next;
                continue;
            }
            
            // Parse key=value
            char* eq = line;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                char* key = line;
                char* value = eq + 1;
                
                // Find matching setting
                for (int i = 0; i < setting_count; i++) {
                    if (sys->strcmp(settings[i].key, key) == 0) {
                        sys->strcpy(settings[i].value, value);
                        break;
                    }
                }
            }
            
            line = next;
        }
    }
}

static void save_config(void) {
    char buffer[1024];
    int pos = 0;
    
    pos += sys->sprintf(buffer + pos, "# Camel OS System Configuration\n");
    pos += sys->sprintf(buffer + pos, "# Generated by System Settings\n\n");
    
    for (int i = 0; i < setting_count; i++) {
        pos += sys->sprintf(buffer + pos, "%s=%s\n", 
                           settings[i].key, settings[i].value);
    }
    
    sys->fs_create(CONFIG_PATH, 1);
    sys->fs_write(CONFIG_PATH, buffer, pos);
}

// ============================================================================
// DRAWING HELPERS
// ============================================================================

static void draw_rounded_rect(int x, int y, int w, int h, uint32_t color, int r) {
    sys->draw_rect_rounded(x, y, w, h, color, r);
}

static void draw_setting_row(int x, int y, int w, Setting* s, int selected) {
    int h = 44;
    
    // Background
    uint32_t bg = selected ? 0xFFE8F4FD : 0xFFFFFFFF;
    draw_rounded_rect(x, y, w, h, bg, 8);
    
    if (selected) {
        sys->draw_rect(x, y, 3, h, 0xFF007AFF);
    }
    
    // Setting name
    sys->draw_text(x + 16, y + 10, s->display_name, 0xFF1D1D1F);
    
    // Setting value
    int val_x = x + w - 16;
    
    switch (s->type) {
        case 0:  // String
        case 1:  // Int
            {
                int len = sys->strlen(s->value) * 8;
                val_x -= len;
                sys->draw_text(val_x, y + 10, s->value, 0xFF8E8E93);
            }
            break;
            
        case 2:  // Bool
            {
                int is_on = (s->value[0] == '1');
                int sw_w = 40, sw_h = 22;
                int sw_x = val_x - sw_w;
                int sw_y = y + (h - sw_h) / 2;
                
                // Switch background
                uint32_t sw_bg = is_on ? 0xFF34C759 : 0xFFE5E5EA;
                draw_rounded_rect(sw_x, sw_y, sw_w, sw_h, sw_bg, 11);
                
                // Switch knob
                int knob_x = is_on ? sw_x + sw_w - 18 : sw_x + 2;
                draw_rounded_rect(knob_x, sw_y + 2, 18, 18, 0xFFFFFFFF, 9);
            }
            break;
            
        case 3:  // Enum
            {
                // Find display name for enum value
                int val = 0;
                for (int i = 0; s->value[i]; i++) {
                    val = val * 10 + (s->value[i] - '0');
                }
                
                // Parse enum options
                char opts[128];
                sys->strcpy(opts, s->enum_options);
                char* opt = opts;
                int idx = 0;
                char* opt_name = opts;
                
                while (*opt && idx <= val) {
                    if (*opt == ',') {
                        if (idx == val) {
                            *opt = 0;
                            break;
                        }
                        idx++;
                        opt_name = opt + 1;
                    }
                    opt++;
                }
                
                int len = sys->strlen(opt_name) * 8;
                val_x -= len + 12;
                sys->draw_text(val_x, y + 10, opt_name, 0xFF8E8E93);
                
                // Arrow
                sys->draw_text(val_x + len + 4, y + 10, ">", 0xFFC7C7CC);
            }
            break;
    }
}

// ============================================================================
// ABOUT PANEL
// ============================================================================

static void draw_about_panel(int x, int y, int w, int h) {
    // Background
    draw_rounded_rect(x, y, w, h, 0xFFFFFFFF, 12);
    sys->draw_rect(x, y, w, h, 0xFFE5E5EA);
    
    // Logo area
    int logo_y = y + 30;
    draw_rounded_rect(x + w/2 - 40, logo_y, 80, 80, 0xFF007AFF, 16);
    sys->draw_text(x + w/2 - 30, logo_y + 30, "🐪", 0xFFFFFFFF);
    
    // Title
    sys->draw_text(x + w/2 - 60, logo_y + 100, "Camel OS", 0xFF1D1D1F);
    
    // Version
    sys->draw_text(x + w/2 - 40, logo_y + 130, "Version 1.0", 0xFF8E8E93);
    
    // Info
    int info_y = logo_y + 170;
    sys->draw_text(x + 20, info_y, "A nostalgic yet modern operating system", 0xFF3C3C43);
    sys->draw_text(x + 20, info_y + 20, "inspired by classic macOS X.", 0xFF3C3C43);
    
    sys->draw_text(x + 20, info_y + 60, "Built with passion through vibe coding.", 0xFF8E8E93);
    sys->draw_text(x + 20, info_y + 80, "Development time: 2 months", 0xFF8E8E93);
    
    // System info
    info_y += 120;
    draw_rounded_rect(x + 10, info_y, w - 20, 100, 0xFFF5F5F7, 8);
    
    sys->draw_text(x + 20, info_y + 10, "System Information", 0xFF8E8E93);
    
    char buf[64];
    sys->draw_text(x + 20, info_y + 35, "Architecture: ", 0xFF3C3C43);
    sys->draw_text(x + 120, info_y + 35, "x86 (32-bit)", 0xFF8E8E93);
    
    sys->draw_text(x + 20, info_y + 55, "Memory: ", 0xFF3C3C43);
    sys->sprintf(buf, "%d MB", 256);  // Would read from system
    sys->draw_text(x + 120, info_y + 55, buf, 0xFF8E8E93);
    
    sys->draw_text(x + 20, info_y + 75, "Display: ", 0xFF3C3C43);
    sys->draw_text(x + 120, info_y + 75, "1024 x 768", 0xFF8E8E93);
}

// ============================================================================
// MAIN PAINT
// ============================================================================

static void paint(int x, int y, int w, int h) {
    // Window background
    sys->draw_rect(x, y, w, h, 0xFFF5F5F7);
    
    // Sidebar
    int sidebar_w = 180;
    sys->draw_rect(x, y, sidebar_w, h, 0xFFE8E8ED);
    sys->draw_rect(x + sidebar_w, y, 1, h, 0xFFD1D1D6);
    
    // Sidebar header
    sys->draw_text(x + 16, y + 12, "Settings", 0xFF1D1D1F);
    sys->draw_rect(x, y + 36, sidebar_w, 1, 0xFFD1D1D6);
    
    // Categories
    int cat_y = y + 50;
    for (int i = 0; i < category_count; i++) {
        int is_selected = (current_category == i);
        
        if (is_selected) {
            draw_rounded_rect(x + 8, cat_y - 2, sidebar_w - 16, 28, 0xFF007AFF, 6);
            sys->draw_text(x + 20, cat_y + 4, categories[i], 0xFFFFFFFF);
        } else {
            sys->draw_text(x + 20, cat_y + 4, categories[i], 0xFF1D1D1F);
        }
        
        cat_y += 36;
    }
    
    // Main content area
    int content_x = x + sidebar_w + 20;
    int content_y = y + 20;
    int content_w = w - sidebar_w - 40;
    
    if (current_category == category_count - 1) {
        // About panel
        draw_about_panel(content_x, content_y, content_w - 20, 400);
    } else {
        // Settings for current category
        sys->draw_text(content_x, content_y, categories[current_category], 0xFF1D1D1F);
        
        int set_y = content_y + 50;
        int row_idx = 0;
        
        for (int i = 0; i < setting_count; i++) {
            if (sys->strcmp(settings[i].category, categories[current_category]) == 0) {
                draw_setting_row(content_x, set_y, content_w - 20, 
                               &settings[i], selected_setting == i);
                set_y += 52;
                row_idx++;
            }
        }
    }
    
    // Save button
    int btn_y = y + h - 50;
    int btn_w = 120;
    int btn_h = 36;
    int btn_x = x + w - btn_w - 20;
    
    draw_rounded_rect(btn_x, btn_y, btn_w, btn_h, 0xFF007AFF, 8);
    sys->draw_text(btn_x + 30, btn_y + 10, "Save", 0xFFFFFFFF);
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

static void on_mouse(int mx, int my, int btn) {
    if (btn == 0) return;  // Only handle clicks
    
    int sidebar_w = 180;
    
    // Check category clicks
    if (mx < sidebar_w) {
        int cat_y = 50;
        for (int i = 0; i < category_count; i++) {
            if (my >= cat_y - 2 && my <= cat_y + 26) {
                current_category = i;
                selected_setting = 0;
                return;
            }
            cat_y += 36;
        }
        return;
    }
    
    // Check setting rows
    if (current_category < category_count - 1) {
        int content_x = sidebar_w + 20;
        int content_y = 20 + 50;
        int content_w = 800 - sidebar_w - 40;
        
        int row_idx = 0;
        for (int i = 0; i < setting_count; i++) {
            if (sys->strcmp(settings[i].category, categories[current_category]) == 0) {
                if (my >= content_y + row_idx * 52 && 
                    my <= content_y + row_idx * 52 + 44) {
                    
                    selected_setting = i;
                    
                    // Handle toggle
                    if (settings[i].type == 2) {
                        settings[i].value[0] = (settings[i].value[0] == '1') ? '0' : '1';
                    }
                    
                    return;
                }
                row_idx++;
            }
        }
    }
    
    // Check save button
    int btn_y = 600 - 50;
    int btn_w = 120;
    int btn_h = 36;
    int btn_x = 800 - btn_w - 20;
    
    if (mx >= btn_x && mx <= btn_x + btn_w &&
        my >= btn_y && my <= btn_y + btn_h) {
        save_config();
    }
}

static void on_key(int key) {
    // Navigation
    if (key == 0x11) {  // Up
        if (selected_setting > 0) selected_setting--;
    }
    if (key == 0x12) {  // Down
        if (selected_setting < setting_count - 1) selected_setting++;
    }
    if (key == 0x1C) {  // Enter - toggle bool or cycle enum
        Setting* s = &settings[selected_setting];
        if (s->type == 2) {
            s->value[0] = (s->value[0] == '1') ? '0' : '1';
        } else if (s->type == 3) {
            int val = 0;
            for (int i = 0; s->value[i]; i++) {
                val = val * 10 + (s->value[i] - '0');
            }
            // Count options
            int count = 1;
            for (char* p = s->enum_options; *p; p++) {
                if (*p == ',') count++;
            }
            val = (val + 1) % count;
            char buf[16];
            sys->sprintf(buf, "%d", val);
            sys->strcpy(s->value, buf);
        }
    }
}

// ============================================================================
// ENTRY POINT
// ============================================================================

int cdl_main(kernel_api_t* api) {
    sys = api;
    
    // Initialize sys first before using it
    load_defaults();
    load_config();
    
    // Create window with callbacks - this is how apps work in Camel OS
    win_handle_t win = sys->create_window("Settings", 800, 600, paint, on_key, on_mouse);
    if (!win) {
        sys->print("Failed to create settings window\n");
        return 1;
    }
    
    // Event loop - process window events
    while (1) {
        sys->process_events();
    }
    
    return 0;
}
