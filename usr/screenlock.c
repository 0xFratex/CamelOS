// usr/screenlock.c - Camel OS Screen Lock Implementation
// Enhanced with encrypted password verification (SHA-256) and boot-time lock

#include "screenlock.h"
#include "lib/camel_framework.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "../core/sha256.h"
#include "../common/time.h"
#include "../sys/api.h"
#include "../hal/cpu/timer.h"

extern int screen_w;
extern int screen_h;
extern int mouse_x, mouse_y, mouse_btn_left;

// Global lock state
static ScreenLock g_lock;

// Inactivity tracking
static uint32_t last_activity_time = 0;
static uint32_t inactivity_timeout_seconds = 600; // 10 minutes default
static int inactivity_enabled = 1;

// Design constants - macOS X inspired
#define C_LOCK_BG_TOP      0xFF1A1A2E
#define C_LOCK_BG_BOTTOM   0xFF16213E
#define C_LOCK_ACCENT      0xFF007AFF
#define C_LOCK_TEXT        0xFFFFFFFF
#define C_LOCK_TEXT_DIM    0xFF8E8E93
#define C_LOCK_ERROR       0xFFFF3B30
#define C_LOCK_INPUT_BG    0x40FFFFFF
#define C_LOCK_AVATAR_BG   0xFF4A5568

// Avatar colors palette
static uint32_t avatar_colors[] = {
    0xFF007AFF, 0xFF34C759, 0xFFFF9500, 0xFFFF3B30,
    0xFF5856D6, 0xFFFF2D55, 0xFF00C7BE, 0xFFAF52DE
};

// --- Initialization ---

void screenlock_init(void) {
    memset(&g_lock, 0, sizeof(g_lock));
    g_lock.state = LOCK_STATE_UNLOCKED;
    strcpy(g_lock.user.username, "User");
    g_lock.user.password_hash[0] = 0;
    g_lock.user.has_password = 0;
    g_lock.user.avatar_color = 0;
    g_lock.blur_amount = 0;
    g_lock.anim_progress = 0.0f;
    g_lock.entered_password[0] = 0;
    g_lock.cursor_pos = 0;
    g_lock.show_error = 0;
    g_lock.error_timer = 0;
    last_activity_time = get_tick_count();
}

// --- User Management ---

void screenlock_set_user(const char* username, const char* password_hash) {
    if (username) {
        strncpy(g_lock.user.username, username, LOCK_USER_MAX - 1);
        g_lock.user.username[LOCK_USER_MAX - 1] = 0;
    }
    if (password_hash) {
        strncpy(g_lock.user.password_hash, password_hash, LOCK_PASSWORD_MAX - 1);
        g_lock.user.password_hash[LOCK_PASSWORD_MAX - 1] = 0;
        g_lock.user.has_password = (password_hash[0] != 0) ? 1 : 0;
    }
}

void screenlock_create_user(const char* username, const char* password_hash, int avatar_color) {
    screenlock_set_user(username, password_hash);
    if (avatar_color >= 0 && avatar_color < 8) {
        g_lock.user.avatar_color = avatar_color;
    }
}

LockUserProfile* screenlock_get_user(void) {
    return &g_lock.user;
}

// Load user configuration from system.conf
void screenlock_load_user(void) {
    char buffer[1024];
    int result = sys_fs_read("/Library/Preferences/system.conf", buffer, sizeof(buffer) - 1);
    
    if (result <= 0) {
        // Try legacy path
        result = sys_fs_read("/etc/system.conf", buffer, sizeof(buffer) - 1);
    }
    
    if (result > 0) {
        buffer[result] = 0;
        
        char* line = buffer;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            
            if (strncmp(line, "username=", 9) == 0) {
                strncpy(g_lock.user.username, line + 9, LOCK_USER_MAX - 1);
                g_lock.user.username[LOCK_USER_MAX - 1] = 0;
            } else if (strncmp(line, "password_hash=", 14) == 0) {
                strncpy(g_lock.user.password_hash, line + 14, 64);
                g_lock.user.password_hash[64] = 0;
                g_lock.user.has_password = (g_lock.user.password_hash[0] != 0) ? 1 : 0;
            } else if (strncmp(line, "auto_lock=", 10) == 0) {
                inactivity_enabled = (line[10] == '1') ? 1 : 0;
            } else if (strncmp(line, "lock_timeout=", 13) == 0) {
                int timeout = 0;
                const char* p = line + 13;
                while (*p >= '0' && *p <= '9') {
                    timeout = timeout * 10 + (*p - '0');
                    p++;
                }
                if (timeout > 0) inactivity_timeout_seconds = timeout * 60;
            }
            
            line = next;
        }
    }
}

