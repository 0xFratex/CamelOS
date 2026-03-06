// usr/screenlock.h - Camel OS Screen Lock Header
#ifndef SCREENLOCK_H
#define SCREENLOCK_H

#include "../include/types.h"

// Lock screen configuration
#define LOCK_PASSWORD_MAX 32
#define LOCK_USER_MAX 64
#define LOCK_BLUR_RADIUS 8

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
    char password[LOCK_PASSWORD_MAX];
    int  password_len;
    char avatar_color;  // 0-7 for different avatar colors
} LockUserProfile;

// Lock screen state
typedef struct {
    LockState state;
    LockUserProfile user;
    char entered_password[LOCK_PASSWORD_MAX];
    int cursor_pos;
    int show_error;
    int error_timer;
    uint32_t lock_time;
    int blur_amount;
    float anim_progress;
} ScreenLock;

// Public API
void screenlock_init(void);
void screenlock_set_user(const char* username, const char* password);
void screenlock_lock(void);
void screenlock_unlock(void);
int screenlock_is_locked(void);
int screenlock_handle_key(int key);
void screenlock_render(uint32_t* buffer, int w, int h, int mx, int my);
void screenlock_handle_mouse(int mx, int my, int click);

// User management
void screenlock_create_user(const char* username, const char* password, int avatar_color);
LockUserProfile* screenlock_get_user(void);

// Animation
void screenlock_update(float dt);

#endif // SCREENLOCK_H
