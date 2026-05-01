// usr/welcome_setup.c - Camel OS Welcome Setup Implementation
// Enhanced with encrypted password, macOS-like directory structure, and screenlock integration

#include "welcome_setup.h"
#include "screenlock.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../core/sha256.h"
#include "../common/time.h"
#include "../sys/api.h"
#include "../fs/pfs32.h"
#include "../fs/disk.h"

// External API
extern kernel_api_t* sys;
extern int screen_w;
extern int screen_h;

// Global setup state
static WelcomeSetup g_setup;

// Design constants - Warm macOS X style
#define C_BG_TOP          0xFFF5F5F7
#define C_BG_BOTTOM       0xFFE8E8ED
#define C_ACCENT          0xFF007AFF
#define C_ACCENT_HOVER    0xFF0051D5
#define C_TEXT_DARK       0xFF1C1C1E
#define C_TEXT_MUTED      0xFF8E8E93
#define C_TEXT_LIGHT      0xFFFFFFFF
#define C_CARD_BG         0xFFFFFFFF
#define C_INPUT_BG        0xFFF2F2F7
#define C_BORDER          0xFFC6C6C8
#define C_SUCCESS         0xFF34C759
#define C_ERROR           0xFFFF3B30
#define C_LOCK_ICON       0xFF8E8E93

// Timezone data (major timezones)
static TimeZone timezones[] = {
    {"UTC",      "UTC (Coordinated Universal Time)", 0},
    {"GMT",      "GMT (Greenwich Mean Time)", 0},
    {"EST",      "New York (EST)", -300},
    {"PST",      "Los Angeles (PST)", -480},
    {"CST",      "Chicago (CST)", -360},
    {"MST",      "Denver (MST)", -420},
    {"CET",      "Paris/Berlin (CET)", 60},
    {"EET",      "Athens/Helsinki (EET)", 120},
    {"JST",      "Tokyo (JST)", 540},
    {"CST_ASIA", "Beijing/Shanghai (CST)", 480},
    {"IST",      "Mumbai (IST)", 330},
    {"AEST",     "Sydney (AEST)", 600},
    {"NZST",     "Auckland (NZST)", 720},
    {"BRT",      "Sao Paulo (BRT)", -180},
    {"IST_EURO", "Dublin (IST)", 60},
    {"MSK",      "Moscow (MSK)", 180}
};
#define TIMEZONE_COUNT (sizeof(timezones) / sizeof(TimeZone))

// Theme definitions
typedef struct {
    const char* name;
    uint32_t primary;
    uint32_t secondary;
    uint32_t accent;
} ThemeDef;

static ThemeDef themes[THEME_COUNT] = {
    {"Aqua",     0xFF007AFF, 0xFF5AC8FA, 0xFF007AFF},  // Classic blue
    {"Graphite", 0xFF8E8E93, 0xFF636366, 0xFF636366},  // Grey
    {"Sunset",   0xFFFF9500, 0xFFFF6B00, 0xFFFF9500},  // Orange
    {"Ocean",    0xFF00C7BE, 0xFF30D5C8, 0xFF00C7BE},  // Teal
    {"Forest",   0xFF34C759, 0xFF30B350, 0xFF34C759}   // Green
};

// --- Initialization ---

void welcome_setup_init(void) {
    memset(&g_setup, 0, sizeof(g_setup));
    g_setup.state = SETUP_STATE_WELCOME;
    g_setup.current_step = 0;
    g_setup.total_steps = 5;  // Now 5 steps: Welcome, User, Password, Timezone, Theme
    g_setup.selected_tz_idx = 0;
    g_setup.selected_theme_idx = 0;
    g_setup.input_buffer[0] = 0;
    g_setup.input_cursor = 0;
    g_setup.anim_progress = 0.0f;
    g_setup.password_buffer[0] = 0;
    g_setup.password_confirm[0] = 0;
    g_setup.password_cursor = 0;
    g_setup.confirm_cursor = 0;
    g_setup.password_active = 1;
    g_setup.password_match_error = 0;
    g_setup.password_step = 0;
    
    welcome_setup_set_defaults();
    welcome_setup_load_config();
}

void welcome_setup_set_defaults(void) {
    g_setup.config.username[0] = 0;  // Empty - show placeholder "Enter name..."
    strcpy(g_setup.config.computer_name, "CamelOS");
    g_setup.config.password_hash[0] = 0;
    memcpy(&g_setup.config.timezone, &timezones[0], sizeof(TimeZone));
    g_setup.config.theme = THEME_AQUA;
    g_setup.config.is_configured = 0;
    g_setup.config.auto_lock = 1;
    g_setup.config.lock_timeout = 10;
    g_setup.config.config_version = 2;  // Bumped for encrypted password support
}

// --- Persistence ---