// --- Inactivity Tracking ---

void screenlock_set_inactivity_timeout(int minutes) {
    inactivity_timeout_seconds = minutes * 60;
}

void screenlock_set_inactivity_enabled(int enabled) {
    inactivity_enabled = enabled;
}

// Call this from main event loop when there's user activity
void screenlock_on_activity(void) {
    last_activity_time = get_tick_count();
}

// Check if we should auto-lock due to inactivity
int screenlock_check_inactivity(void) {
    if (!inactivity_enabled) return 0;
    if (g_lock.state != LOCK_STATE_UNLOCKED) return 0;
    if (!g_lock.user.has_password) return 0;  // Don't auto-lock if no password set
    
    uint32_t now = get_tick_count();
    uint32_t elapsed = now - last_activity_time;
    
    // ticks are typically milliseconds
    if (elapsed >= inactivity_timeout_seconds * 1000) {
        screenlock_lock();
        return 1;
    }
    
    return 0;
}

// --- Lock Control ---

void screenlock_lock(void) {
    if (g_lock.state == LOCK_STATE_UNLOCKED) {
        g_lock.state = LOCK_STATE_LOCKING;
        g_lock.blur_amount = 0;
        g_lock.anim_progress = 0.0f;
        g_lock.entered_password[0] = 0;
        g_lock.cursor_pos = 0;
        g_lock.show_error = 0;
        
        // Get current time for lock display
        int h, m, s;
        sys_get_time(&h, &m, &s);
        g_lock.lock_time = (h * 3600) + (m * 60) + s;
    }
}

void screenlock_unlock(void) {
    if (g_lock.state == LOCK_STATE_LOCKED || g_lock.state == LOCK_STATE_LOCKING) {
        g_lock.state = LOCK_STATE_UNLOCKING;
        g_lock.anim_progress = 0.0f;
    }
}

int screenlock_is_locked(void) {
    return (g_lock.state == LOCK_STATE_LOCKED || 
            g_lock.state == LOCK_STATE_LOCKING ||
            g_lock.state == LOCK_STATE_UNLOCKING);
}

// --- Input Handling ---

int screenlock_handle_key(int key) {
    if (g_lock.state != LOCK_STATE_LOCKED) {
        return 0;
    }
    
    // No password set - any key unlocks immediately
    if (!g_lock.user.has_password) {
        screenlock_unlock();
        last_activity_time = get_tick_count();
        return 1;
    }
    
    // Handle password input
    if (key == 0x0D || key == '\n') {
        // Enter - check password (has_password guaranteed true here)
        
        // Hash the entered password and compare with stored hash
        if (sha256_verify_password(g_lock.entered_password, g_lock.user.password_hash)) {
            screenlock_unlock();
            // Reset activity timer on unlock
            last_activity_time = get_tick_count();
            return 1;
        } else {
            // Wrong password
            g_lock.show_error = 1;
            g_lock.error_timer = 60;
            g_lock.entered_password[0] = 0;
            g_lock.cursor_pos = 0;
        }
    } else if (key == 0x08 || key == 0x7F) {
        // Backspace
        if (g_lock.cursor_pos > 0) {
            g_lock.cursor_pos--;
            g_lock.entered_password[g_lock.cursor_pos] = 0;
        }
        g_lock.show_error = 0;
    } else if (key >= 0x20 && key < 0x7F && g_lock.cursor_pos < LOCK_RAW_PASSWORD_MAX - 1) {
        // Character input
        g_lock.entered_password[g_lock.cursor_pos] = (char)key;
        g_lock.cursor_pos++;
        g_lock.entered_password[g_lock.cursor_pos] = 0;
        g_lock.show_error = 0;
    }
    
    return 1;
}

