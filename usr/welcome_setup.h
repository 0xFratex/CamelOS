// usr/welcome_setup.h - Camel OS Welcome Setup Header
#ifndef WELCOME_SETUP_H
#define WELCOME_SETUP_H

#include "../include/types.h"

// Configuration limits
#define SETUP_USERNAME_MAX  64
#define SETUP_TIMEZONE_MAX  64

// Theme options
typedef enum {
    THEME_AQUA,       // Classic blue
    THEME_GRAPHITE,   // Grey professional
    THEME_SUNSET,     // Orange warm
    THEME_OCEAN,      // Teal calm
    THEME_FOREST,     // Green nature
    THEME_COUNT
} ThemeType;

// Timezone data
typedef struct {
    char name[32];
    char display[32];
    int offset_minutes;  // Offset from UTC in minutes
} TimeZone;

// System configuration
typedef struct {
    char username[SETUP_USERNAME_MAX];
    char computer_name[SETUP_USERNAME_MAX];
    TimeZone timezone;
    ThemeType theme;
    int is_configured;
    uint32_t config_version;
} SystemConfig;

// Setup wizard state
typedef enum {
    SETUP_STATE_WELCOME,
    SETUP_STATE_USER,
    SETUP_STATE_TIMEZONE,
    SETUP_STATE_THEME,
    SETUP_STATE_COMPLETE
} SetupState;

typedef struct {
    SetupState state;
    SystemConfig config;
    
    // UI state
    int current_step;
    int total_steps;
    int selected_tz_idx;
    int selected_theme_idx;
    int show_keyboard;
    char input_buffer[SETUP_USERNAME_MAX];
    int input_cursor;
    int input_active;
    float anim_progress;
} WelcomeSetup;

// Public API
void welcome_setup_init(void);
int welcome_setup_needs_setup(void);
void welcome_setup_start(void);
int welcome_setup_is_active(void);
void welcome_setup_finish(void);

// Rendering
void welcome_setup_render(uint32_t* buffer, int w, int h, int mx, int my);
void welcome_setup_update(float dt);

// Input handling
int welcome_setup_handle_key(int key);
int welcome_setup_handle_mouse(int mx, int my, int click, int pressed);
int welcome_setup_handle_click(int mx, int my);

// Configuration access
SystemConfig* welcome_setup_get_config(void);
void welcome_setup_load_config(void);
void welcome_setup_save_config(void);

// Defaults
void welcome_setup_set_defaults(void);

#endif // WELCOME_SETUP_H
