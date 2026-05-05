// usr/spotlight.h - Spotlight-like Search (Cmd+Space)
// A macOS-inspired global search overlay for CamelOS
#ifndef SPOTLIGHT_H
#define SPOTLIGHT_H

#include "../common/gui_types.h"

typedef unsigned int uint32_t;

// Spotlight result types
typedef enum {
    RESULT_APP,
    RESULT_FILE,
    RESULT_COMMAND,
    RESULT_SETTING,
    RESULT_CONTACT
} SpotlightResultType;

// A single search result
typedef struct {
    char title[64];
    char subtitle[96];
    SpotlightResultType type;
    char icon_char;         // Single character icon (e.g., 'A' for app, 'F' for file)
    uint32_t icon_color;    // ARGB color for the icon
    void (*action)(void);   // Callback when result is selected
} SpotlightResult;

// Spotlight state
typedef struct {
    int active;             // Is the spotlight overlay visible?
    char query[64];         // Current search query
    int query_len;          // Length of current query
    int cursor_pos;         // Cursor position in query
    SpotlightResult results[16]; // Search results
    int result_count;       // Number of results
    int selected_idx;       // Currently highlighted result (-1 = none)
    int scroll_offset;      // Scroll offset for many results
} SpotlightState;

// Global spotlight state
extern SpotlightState g_spotlight;

// Initialize spotlight subsystem
void spotlight_init(void);

// Show/hide spotlight overlay
void spotlight_toggle(void);
void spotlight_show(void);
void spotlight_hide(void);

// Handle keyboard input while spotlight is active
// Returns 1 if the key was consumed by spotlight, 0 otherwise
int spotlight_handle_key(int key);

// Handle mouse input while spotlight is active
// Returns 1 if the click was consumed by spotlight, 0 otherwise
int spotlight_handle_mouse(int mx, int my, int click);

// Draw the spotlight overlay
void spotlight_draw(void);

// Perform search with current query
void spotlight_search(void);

// Execute the currently selected result
void spotlight_execute_selected(void);

// Register built-in apps and commands for search
void spotlight_register_defaults(void);

// Register a custom search result
void spotlight_register_result(const char* title, const char* subtitle,
                               SpotlightResultType type, char icon_char,
                               uint32_t icon_color, void (*action)(void));

#endif /* SPOTLIGHT_H */
