// usr/lib/camel_framework.h
#ifndef CAMEL_FW_H
#define CAMEL_FW_H

#include "../../sys/cdl_defs.h"

typedef unsigned int uint32_t;

// --- EXISTING INIT ---
void cm_init(kernel_api_t* api);
int cm_load_app_config(const char* app_bundle_path);
void cm_bind_action(const char* action_id, void (*func)(void));
const char* cm_get_config(const char* key);
void cm_draw_image_clipped(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h, int clip_x, int clip_y, int clip_w, int clip_h);
void cm_draw_image(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h);
void cm_apply_menus(void* win_handle);

// --- NEW: FILE DIALOG FRAMEWORK ---

#define FP_MODE_OPEN 0
#define FP_MODE_SAVE 1

// Callback signature: returns 1 if dialog should close, 0 to keep open
// path: The full path selected or typed by the user
typedef void (*file_picker_cb_t)(const char* path);

typedef struct {
    int active;
    int mode;           // FP_MODE_OPEN or FP_MODE_SAVE
    char title[32];
    char current_dir[128];
    char filename_input[64]; // For Save As
    char filter_ext[8];      // e.g. ".txt" or "*"
    
    // UI State
    int scroll_offset;
    int selected_index;
    
    // Callback
    file_picker_cb_t callback;
    
    // Cache
    // Note: In a real OS this would be dynamic.
    // For this environment, we keep a small static cache of names for the view.
    struct {
        char name[40];
        int is_dir;
        int size;
    } entries[64];
    int entry_count;
} file_picker_t;

// Global picker instance (singleton for simplicity per app)
extern file_picker_t cm_picker;

// API
void cm_dialog_init();
void cm_dialog_open(const char* title, const char* start_dir, const char* filter, file_picker_cb_t cb);
void cm_dialog_save(const char* title, const char* start_dir, const char* default_name, const char* filter, file_picker_cb_t cb);

// Integration hooks (call these from your app's main loop)
// Returns 1 if the dialog handled the event (consumed), 0 otherwise
int cm_dialog_render(int win_x, int win_y, int win_w, int win_h);
int cm_dialog_handle_mouse(int x, int y, int btn);
int cm_dialog_handle_input(int key);

#endif