void welcome_setup_load_config(void) {
    // Try to load config from /Library/Preferences/system.conf (macOS-like path)
    // Fall back to /etc/system.conf for backward compatibility
    char buffer[1024];
    int result = sys_fs_read("/Library/Preferences/system.conf", buffer, sizeof(buffer) - 1);
    
    if (result <= 0) {
        // Try legacy path
        result = sys_fs_read("/etc/system.conf", buffer, sizeof(buffer) - 1);
    }
    
    if (result > 0) {
        buffer[result] = 0;
        
        // Parse config format
        char* line = buffer;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            
            // Trim trailing CR if present (in case of CRLF line endings)
            int llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == ' ')) {
                line[--llen] = 0;
            }
            
            // Skip comments and empty lines
            if (line[0] == '#' || line[0] == 0) { line = next; continue; }
            
            if (strncmp(line, "username=", 9) == 0) {
                strncpy(g_setup.config.username, line + 9, SETUP_USERNAME_MAX - 1);
                g_setup.config.username[SETUP_USERNAME_MAX - 1] = 0;
            } else if (strncmp(line, "password_hash=", 14) == 0) {
                strncpy(g_setup.config.password_hash, line + 14, 64);
                g_setup.config.password_hash[64] = 0;
            } else if (strncmp(line, "timezone=", 9) == 0) {
                for (int i = 0; i < (int)TIMEZONE_COUNT; i++) {
                    if (strcmp(line + 9, timezones[i].name) == 0) {
                        memcpy(&g_setup.config.timezone, &timezones[i], sizeof(TimeZone));
                        g_setup.selected_tz_idx = i;
                        break;
                    }
                }
            } else if (strncmp(line, "theme=", 6) == 0) {
                int theme_idx = line[6] - '0';
                if (theme_idx >= 0 && theme_idx < THEME_COUNT) {
                    g_setup.config.theme = theme_idx;
                    g_setup.selected_theme_idx = theme_idx;
                }
            } else if (strncmp(line, "configured=", 11) == 0) {
                g_setup.config.is_configured = (line[11] == '1');
            } else if (strncmp(line, "auto_lock=", 10) == 0) {
                g_setup.config.auto_lock = (line[10] == '1');
            } else if (strncmp(line, "lock_timeout=", 13) == 0) {
                g_setup.config.lock_timeout = 0;
                const char* p = line + 13;
                while (*p >= '0' && *p <= '9') {
                    g_setup.config.lock_timeout = g_setup.config.lock_timeout * 10 + (*p - '0');
                    p++;
                }
            }
            
            line = next;
        }
    }
}

void welcome_setup_save_config(void) {
    char buffer[1024];
    int pos = 0;
    
    pos += sprintf(buffer + pos, "# CamelOS System Configuration\n");
    pos += sprintf(buffer + pos, "# Version 2 - Encrypted password storage\n");
    pos += sprintf(buffer + pos, "username=%s\n", g_setup.config.username);
    pos += sprintf(buffer + pos, "computer=%s\n", g_setup.config.computer_name);
    pos += sprintf(buffer + pos, "password_hash=%s\n", g_setup.config.password_hash);
    pos += sprintf(buffer + pos, "timezone=%s\n", g_setup.config.timezone.name);
    pos += sprintf(buffer + pos, "theme=%d\n", g_setup.config.theme);
    pos += sprintf(buffer + pos, "auto_lock=%d\n", g_setup.config.auto_lock);
    pos += sprintf(buffer + pos, "lock_timeout=%d\n", g_setup.config.lock_timeout);
    pos += sprintf(buffer + pos, "configured=1\n");
    
    // Create macOS-like directory structure
    sys_fs_create("/Library", 1);
    sys_fs_create("/Library/Preferences", 1);
    
    // Delete old config file if it exists, then create fresh and write
    sys_fs_delete("/Library/Preferences/system.conf");
    sys_fs_create("/Library/Preferences/system.conf", 0);
    int wres = sys_fs_write("/Library/Preferences/system.conf", buffer, pos);
    
    // Verify the write succeeded by reading back
    if (wres > 0) {
        char verify[64];
        int vres = sys_fs_read("/Library/Preferences/system.conf", verify, sizeof(verify) - 1);
        if (vres <= 0 || strncmp(verify, "# CamelOS", 9) != 0) {
            // Primary write failed verification - try legacy path only
            wres = -1;
        }
    }
    
    // Also write to legacy path for backward compatibility during transition
    sys_fs_create("/etc", 1);
    sys_fs_delete("/etc/system.conf");
    sys_fs_create("/etc/system.conf", 0);
    sys_fs_write("/etc/system.conf", buffer, pos);
    
    // Create macOS-like user directory structure
    char home_path[128];
    
    // /Users/<name> (macOS-like, replaces /home/<name>)
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    sys_fs_create(home_path, 1);
    
    // Desktop
    strcat(home_path, "/Desktop");
    sys_fs_create(home_path, 1);
    
    // Documents
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Documents");
    sys_fs_create(home_path, 1);
    
    // Downloads
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Downloads");
    sys_fs_create(home_path, 1);
    
    // Pictures
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Pictures");
    sys_fs_create(home_path, 1);
    
    // Music
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Music");
    sys_fs_create(home_path, 1);
    
    // Movies
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Movies");
    sys_fs_create(home_path, 1);
    
    // Public
    strcpy(home_path, "/Users/");
    strcat(home_path, g_setup.config.username);
    strcat(home_path, "/Public");
    sys_fs_create(home_path, 1);
    
    // Also create legacy /home/desktop symlink for compatibility
    sys_fs_create("/home/desktop", 1);
    
    // Create macOS system directories
    sys_fs_create("/Applications", 1);
    sys_fs_create("/System", 1);
    sys_fs_create("/System/Library", 1);
    sys_fs_create("/System/Library/Frameworks", 1);
    sys_fs_create("/System/Library/CoreServices", 1);
    sys_fs_create("/Library/Application Support", 1);
    sys_fs_create("/Library/Fonts", 1);
    sys_fs_create("/private", 1);
    sys_fs_create("/private/var", 1);
    sys_fs_create("/private/tmp", 1);
    sys_fs_create("/usr", 1);
    sys_fs_create("/usr/apps", 1);  // Legacy compatibility
    sys_fs_create("/usr/lib", 1);   // Legacy compatibility
    sys_fs_create("/bin", 1);
    sys_fs_create("/sbin", 1);
    sys_fs_create("/dev", 1);
    sys_fs_create("/Volumes", 1);
    
    // CRITICAL: Flush all changes to disk so config persists across reboots
    // Double-flush to ensure data reaches persistent storage
    pfs32_sync();
    disk_flush_cache();
    pfs32_sync();
    disk_flush_cache();
}

