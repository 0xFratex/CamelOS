// usr/lib/file_picker.h - File Picker Dialog
// A modal "Choose From" file picker that can be opened from any app
#ifndef FILE_PICKER_H
#define FILE_PICKER_H

#include "../../sys/api.h"
#include "../../fs/pfs32.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"

// Callback type: called when user selects a file (path is the full path)
typedef void (*file_picker_callback_t)(const char* path);

// File picker state (internal, but exposed for window callback access)
typedef struct {
    int active;
    window_t* window;

    // Navigation
    char current_path[256];

    // Directory entries
    pfs32_direntry_t entries[64];
    int entry_count;
    int selected_idx;

    // Scroll
    int scroll_offset;

    // Double-click tracking
    int last_click_idx;
    int last_click_frame;
    int frame_counter;

    // Callback
    file_picker_callback_t on_selected;

    // Window dimensions
    int win_w;
    int win_h;
} file_picker_state_t;

// Open a file picker dialog starting at the given path.
// When user selects a file and clicks "Open", on_selected is called with the full path.
// If user cancels, the picker closes without calling the callback.
void file_picker_open(const char* start_path, file_picker_callback_t on_selected);

// Close the file picker programmatically
void file_picker_close(void);

// Global picker state (accessible from callbacks)
extern file_picker_state_t g_file_picker;

#endif