void screenlock_handle_mouse(int mx, int my, int click) {
    // Mouse handling on lock screen - wake on any click
    if (click && g_lock.state == LOCK_STATE_LOCKED) {
        // Just wake the screen, still need to enter password
        g_lock.show_error = 0;
    }
}

// --- Animation ---

void screenlock_update(float dt) {
    switch (g_lock.state) {
        case LOCK_STATE_LOCKING:
            g_lock.anim_progress += dt * 3.0f;
            g_lock.blur_amount = (int)(g_lock.anim_progress * LOCK_BLUR_RADIUS);
            if (g_lock.anim_progress >= 1.0f) {
                g_lock.state = LOCK_STATE_LOCKED;
                g_lock.blur_amount = LOCK_BLUR_RADIUS;
                g_lock.anim_progress = 1.0f;
            }
            break;
            
        case LOCK_STATE_UNLOCKING:
            g_lock.anim_progress += dt * 4.0f;
            g_lock.blur_amount = (int)((1.0f - g_lock.anim_progress) * LOCK_BLUR_RADIUS);
            if (g_lock.anim_progress >= 1.0f) {
                g_lock.state = LOCK_STATE_UNLOCKED;
                g_lock.blur_amount = 0;
                g_lock.anim_progress = 0.0f;
            }
            break;
            
        case LOCK_STATE_LOCKED:
            // Update error timer
            if (g_lock.error_timer > 0) {
                g_lock.error_timer--;
                if (g_lock.error_timer == 0) {
                    g_lock.show_error = 0;
                }
            }
            break;
            
        default:
            break;
    }
}

// --- Rendering Helpers ---

static void draw_gradient_background(int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t blend = (y * 255) / h;
        uint8_t r1 = (C_LOCK_BG_TOP >> 16) & 0xFF;
        uint8_t g1 = (C_LOCK_BG_TOP >> 8) & 0xFF;
        uint8_t b1 = C_LOCK_BG_TOP & 0xFF;
        uint8_t r2 = (C_LOCK_BG_BOTTOM >> 16) & 0xFF;
        uint8_t g2 = (C_LOCK_BG_BOTTOM >> 8) & 0xFF;
        uint8_t b2 = C_LOCK_BG_BOTTOM & 0xFF;
        
        uint8_t r = r1 + ((r2 - r1) * blend) / 255;
        uint8_t g = g1 + ((g2 - g1) * blend) / 255;
        uint8_t b = b1 + ((b2 - b1) * blend) / 255;
        
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        gfx_fill_rect(0, y, w, 1, col);
    }
}

static void draw_avatar(int cx, int cy, int size, int color_idx) {
    uint32_t color = avatar_colors[color_idx % 8];
    
    // Outer circle (avatar background)
    gfx_fill_rounded_rect(cx - size/2, cy - size/2, size, size, color, size/2);
    
    // Inner silhouette (simple person icon)
    int head_size = size / 4;
    int head_y = cy - size/6;
    
    // Head
    gfx_fill_rounded_rect(cx - head_size/2, head_y - head_size/2, 
                          head_size, head_size, 0xFFFFFFFF, head_size/2);
    
    // Body (trapezoid approximation)
    int body_top = head_y + head_size/2 + 2;
    int body_bottom = cy + size/3;
    int body_top_w = size / 4;
    int body_bot_w = size / 2;
    
    for (int y = body_top; y < body_bottom; y++) {
        int progress = (y - body_top) * 255 / (body_bottom - body_top);
        int w = body_top_w + ((body_bot_w - body_top_w) * progress) / 255;
        gfx_fill_rect(cx - w/2, y, w, 1, 0xFFFFFFFF);
    }
}