// --- State Management ---

int welcome_setup_needs_setup(void) {
    return !g_setup.config.is_configured;
}

void welcome_setup_start(void) {
    if (!g_setup.config.is_configured) {
        g_setup.state = SETUP_STATE_WELCOME;
        g_setup.current_step = 0;
        g_setup.anim_progress = 0.0f;
    }
}

int welcome_setup_is_active(void) {
    return g_setup.state != SETUP_STATE_COMPLETE || !g_setup.config.is_configured;
}

void welcome_setup_finish(void) {
    g_setup.config.is_configured = 1;
    
    // Hash the password if one was set
    if (g_setup.password_buffer[0] != 0) {
        sha256_hash_password(g_setup.password_buffer, g_setup.config.password_hash);
    } else {
        // No password set - store empty hash
        g_setup.config.password_hash[0] = 0;
    }
    
    welcome_setup_save_config();
    
    // Configure screenlock with user and hashed password
    screenlock_create_user(g_setup.config.username, g_setup.config.password_hash, g_setup.config.theme);
    
    // Configure screenlock timeout
    screenlock_set_inactivity_timeout(g_setup.config.lock_timeout);
    screenlock_set_inactivity_enabled(g_setup.config.auto_lock);
    
    g_setup.state = SETUP_STATE_COMPLETE;
}

SystemConfig* welcome_setup_get_config(void) {
    return &g_setup.config;
}

// --- Rendering Helpers ---

