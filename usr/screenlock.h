// usr/screenlock.h - Camel OS Screen Lock Header
// Enhanced with encrypted password verification
#ifndef SCREENLOCK_H
#define SCREENLOCK_H

#include "../include/types.h"

// Lock screen configuration
#define LOCK_PASSWORD_MAX 65    // SHA-256 hex hash is 64 chars + null
#define LOCK_USER_MAX 64
#define LOCK_BLUR_RADIUS 8
#define LOCK_RAW_PASSWORD_MAX 32  // For entered (plaintext) password

// Lock states
typedef enum {
    LOCK_STATE_UNLOCKED,
    LOCK_STATE_LOCKING,
    LOCK_STATE_LOCKED,
    LOCK_STATE_UNLOCKING
} LockState;

// User profile for lock screen
typedef struct {
    char username[LOCK_USER_MAX];
    char password_hash[LOCK_PASSWORD_MAX];  // SHA-256 hash (hex string)
    int  has_password;                       // Whether a password is set
    char avatar_color;                       // 0-7 for different avatar colors
} LockUserProfile;

// Lock screen state
typedef struct {
    LockState state;
    LockUserProfile user;
    char entered_password[LOCK_RAW_PASSWORD_MAX];  // Raw password being entered
    int cursor_pos;
    int show_error;
    int error_timer;
    uint32_t lock_time;
    int blur_amount;
    float anim_progress;
} ScreenLock;

// Public API
void screenlock_init(void);
void screenlock_set_user(const char* username, const char* password_hash);
void screenlock_lock(void);
void screenlock_unlock(void);
int screenlock_is_locked(void);
int screenlock_handle_key(int key);
void screenlock_render(uint32_t* buffer, int w, int h, int mx, int my);
void screenlock_handle_mouse(int mx, int my, int click);

// User management - now takes hash instead of plaintext
void screenlock_create_user(const char* username, const char* password_hash, int avatar_color);
LockUserProfile* screenlock_get_user(void);

// Load user from system config
void screenlock_load_user(void);

// Animation
void screenlock_update(float dt);

// Inactivity
void screenlock_set_inactivity_timeout(int minutes);
void screenlock_set_inactivity_enabled(int enabled);
void screenlock_on_activity(void);
int screenlock_check_inactivity(void);

// Event filtering - returns 1 if event was consumed (locked), 0 if not
int screenlock_filter_event(int* key, int* mx, int* my, int* click);

#endif // SCREENLOCK_H