static void draw_password_dots(int x, int y, int count, int show_error) {
    int dot_size = 6;
    int spacing = 10;
    // Left-aligned dots starting from center - count matches cursor_pos exactly
    int total_w = count * spacing;
    int start_x = x - total_w / 2;
    
    for (int i = 0; i < count; i++) {
        int dot_x = start_x + i * spacing + 2;
        uint32_t color = show_error ? C_LOCK_ERROR : C_LOCK_TEXT;
        gfx_fill_rounded_rect(dot_x, y - dot_size/2, 
                              dot_size, dot_size, color, dot_size/2);
    }
}

// --- Main Render ---

void screenlock_render(uint32_t* buffer, int w, int h, int mx, int my) {
    (void)buffer;
    
    if (g_lock.state == LOCK_STATE_UNLOCKED) {
        return;
    }
    
    int cx = w / 2;
    int cy = h / 2;
    
    // Draw dark gradient background
    draw_gradient_background(w, h);
    
    // Animated fade/blur effect
    uint32_t fade_alpha = (uint32_t)(g_lock.anim_progress * 255);
    if (fade_alpha > 0xFF) fade_alpha = 0xFF;
    
    // Time display at top
    int hour, minute, second;
    sys_get_time(&hour, &minute, &second);
    
    char time_str[16];
    char hour_str[8], min_str[8];
    
    // Format with leading zeros
    if (hour < 10) {
        hour_str[0] = '0';
        hour_str[1] = '0' + hour;
        hour_str[2] = 0;
    } else {
        char buf[8];
        int_to_str(hour, buf);
        strcpy(hour_str, buf);
    }
    
    if (minute < 10) {
        min_str[0] = '0';
        min_str[1] = '0' + minute;
        min_str[2] = 0;
    } else {
        char buf[8];
        int_to_str(minute, buf);
        strcpy(min_str, buf);
    }
    
    strcpy(time_str, hour_str);
    strcat(time_str, ":");
    strcat(time_str, min_str);
    
    // Draw large time
    int time_y = cy - 120;
    gfx_draw_string_scaled(cx - strlen(time_str) * 16, time_y, time_str, C_LOCK_TEXT, 4);
    
    // Date display (below time)
    char date_str[32] = "Welcome to CamelOS";
    gfx_draw_string_scaled(cx - strlen(date_str) * 4, time_y + 80, date_str, C_LOCK_TEXT_DIM, 1);
    
    // Avatar
    int avatar_y = cy + 20;
    int avatar_size = 80;
    draw_avatar(cx, avatar_y, avatar_size, g_lock.user.avatar_color);
    
    // Username
    gfx_draw_string_scaled(cx - strlen(g_lock.user.username) * 4, 
                          avatar_y + avatar_size/2 + 30, 
                          g_lock.user.username, C_LOCK_TEXT, 1);
    
    // Password input field (only show if password is set)
    if (g_lock.user.has_password) {
        int input_y = avatar_y + avatar_size/2 + 80;
        
        // Input background
        int field_w = 200;
        int field_h = 36;
        gfx_fill_rounded_rect(cx - field_w/2, input_y - field_h/2, 
                              field_w, field_h, C_LOCK_INPUT_BG, 8);
        
        // Password dots (dynamic sizing based on actual typed count)
        draw_password_dots(cx, input_y, g_lock.cursor_pos, g_lock.show_error);
        
        // Blinking cursor
        static int blink_timer = 0;
        blink_timer++;
        if ((blink_timer / 30) % 2 == 0) {
            // Cursor position matches the dot layout exactly
            int dot_spacing = 10;
            int total_w = g_lock.cursor_pos * dot_spacing;
            int start_x = cx - total_w / 2;
            int cursor_x = start_x + g_lock.cursor_pos * dot_spacing + 2;
            gfx_fill_rect(cursor_x, input_y - 8, 2, 16, C_LOCK_TEXT);
        }
        
        // Error message
        if (g_lock.show_error) {
            char* error_msg = "Incorrect password. Try again.";
            gfx_draw_string(cx - strlen(error_msg) * 4, input_y + 30, error_msg, C_LOCK_ERROR);
        }
        
        // Bottom hint
        char* bottom_hint = "Enter password to unlock";
        gfx_draw_string(cx - strlen(bottom_hint) * 4, h - 50, bottom_hint, C_LOCK_TEXT_DIM);
    } else {
        // No password - show "Click to unlock"
        int input_y = avatar_y + avatar_size/2 + 80;
        char* hint = "Click or press Enter to unlock";
        gfx_draw_string(cx - strlen(hint) * 4, input_y, hint, C_LOCK_TEXT_DIM);
    }
    
    // Lock icon in corner
    int lock_x = w - 60;
    int lock_y = 30;
    gfx_draw_string(lock_x - 10, lock_y, "LOCK", C_LOCK_TEXT_DIM);
    
    // Draw mouse cursor - proper arrow shape
    if (g_lock.state == LOCK_STATE_LOCKED) {
        static const uint8_t cursor_shape[19][12] = {
            {1,0,0,0,0,0,0,0,0,0,0,0},
            {1,1,0,0,0,0,0,0,0,0,0,0},
            {1,2,1,0,0,0,0,0,0,0,0,0},
            {1,2,2,1,0,0,0,0,0,0,0,0},
            {1,2,2,2,1,0,0,0,0,0,0,0},
            {1,2,2,2,2,1,0,0,0,0,0,0},
            {1,2,2,2,2,2,1,0,0,0,0,0},
            {1,2,2,2,2,2,2,1,0,0,0,0},
            {1,2,2,2,2,2,2,2,1,0,0,0},
            {1,2,2,2,2,2,2,2,2,1,0,0},
            {1,2,2,2,2,2,2,2,2,2,1,0},
            {1,2,2,2,2,2,1,1,1,1,1,1},
            {1,2,2,2,1,2,2,1,0,0,0,0},
            {1,2,2,1,0,1,2,2,1,0,0,0},
            {1,2,1,0,0,1,2,2,1,0,0,0},
            {1,1,0,0,0,0,1,2,2,1,0,0},
            {1,0,0,0,0,0,1,2,2,1,0,0},
            {0,0,0,0,0,0,0,1,2,1,0,0},
            {0,0,0,0,0,0,0,1,1,0,0,0},
        };
        for (int row = 0; row < 19; row++) {
            for (int col = 0; col < 12; col++) {
                int px = mx + col;
                int py = my + row;
                if (px < 0 || px >= 1024 || py < 0 || py >= 768) continue;
                uint8_t v = cursor_shape[row][col];
                if (v == 1) gfx_put_pixel(px, py, 0xFF000000);
                else if (v == 2) gfx_put_pixel(px, py, 0xFFFFFFFF);
            }
        }
    }
}

// --- Integration API ---

int screenlock_filter_event(int* key, int* mx, int* my, int* click) {
    // First check for inactivity timeout
    screenlock_check_inactivity();
    
    if (!screenlock_is_locked()) {
        // Track activity
        if ((key && *key) || (click && *click)) {
            screenlock_on_activity();
        }
        return 0;
    }
    
    // If no password set and locked, unlock on any click immediately
    if (!g_lock.user.has_password && click && *click) {
        screenlock_unlock();
        *click = 0;
        return 1;
    }
    
    // Consume mouse events when locked (but track position)
    if (mx && my && click) {
        screenlock_handle_mouse(*mx, *my, *click);
        *click = 0; // Block the click
    }
    
    // Process keyboard for password entry
    if (key && *key) {
        int consumed = screenlock_handle_key(*key);
        if (consumed) {
            *key = 0; // Block the key from reaching other handlers
        }
        return consumed;
    }
    
    return 1;
}