static void draw_gradient_bg(int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t blend = (y * 255) / h;
        uint8_t r1 = (C_BG_TOP >> 16) & 0xFF;
        uint8_t g1 = (C_BG_TOP >> 8) & 0xFF;
        uint8_t b1 = C_BG_TOP & 0xFF;
        uint8_t r2 = (C_BG_BOTTOM >> 16) & 0xFF;
        uint8_t g2 = (C_BG_BOTTOM >> 8) & 0xFF;
        uint8_t b2 = C_BG_BOTTOM & 0xFF;
        
        uint8_t r = r1 + ((r2 - r1) * blend) / 255;
        uint8_t g = g1 + ((g2 - g1) * blend) / 255;
        uint8_t b = b1 + ((b2 - b1) * blend) / 255;
        
        gfx_fill_rect(0, y, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
}

static void draw_progress_dots(int cx, int y, int current, int total) {
    int dot_size = 8;
    int spacing = 16;
    int total_w = total * spacing;
    int start_x = cx - total_w / 2;
    
    for (int i = 0; i < total; i++) {
        int dot_x = start_x + i * spacing + spacing / 2;
        uint32_t color = (i == current) ? C_ACCENT : C_BORDER;
        gfx_fill_rounded_rect(dot_x - dot_size/2, y - dot_size/2, 
                              dot_size, dot_size, color, dot_size/2);
    }
}

static void draw_card(int x, int y, int w, int h) {
    // Shadow
    gfx_fill_rounded_rect(x + 4, y + 6, w, h, 0x20000000, 16);
    // Background
    gfx_fill_rounded_rect(x, y, w, h, C_CARD_BG, 16);
    // Border
    gfx_draw_rect(x, y, w, h, C_BORDER);
}

static int draw_button(int x, int y, int w, int h, const char* label, int primary, int mx, int my, int click) {
    int hover = (mx >= x && mx <= x + w && my >= y && my <= y + h);
    
    uint32_t bg = primary ? (hover ? C_ACCENT_HOVER : C_ACCENT) : 
                            (hover ? C_INPUT_BG : C_CARD_BG);
    uint32_t text = primary ? C_TEXT_LIGHT : C_TEXT_DARK;
    
    // Shadow for primary
    if (primary) {
        gfx_fill_rounded_rect(x + 2, y + 3, w, h, 0x20000000, 10);
    }
    
    gfx_fill_rounded_rect(x, y, w, h, bg, 10);
    if (!primary) {
        gfx_draw_rect(x, y, w, h, C_BORDER);
    }
    
    int text_x = x + (w - strlen(label) * 8) / 2;
    int text_y = y + (h - 16) / 2;
    gfx_draw_string(text_x, text_y, label, text);
    
    return hover && click;
}

static void draw_text_field(int x, int y, int w, int h, const char* value, int active, int is_password, int mx, int my, int click, int cursor_pos) {
    int hover = (mx >= x && mx <= x + w && my >= y && my <= y + h);
    
    uint32_t bg = active ? C_TEXT_LIGHT : (hover ? C_INPUT_BG : C_CARD_BG);
    uint32_t border = active ? C_ACCENT : (hover ? C_TEXT_MUTED : C_BORDER);
    
    gfx_fill_rounded_rect(x, y, w, h, bg, 8);
    gfx_draw_rect(x, y, w, h, border);
    
    // Calculate content dimensions for centering
    int text_y = y + (h - 16) / 2;
    
    if (is_password) {
        // Use cursor_pos for accurate dot count (not strlen of hash)
        int len = cursor_pos;
        int content_w = len * 14;
        int text_x = x + (w - content_w) / 2;  // Center dots in field
        
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                gfx_fill_rounded_rect(text_x + i * 14 + 4, text_y + 4, 8, 8, C_TEXT_DARK, 4);
            }
        } else {
            // Placeholder in grey, centered
            const char* ph = "Enter password...";
            int ph_w = strlen(ph) * 8;
            gfx_draw_string(x + (w - ph_w) / 2, text_y, ph, C_TEXT_MUTED);
        }
    } else {
        if (value && value[0]) {
            int content_w = strlen(value) * 8;
            int text_x = x + (w - content_w) / 2;  // Center text in field
            gfx_draw_string(text_x, text_y, value, C_TEXT_DARK);
        } else {
            // Placeholder in grey, centered
            const char* ph = "Enter name...";
            int ph_w = strlen(ph) * 8;
            gfx_draw_string(x + (w - ph_w) / 2, text_y, ph, C_TEXT_MUTED);
        }
    }
    
    // Cursor
    if (active) {
        static int blink = 0;
        blink++;
        if ((blink / 30) % 2 == 0) {
            int cursor_x;
            if (is_password) {
                int len = cursor_pos;
                if (len > 0) {
                    int content_w = len * 14;
                    int text_x = x + (w - content_w) / 2;
                    cursor_x = text_x + len * 14 + 4;
                } else {
                    // Empty password: cursor in center
                    cursor_x = x + w / 2;
                }
            } else {
                int vlen = value ? strlen(value) : 0;
                if (vlen > 0) {
                    int content_w = vlen * 8;
                    int text_x = x + (w - content_w) / 2;
                    cursor_x = text_x + vlen * 8;
                } else {
                    // Empty field: cursor in center
                    cursor_x = x + w / 2;
                }
            }
            gfx_fill_rect(cursor_x, text_y, 1, 16, C_ACCENT);
        }
    }
}

// --- State Renderers ---

static void render_welcome(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Logo / Title
    int title_y = cy - 100;
    gfx_draw_string_scaled(cx - 100, title_y, "Camel", C_ACCENT, 4);
    gfx_draw_string_scaled(cx - 40, title_y + 60, "OS", C_TEXT_DARK, 4);
    
    // Subtitle
    char* subtitle = "Welcome to your new operating system";
    gfx_draw_string(cx - strlen(subtitle) * 4, title_y + 130, subtitle, C_TEXT_MUTED);
    
    // Feature list
    int feat_y = title_y + 180;
    char* features[] = {
        "Fast and lightweight performance",
        "Beautiful macOS-inspired interface",
        "Encrypted user authentication",
        "Built-in apps and utilities",
        "Secure and private by design"
    };
    
    for (int i = 0; i < 5; i++) {
        gfx_draw_string(cx - 160, feat_y + i * 25, ">", C_ACCENT);
        gfx_draw_string(cx - 145, feat_y + i * 25, features[i], C_TEXT_DARK);
    }
    
    // Continue button
    if (draw_button(cx - 80, cy + 150, 160, 48, "Continue", 1, mx, my, click)) {
        g_setup.state = SETUP_STATE_USER;
        g_setup.current_step = 1;
        g_setup.input_buffer[0] = 0;
        g_setup.input_cursor = 0;
        g_setup.input_active = 1;
        // Don't pre-fill - show placeholder text instead
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 0, g_setup.total_steps);
}

static void render_user_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 480;
    int card_h = 340;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Create Your Account";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 12) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "This will be your user account name";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 6) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Avatar preview
    int avatar_x = cx;
    int avatar_y = card_y + 120;
    uint32_t avatar_color = themes[g_setup.selected_theme_idx].primary;
    gfx_fill_rounded_rect(avatar_x - 40, avatar_y - 40, 80, 80, avatar_color, 40);
    
    // User icon on avatar
    gfx_fill_rounded_rect(avatar_x - 10, avatar_y - 20, 20, 20, C_TEXT_LIGHT, 10);
    gfx_fill_rounded_rect(avatar_x - 20, avatar_y + 5, 40, 30, C_TEXT_LIGHT, 8);
    
    // Username field
    draw_text_field(cx - 150, card_y + 210, 300, 44, 
                   g_setup.input_buffer, g_setup.input_active, 0, mx, my, click,
                   g_setup.input_cursor);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_WELCOME;
        g_setup.current_step = 0;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
        if (g_setup.input_buffer[0]) {
            strcpy(g_setup.config.username, g_setup.input_buffer);
            g_setup.state = SETUP_STATE_PASSWORD;
            g_setup.current_step = 2;
            g_setup.password_buffer[0] = 0;
            g_setup.password_confirm[0] = 0;
            g_setup.password_cursor = 0;
            g_setup.confirm_cursor = 0;
            g_setup.password_active = 1;
            g_setup.password_step = 0;
            g_setup.password_match_error = 0;
        }
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 1, g_setup.total_steps);
}

static void render_password_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 480;
    int card_h = 400;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Lock icon at top
    int icon_y = card_y + 20;
    gfx_fill_rounded_rect(cx - 15, icon_y, 30, 24, C_LOCK_ICON, 4);
    gfx_fill_rounded_rect(cx - 10, icon_y - 10, 20, 16, 0x00000000, 8); // Arch
    gfx_fill_rounded_rect(cx - 8, icon_y + 4, 16, 12, C_TEXT_LIGHT, 3);  // Keyhole
    
    // Title
    if (g_setup.password_step == 0) {
        char* title = "Set Your Password";
        gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 12) / 2, 
                              card_y + 50, title, C_TEXT_DARK, 2);
        char* subtitle = "This protects your account and locks your screen";
        gfx_draw_string(card_x + (card_w - strlen(subtitle) * 6) / 2, 
                       card_y + 85, subtitle, C_TEXT_MUTED);
    } else {
        char* title = "Confirm Password";
        gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 12) / 2, 
                              card_y + 50, title, C_TEXT_DARK, 2);
        char* subtitle = "Enter your password again to confirm";
        gfx_draw_string(card_x + (card_w - strlen(subtitle) * 6) / 2, 
                       card_y + 85, subtitle, C_TEXT_MUTED);
    }
    
    // Password field
    if (g_setup.password_step == 0) {
        draw_text_field(cx - 150, card_y + 130, 300, 44,
                       g_setup.password_buffer, g_setup.password_active == 1, 1, mx, my, click,
                       g_setup.password_cursor);
    } else {
        // Show first password as confirmed dots
        gfx_draw_string(cx - 140, card_y + 120, "Password set", C_SUCCESS);
        
        // Confirm field
        draw_text_field(cx - 150, card_y + 150, 300, 44,
                       g_setup.password_confirm, g_setup.password_active == 2, 1, mx, my, click,
                       g_setup.confirm_cursor);
    }
    
    // Optional hint
    char* hint = "Leave blank to skip password protection";
    gfx_draw_string(cx - strlen(hint) * 3, card_y + 220, hint, C_TEXT_MUTED);
    
    // Error message
    if (g_setup.password_match_error) {
        char* error = "Passwords do not match. Try again.";
        gfx_draw_string(cx - strlen(error) * 3, card_y + 250, error, C_ERROR);
    }
    
    // Security note
    char* security = "Your password is encrypted with SHA-256";
    gfx_draw_string(cx - strlen(security) * 3, card_y + 280, security, C_TEXT_MUTED);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_USER;
        g_setup.current_step = 1;
        g_setup.password_match_error = 0;
    }
    
    if (g_setup.password_step == 0) {
        if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Next", 1, mx, my, click)) {
            if (g_setup.password_buffer[0] == 0) {
                // No password - skip confirmation, go to timezone
                g_setup.state = SETUP_STATE_TIMEZONE;
                g_setup.current_step = 3;
            } else {
                // Move to confirm step
                g_setup.password_step = 1;
                g_setup.password_active = 2;
                g_setup.password_confirm[0] = 0;
                g_setup.confirm_cursor = 0;
                g_setup.password_match_error = 0;
            }
        }
    } else {
        if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
            // Verify passwords match
            if (strcmp(g_setup.password_buffer, g_setup.password_confirm) == 0) {
                g_setup.state = SETUP_STATE_TIMEZONE;
                g_setup.current_step = 3;
                g_setup.password_match_error = 0;
            } else {
                g_setup.password_match_error = 1;
                g_setup.password_confirm[0] = 0;
                g_setup.confirm_cursor = 0;
            }
        }
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 2, g_setup.total_steps);
}

static void render_timezone_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 520;
    int card_h = 400;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2 - 20;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Select Your Time Zone";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 12) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "This helps show the correct time on your system";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 6) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Timezone list (scrollable region)
    int list_x = card_x + 30;
    int list_y = card_y + 100;
    int list_w = card_w - 60;
    int item_h = 32;
    int visible_items = 7;
    
    // List background
    gfx_fill_rounded_rect(list_x, list_y, list_w, visible_items * item_h, C_INPUT_BG, 8);
    
    for (int i = 0; i < (int)TIMEZONE_COUNT && i < visible_items; i++) {
        int item_y = list_y + i * item_h;
        int is_selected = (i == g_setup.selected_tz_idx);
        int hover = (mx >= list_x && mx <= list_x + list_w && 
                    my >= item_y && my < item_y + item_h);
        
        if (is_selected || hover) {
            uint32_t bg = is_selected ? C_ACCENT : (hover ? C_BG_TOP : C_INPUT_BG);
            gfx_fill_rounded_rect(list_x + 4, item_y + 2, list_w - 8, item_h - 4, bg, 6);
        }
        
        uint32_t text_color = is_selected ? C_TEXT_LIGHT : C_TEXT_DARK;
        TimeZone* tz = &timezones[i];
        
        gfx_draw_string(list_x + 16, item_y + 8, tz->display, text_color);
    }
    
    // Scroll indicator
    if (TIMEZONE_COUNT > visible_items) {
        int scroll_h = (visible_items * visible_items * item_h) / TIMEZONE_COUNT;
        int scroll_y = list_y + (g_setup.selected_tz_idx * item_h * visible_items) / TIMEZONE_COUNT;
        gfx_fill_rounded_rect(list_x + list_w - 12, scroll_y, 8, scroll_h, C_TEXT_MUTED, 4);
    }
    
    // Current selection preview
    TimeZone* selected = &timezones[g_setup.selected_tz_idx];
    char preview[64];
    strcpy(preview, "Selected: ");
    strcat(preview, selected->name);
    strcat(preview, " (UTC");
    if (selected->offset_minutes >= 0) strcat(preview, "+");
    char offset_str[8];
    int hours = selected->offset_minutes / 60;
    int_to_str(hours, offset_str);
    strcat(preview, offset_str);
    strcat(preview, ")");
    
    gfx_draw_string(card_x + (card_w - strlen(preview) * 6) / 2, 
                   card_y + card_h - 100, preview, C_TEXT_MUTED);
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_PASSWORD;
        g_setup.current_step = 2;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Continue", 1, mx, my, click)) {
        memcpy(&g_setup.config.timezone, &timezones[g_setup.selected_tz_idx], sizeof(TimeZone));
        g_setup.state = SETUP_STATE_THEME;
        g_setup.current_step = 4;
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 3, g_setup.total_steps);
}

static void render_theme_setup(int cx, int cy, int w, int h, int mx, int my, int click) {
    // Card
    int card_w = 540;
    int card_h = 380;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    draw_card(card_x, card_y, card_w, card_h);
    
    // Title
    char* title = "Choose Your Theme";
    gfx_draw_string_scaled(card_x + (card_w - strlen(title) * 12) / 2, 
                          card_y + 24, title, C_TEXT_DARK, 2);
    
    // Subtitle
    char* subtitle = "Personalize your CamelOS experience";
    gfx_draw_string(card_x + (card_w - strlen(subtitle) * 6) / 2, 
                   card_y + 60, subtitle, C_TEXT_MUTED);
    
    // Theme options
    int theme_start_x = card_x + 40;
    int theme_y = card_y + 110;
    int theme_w = 90;
    int theme_h = 140;
    int theme_spacing = 100;
    
    for (int i = 0; i < THEME_COUNT; i++) {
        int theme_x = theme_start_x + i * theme_spacing;
        int is_selected = (i == g_setup.selected_theme_idx);
        int hover = (mx >= theme_x && mx <= theme_x + theme_w && 
                    my >= theme_y && my <= theme_y + theme_h);
        
        // Theme preview card
        uint32_t border = is_selected ? C_ACCENT : (hover ? C_TEXT_MUTED : C_BORDER);
        
        // Shadow
        if (is_selected) {
            gfx_fill_rounded_rect(theme_x + 2, theme_y + 3, theme_w, theme_h, 0x20000000, 12);
        }
        
        // Background
        gfx_fill_rounded_rect(theme_x, theme_y, theme_w, theme_h, C_CARD_BG, 12);
        gfx_draw_rect(theme_x, theme_y, theme_w, theme_h, border);
        
        // Color preview (mini window)
        int preview_x = theme_x + 10;
        int preview_y = theme_y + 10;
        int preview_w = theme_w - 20;
        int preview_h = 60;
        
        // Title bar
        gfx_fill_rounded_rect(preview_x, preview_y, preview_w, 20, 
                             themes[i].primary, 6);
        // Traffic lights
        gfx_fill_rounded_rect(preview_x + 6, preview_y + 6, 8, 8, 0xFFFF5F57, 4);
        gfx_fill_rounded_rect(preview_x + 18, preview_y + 6, 8, 8, 0xFFFFBD2E, 4);
        gfx_fill_rounded_rect(preview_x + 30, preview_y + 6, 8, 8, 0xFF28C940, 4);
        
        // Content area
        gfx_fill_rect(preview_x, preview_y + 20, preview_w, 40, C_INPUT_BG);
        
        // Sidebar
        gfx_fill_rect(preview_x, preview_y + 20, 20, 40, themes[i].secondary);
        
        // Theme name
        gfx_draw_string(theme_x + (theme_w - strlen(themes[i].name) * 6) / 2, 
                       theme_y + theme_h - 30, themes[i].name, 
                       is_selected ? C_ACCENT : C_TEXT_DARK);
        
        // Selection checkmark
        if (is_selected) {
            gfx_draw_string(theme_x + theme_w - 20, theme_y + 5, "OK", C_ACCENT);
        }
    }
    
    // Buttons
    if (draw_button(card_x + 40, card_y + card_h - 60, 120, 40, "Back", 0, mx, my, click)) {
        g_setup.state = SETUP_STATE_TIMEZONE;
        g_setup.current_step = 3;
    }
    
    if (draw_button(card_x + card_w - 160, card_y + card_h - 60, 120, 40, "Get Started", 1, mx, my, click)) {
        g_setup.config.theme = g_setup.selected_theme_idx;
        welcome_setup_finish();
        g_setup.state = SETUP_STATE_COMPLETE;
    }
    
    // Progress dots
    draw_progress_dots(cx, h - 80, 4, g_setup.total_steps);
}

// --- Main Render ---

void welcome_setup_render(uint32_t* buffer, int w, int h, int mx, int my) {
    (void)buffer;
    
    if (g_setup.state == SETUP_STATE_COMPLETE && g_setup.config.is_configured) {
        return;
    }
    
    int cx = w / 2;
    int cy = h / 2;
    
    // Background
    draw_gradient_bg(w, h);
    
    // Animated entrance
    g_setup.anim_progress += 0.05f;
    if (g_setup.anim_progress > 1.0f) g_setup.anim_progress = 1.0f;
    
    // Render based on state
    switch (g_setup.state) {
        case SETUP_STATE_WELCOME:
            render_welcome(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_USER:
            render_user_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_PASSWORD:
            render_password_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_TIMEZONE:
            render_timezone_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_THEME:
            render_theme_setup(cx, cy, w, h, mx, my, 0);
            break;
        case SETUP_STATE_COMPLETE:
            // Show completion briefly
            gfx_draw_string_scaled(cx - 120, cy - 20, "All Done!", C_SUCCESS, 3);
            break;
    }
}

// --- Input Handling ---

int welcome_setup_handle_key(int key) {
    if (g_setup.state == SETUP_STATE_COMPLETE) {
        return 0;
    }
    
    if (g_setup.state == SETUP_STATE_USER && g_setup.input_active) {
        if (key == 0x08 || key == 0x7F) {
            // Backspace
            if (g_setup.input_cursor > 0) {
                g_setup.input_cursor--;
                g_setup.input_buffer[g_setup.input_cursor] = 0;
            }
        } else if (key == 0x0D || key == '\n') {
            // Enter - accept and continue
            if (g_setup.input_buffer[0]) {
                strcpy(g_setup.config.username, g_setup.input_buffer);
                g_setup.state = SETUP_STATE_PASSWORD;
                g_setup.current_step = 2;
                g_setup.password_active = 1;
                g_setup.password_step = 0;
            }
        } else if (key >= 0x20 && key < 0x7F && g_setup.input_cursor < SETUP_USERNAME_MAX - 1) {
            g_setup.input_buffer[g_setup.input_cursor] = (char)key;
            g_setup.input_cursor++;
            g_setup.input_buffer[g_setup.input_cursor] = 0;
        }
        return 1;
    }
    
    // Password step input
    if (g_setup.state == SETUP_STATE_PASSWORD) {
        if (key == 0x08 || key == 0x7F) {
            // Backspace
            if (g_setup.password_step == 0 && g_setup.password_cursor > 0) {
                g_setup.password_cursor--;
                g_setup.password_buffer[g_setup.password_cursor] = 0;
            } else if (g_setup.password_step == 1 && g_setup.confirm_cursor > 0) {
                g_setup.confirm_cursor--;
                g_setup.password_confirm[g_setup.confirm_cursor] = 0;
            }
            g_setup.password_match_error = 0;
        } else if (key == 0x0D || key == '\n') {
            // Enter
            if (g_setup.password_step == 0) {
                if (g_setup.password_buffer[0] == 0) {
                    // No password - skip to timezone
                    g_setup.state = SETUP_STATE_TIMEZONE;
                    g_setup.current_step = 3;
                } else {
                    // Move to confirm
                    g_setup.password_step = 1;
                    g_setup.password_active = 2;
                    g_setup.password_confirm[0] = 0;
                    g_setup.confirm_cursor = 0;
                }
            } else {
                // Verify passwords match
                if (strcmp(g_setup.password_buffer, g_setup.password_confirm) == 0) {
                    g_setup.state = SETUP_STATE_TIMEZONE;
                    g_setup.current_step = 3;
                    g_setup.password_match_error = 0;
                } else {
                    g_setup.password_match_error = 1;
                    g_setup.password_confirm[0] = 0;
                    g_setup.confirm_cursor = 0;
                }
            }
        } else if (key >= 0x20 && key < 0x7F) {
            if (g_setup.password_step == 0 && g_setup.password_cursor < SETUP_PASSWORD_MAX - 1) {
                g_setup.password_buffer[g_setup.password_cursor] = (char)key;
                g_setup.password_cursor++;
                g_setup.password_buffer[g_setup.password_cursor] = 0;
            } else if (g_setup.password_step == 1 && g_setup.confirm_cursor < SETUP_PASSWORD_MAX - 1) {
                g_setup.password_confirm[g_setup.confirm_cursor] = (char)key;
                g_setup.confirm_cursor++;
                g_setup.password_confirm[g_setup.confirm_cursor] = 0;
            }
            g_setup.password_match_error = 0;
        }
        return 1;
    }
    
    return 0;
}

int welcome_setup_handle_click(int mx, int my, int click) {
    if (g_setup.state == SETUP_STATE_COMPLETE) {
        return 0;
    }
    
    int w = screen_w ? screen_w : 1024;
    int h = screen_h ? screen_h : 768;
    int cx = w / 2;
    int cy = h / 2;
    
    switch (g_setup.state) {
        case SETUP_STATE_WELCOME:
            render_welcome(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_USER:
            render_user_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_PASSWORD:
            render_password_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_TIMEZONE:
            render_timezone_setup(cx, cy, w, h, mx, my, click);
            break;
        case SETUP_STATE_THEME:
            render_theme_setup(cx, cy, w, h, mx, my, click);
            break;
        default:
            break;
    }
    
    return 1;
}

int welcome_setup_handle_mouse(int mx, int my, int click, int pressed) {
    (void)pressed;
    
    // Handle timezone list scrolling
    if (g_setup.state == SETUP_STATE_TIMEZONE && click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_w = 520;
        int card_x = cx - card_w / 2;
        int card_y = cy - 210;
        
        int list_x = card_x + 30;
        int list_y = card_y + 100;
        int list_w = card_w - 60;
        int item_h = 32;
        
        if (mx >= list_x && mx <= list_x + list_w) {
            int rel_y = my - list_y;
            if (rel_y >= 0 && rel_y < 7 * item_h) {
                int idx = rel_y / item_h;
                if (idx >= 0 && idx < (int)TIMEZONE_COUNT) {
                    g_setup.selected_tz_idx = idx;
                }
            }
        }
    }
    
    // Handle theme selection
    if (g_setup.state == SETUP_STATE_THEME && click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        int card_x = cx - 270;
        int card_y = cy - 190;
        int theme_start_x = card_x + 40;
        int theme_y = card_y + 110;
        int theme_w = 90;
        int theme_h = 140;
        int theme_spacing = 100;
        
        for (int i = 0; i < THEME_COUNT; i++) {
            int theme_x = theme_start_x + i * theme_spacing;
            if (mx >= theme_x && mx <= theme_x + theme_w && 
                my >= theme_y && my <= theme_y + theme_h) {
                g_setup.selected_theme_idx = i;
            }
        }
    }
    
    // Handle text field focus on click (click on input box activates it)
    if (click) {
        int w = screen_w ? screen_w : 1024;
        int h = screen_h ? screen_h : 768;
        int cx = w / 2;
        int cy = h / 2;
        
        if (g_setup.state == SETUP_STATE_USER) {
            int card_w = 480;
            int card_x = cx - card_w / 2;
            int card_y = cy - 170;
            int field_x = cx - 150, field_y = card_y + 210;
            if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                g_setup.input_active = 1;
            }
        }
        
        if (g_setup.state == SETUP_STATE_PASSWORD) {
            int card_w = 480;
            int card_x = cx - card_w / 2;
            int card_y = cy - 200;
            if (g_setup.password_step == 0) {
                int field_x = cx - 150, field_y = card_y + 130;
                if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                    g_setup.password_active = 1;
                }
            } else {
                int field_x = cx - 150, field_y = card_y + 150;
                if (mx >= field_x && mx <= field_x + 300 && my >= field_y && my <= field_y + 44) {
                    g_setup.password_active = 2;
                }
            }
        }
    }
    
    // Only process button clicks when there's an actual click event
    if (click) {
        return welcome_setup_handle_click(mx, my, click);
    }
    return 1;
}

void welcome_setup_update(float dt) {
    (void)dt;
    // Animation updates handled in render
}
