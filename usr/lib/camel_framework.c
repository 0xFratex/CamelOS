// usr/lib/camel_framework.c
#include "camel_framework.h"
#include "../../kernel/assets.h"
#include "../../core/string.h"
#include "../../hal/drivers/vga.h"
// REMOVED: #include <string.h>

static kernel_api_t* sys = 0;

// Forward declarations
void parse_menus_from_string(char* xml);

// --- Helpers ---

static inline int safe_div2(int val) {
    return val >> 1;  // Divide by 2 using shift
}

int my_memcmp(const void* s1, const void* s2, unsigned long n) {
    const char* p1 = s1;
    const char* p2 = s2;
    for(unsigned long i=0; i<n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

char* my_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    int hlen = sys->strlen(haystack);
    int nlen = sys->strlen(needle);
    for(int i=0; i<=hlen-nlen; i++) {
        if (my_memcmp(haystack+i, needle, nlen) == 0) return (char*)haystack+i;
    }
    return 0;
}

char* my_strchr(const char* s, int c) {
    if(!s) return 0;
    while(*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return 0;
}

// --- Data Structures ---

#define MAX_ACTIONS 16
typedef struct {
    char id[32];
    void (*func)(void);
} action_bind_t;

action_bind_t actions[MAX_ACTIONS];
int action_count = 0;

static menu_def_t temp_menus[4];
static int temp_menu_count = 0;

#define MAX_CONFIG_KEYS 8
typedef struct {
    char key[32];
    char value[256]; // Increased size to hold menu XML chunks
} config_pair_t;

config_pair_t app_config[MAX_CONFIG_KEYS];
int config_count = 0;

// --- Initialization ---

void cm_init(kernel_api_t* api) {
    static int initialized = 0;
    if (initialized) {
        if(api && api->print) api->print("[FW] WARNING: cm_init called twice!\n");
        return;
    }
    initialized = 1;
    
    sys = api;
    // Avoid calling print here if stack alignment is still suspect, but it helps debugging
    sys->print("[FW] cm_init: Setting up framework...\n");
    
    sys->memset(actions, 0, sizeof(actions));
    action_count = 0;
    config_count = 0;
    temp_menu_count = 0;
    
    sys->print("[FW] cm_init: Done.\n");
}

void cm_bind_action(const char* action_id, void (*func)(void)) {
    if (action_count >= MAX_ACTIONS) return;
    sys->strcpy(actions[action_count].id, action_id);
    actions[action_count].func = func;
    action_count++;
}

void execute_action_by_id(const char* id) {
    for(int i=0; i<action_count; i++) {
        if (sys->strcmp(actions[i].id, id) == 0) {
            if (actions[i].func) actions[i].func();
            return;
        }
    }
}

// Callback from Kernel
void internal_menu_callback(int menu_idx, int item_idx) {
    // === HANDLE MAGIC SUBMENU IDs (Fix for Prompt) ===
    // Bubbleview injects these IDs when the cascading submenu items are clicked.
    if (item_idx == 100) { 
        // "Create > Folder" clicked
        execute_action_by_id("fs_new_folder");
        return;
    }
    if (item_idx == 101) { 
        // "Create > File" clicked
        execute_action_by_id("fs_new_file");
        return;
    }
    
    if (menu_idx < 0 || menu_idx >= temp_menu_count) return;
    
    const char* id = temp_menus[menu_idx].items[item_idx].action_id;
    execute_action_by_id(id);
}

// --- Improved XML Parser ---

// Helper to copy N chars
void strncpy_safe(char* dest, const char* src, int n) {
    int i;
    for(i=0; i<n && src[i] != 0; i++) dest[i] = src[i];
    dest[i] = 0;
}

void parse_plist_xml(char* buf) {
    char* ptr = buf;
    config_count = 0;
    
    while (*ptr) {
        // 1. Find <key>
        char* key_tag = my_strstr(ptr, "<key>");
        if (!key_tag) break;
        
        char* key_content_start = my_strstr(key_tag, ">");
        if (key_content_start) key_content_start++; else break;
        
        char* key_content_end = my_strstr(key_content_start, "</key>");
        if (!key_content_end) break;
        
        // Store Key
        char key_name[32];
        int klen = key_content_end - key_content_start;
        if (klen > 31) klen = 31;
        strncpy_safe(key_name, key_content_start, klen);
        
        // 2. Find value <string> immediately following
        char* val_tag = my_strstr(key_content_end, "<string>");
        if (!val_tag) break; 
        
        char* val_content_start = my_strstr(val_tag, ">");
        if(val_content_start) val_content_start++; else break;
        
        // Look for closing tag specifically
        char* val_content_end = my_strstr(val_content_start, "</string>");
        if (!val_content_end) break;
        
        int vlen = val_content_end - val_content_start;
        
        // Store in Config
        if (config_count < MAX_CONFIG_KEYS) {
            sys->strcpy(app_config[config_count].key, key_name);
            
            if (vlen > 255) vlen = 255;
            strncpy_safe(app_config[config_count].value, val_content_start, vlen);
            
            // Parse menu def immediately
            if (sys->strcmp(key_name, "CamelMenuDef") == 0) {
                parse_menus_from_string(app_config[config_count].value);
            }
            
            config_count++;
        }
        
        ptr = val_content_end + 9; 
    }
}

void parse_menus_from_string(char* xml) {
    char* ptr = xml;
    temp_menu_count = 0;
    sys->memset(temp_menus, 0, sizeof(temp_menus));
    
    while (*ptr) {
        char* menu_tag = my_strstr(ptr, "<Menu");
        if (!menu_tag) break;
        
        char* name_ptr = my_strstr(menu_tag, "name=\"");
        if (name_ptr) {
            name_ptr += 6;
            
            if (temp_menu_count >= 4) break; 
            
            int m_idx = temp_menu_count++;
            int i=0;
            while(*name_ptr && *name_ptr != '"' && i<11) {
                temp_menus[m_idx].name[i++] = *name_ptr++;
            }
            temp_menus[m_idx].name[i] = 0;
            
            char* menu_end = my_strstr(menu_tag, "</Menu>");
            if (!menu_end) break;
            
            char* item_ptr = name_ptr;
            while(item_ptr < menu_end) {
                char* item_tag = my_strstr(item_ptr, "<Item");
                if(!item_tag || item_tag > menu_end) break;
                
                int i_idx = temp_menus[m_idx].item_count;
                if (i_idx >= 5) break;
                
                char* lbl_p = my_strstr(item_tag, "label=\"");
                if (lbl_p) {
                    lbl_p += 7;
                    int k=0; while(*lbl_p && *lbl_p != '"' && k<15) {
                        temp_menus[m_idx].items[i_idx].label[k++] = *lbl_p++;
                    }
                }
                
                char* id_p = my_strstr(item_tag, "id=\"");
                if (id_p) {
                    id_p += 4;
                    int k=0; while(*id_p && *id_p != '"' && k<31) {
                        temp_menus[m_idx].items[i_idx].action_id[k++] = *id_p++;
                    }
                }
                
                temp_menus[m_idx].item_count++;
                item_ptr = item_tag + 5;
            }
        }
        ptr = menu_tag + 5;
    }
}

const char* cm_get_config(const char* key) {
    for(int i=0; i<config_count; i++) {
        if (sys->strcmp(app_config[i].key, key) == 0) return app_config[i].value;
    }
    return 0;
}

int cm_load_app_config(const char* app_bundle_path) {
    sys->print("[FW] Loading config for: "); sys->print(app_bundle_path); sys->print("\n");
    
    char clist_path[128];
    sys->strcpy(clist_path, app_bundle_path);
    
    int len = sys->strlen(clist_path);
    if (len > 0 && clist_path[len-1] != '/') sys->strcpy(clist_path + len, "/");
    
    sys->strcpy(clist_path + sys->strlen(clist_path), "Info.clist");
    
    char* file_buf = (char*)sys->malloc(4096);
    if (!file_buf) {
        sys->print("[FW] Failed to allocate config buffer\n");
        return 0;
    }
    sys->memset(file_buf, 0, 4096);
    
    int size = sys->fs_read(clist_path, file_buf, 4095);
    if (size <= 0) {
        sys->print("[FW] Config not found (or empty): "); sys->print(clist_path); sys->print("\n");
        sys->free(file_buf);
        return 0;
    }
    
    // FIX: Ensure buffer is null terminated even if fs_read filled it completely
    file_buf[size] = 0; 
    
    parse_plist_xml(file_buf);
    sys->free(file_buf);
    
    sys->print("[FW] Config loaded successfully.\n");
    return 1;
}

void cm_apply_menus(void* win_handle) {
    if (!sys || !win_handle) return;
    if (temp_menu_count > 0) {
        sys->set_window_menu(win_handle, temp_menus, temp_menu_count, internal_menu_callback);
    }
}

#ifdef KERNEL_MODE
// Fallback strcmp implementation for when sys is NULL
static int fallback_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void cm_draw_image(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h) {
    uint32_t count = 0;
    const embedded_image_t* assets = get_embedded_images(&count);
    
    // Safety check for sys pointer
    int (*strcmp_func)(const char*, const char*) = (sys) ? sys->strcmp : fallback_strcmp;
    
    for(uint32_t i=0; i<count; i++) {
        if(strcmp_func(assets[i].name, name) == 0) {
            gfx_draw_asset_scaled(buffer, x, y, assets[i].data, assets[i].width, assets[i].height, req_w, req_h);
            return;
        }
    }
}
void cm_draw_image_clipped(uint32_t* buff, const char* name, int x, int y, int dw, int dh, int cx, int cy, int cw, int ch) {
    cm_draw_image(buff, name, x, y, dw, dh);
}
#else
void cm_draw_image(uint32_t* buffer, const char* name, int x, int y, int req_w, int req_h) {
    if(sys && sys->draw_image) sys->draw_image(x, y, name);
}
void cm_draw_image_clipped(uint32_t* buff, const char* name, int x, int y, int dw, int dh, int cx, int cy, int cw, int ch) {
    if(sys && sys->draw_image) sys->draw_image(x, y, name);
}
#endif

// --- FILE PICKER IMPLEMENTATION ---

file_picker_t cm_picker;

// Constants for Dialog UI
#define DLG_W 400
#define DLG_H 300
#define DLG_BG 0xFFF0F0F0
#define DLG_TITLE 0xFF404040
#define DLG_LIST_BG 0xFFFFFFFF
#define DLG_SEL 0xFFB3D7FF

// Helper to refresh file list
// FIX: Reset selection when changing directories
void cm_picker_refresh() {
    sys->print("[FW] Picker Refresh...\n");
    if (!sys) return;
    
    // 1. Clear current
    cm_picker.entry_count = 0;
    
    // 2. Alloc temp buffer for FS call
    // === FIX: Match Kernel pfs32_direntry_t Layout EXACTLY (Packed) ===
    typedef struct {
        char filename[40];
        uint32_t size;
        uint32_t start;
        unsigned char attr;
        unsigned char uid;
        unsigned char perm;
        unsigned char gid;
        uint32_t dates[3]; // create, mod, access
    } __attribute__((packed)) raw_entry_t;
    
    int max_read = 64;
    raw_entry_t* buffer = (raw_entry_t*)sys->malloc(max_read * sizeof(raw_entry_t));
    if (!buffer) return;
    
    sys->memset(buffer, 0, max_read * sizeof(raw_entry_t));
    
    int count = sys->fs_list(cm_picker.current_dir, buffer, max_read);
    
    for(int i=0; i<count; i++) {
        if (buffer[i].filename[0] == 0) continue;
        if (sys->strcmp(buffer[i].filename, ".") == 0) continue; // Skip .
        
        int is_dir = (buffer[i].attr & 0x10) ? 1 : 0;
        
        // Filter Logic:
        // 1. Always show directories
        // 2. If filter is "*", show all files
        // 3. If filter is specific (e.g., ".txt"), check extension
        int show = 0;
        if (is_dir) show = 1;
        else {
            if (sys->strcmp(cm_picker.filter_ext, "*") == 0 || cm_picker.filter_ext[0] == 0) {
                show = 1;
            } else {
                // Check extension
                int nlen = sys->strlen(buffer[i].filename);
                int flen = sys->strlen(cm_picker.filter_ext);
                if (nlen > flen) {
                    if (sys->strcmp(buffer[i].filename + nlen - flen, cm_picker.filter_ext) == 0) show = 1;
                }
            }
        }
        
        if (show && cm_picker.entry_count < 64) {
            int idx = cm_picker.entry_count++;
            sys->strcpy(cm_picker.entries[idx].name, buffer[i].filename);
            cm_picker.entries[idx].is_dir = is_dir;
            cm_picker.entries[idx].size = buffer[i].size;
        }
    }
    
    sys->free(buffer);
    // Reset state to prevent ghost clicks on wrong index
    cm_picker.selected_index = -1;
    cm_picker.scroll_offset = 0;
    
    sys->print("[FW] Picker Refresh Done.\n");
}

void cm_dialog_init() {
    sys->print("[FW] Dialog Init...\n");
    sys->memset(&cm_picker, 0, sizeof(file_picker_t));
    cm_picker.active = 0;
    sys->print("[FW] Dialog Init Done.\n");
}

void cm_dialog_open(const char* title, const char* start_dir, const char* filter, file_picker_cb_t cb) {
    cm_picker.active = 1;
    cm_picker.mode = FP_MODE_OPEN;
    sys->strcpy(cm_picker.title, title ? title : "Open");
    sys->strcpy(cm_picker.current_dir, start_dir ? start_dir : "/home");
    sys->strcpy(cm_picker.filter_ext, filter ? filter : "*");
    cm_picker.callback = cb;
    cm_picker.filename_input[0] = 0;
    cm_picker_refresh();
}

void cm_dialog_save(const char* title, const char* start_dir, const char* default_name, const char* filter, file_picker_cb_t cb) {
    cm_dialog_open(title, start_dir, filter, cb);
    cm_picker.mode = FP_MODE_SAVE;
    if (default_name) sys->strcpy(cm_picker.filename_input, default_name);
}

void cm_dialog_up_dir() {
    if (sys->strcmp(cm_picker.current_dir, "/") == 0) return;
    
    // Strip last segment
    int len = sys->strlen(cm_picker.current_dir);
    if (len > 1 && cm_picker.current_dir[len-1] == '/') {
        cm_picker.current_dir[len-1] = 0;
        len--;
    }
    
    while(len > 0 && cm_picker.current_dir[len] != '/') len--;
    if (len == 0) sys->strcpy(cm_picker.current_dir, "/");
    else cm_picker.current_dir[len] = 0;
    
    cm_picker_refresh();
}

void cm_dialog_select_dir(const char* dirname) {
    int len = sys->strlen(cm_picker.current_dir);
    if (len > 0 && cm_picker.current_dir[len-1] != '/') sys->strcpy(cm_picker.current_dir + len, "/");
    sys->strcpy(cm_picker.current_dir + sys->strlen(cm_picker.current_dir), dirname);
    cm_picker_refresh();
}

// === FIXED: Submit Logic handles Directory Selection ===
void cm_dialog_submit() {
    // If a directory is selected, enter it instead of submitting
    if (cm_picker.selected_index >= 0 && cm_picker.selected_index < cm_picker.entry_count) {
        if (cm_picker.entries[cm_picker.selected_index].is_dir) {
            cm_dialog_select_dir(cm_picker.entries[cm_picker.selected_index].name);
            return;
        }
    }

    char full_path[128];
    sys->strcpy(full_path, cm_picker.current_dir);
    int len = sys->strlen(full_path);
    
    // Only append slash if not root and not already ending in slash
    if (len > 0 && full_path[len-1] != '/' && sys->strcmp(full_path, "/") != 0) {
        sys->strcpy(full_path + len, "/");
    } else if (len == 0) {
        sys->strcpy(full_path, "/");
    }
    
    if (cm_picker.mode == FP_MODE_OPEN) {
        if (cm_picker.selected_index >= 0 && cm_picker.selected_index < cm_picker.entry_count) {
            sys->strcpy(full_path + sys->strlen(full_path), cm_picker.entries[cm_picker.selected_index].name);
            if (cm_picker.callback) cm_picker.callback(full_path);
            cm_picker.active = 0;
        }
    } else {
        // Save
        if (sys->strlen(cm_picker.filename_input) > 0) {
            sys->strcpy(full_path + sys->strlen(full_path), cm_picker.filename_input);
            if (cm_picker.callback) cm_picker.callback(full_path);
            cm_picker.active = 0;
        }
    }
}

// RENDER
int cm_dialog_render(int win_x, int win_y, int win_w, int win_h) {
    if (!cm_picker.active || !sys) return 0;
    
    // Calculate center
    int x = win_x + safe_div2(win_w - DLG_W);
    int y = win_y + safe_div2(win_h - DLG_H);
    
    // 1. Shadow / Modal Dim
    // sys->draw_rect(win_x, win_y, win_w, win_h, 0x40000000); // Optional global dim
    sys->draw_rect_rounded(x+5, y+5, DLG_W, DLG_H, 0x40000000, 8); // Shadow
    
    // 2. Background
    sys->draw_rect_rounded(x, y, DLG_W, DLG_H, DLG_BG, 6);
    sys->draw_rect(x, y, DLG_W, 1, 0xFF888888); // Border
    sys->draw_rect(x, y+DLG_H, DLG_W, 1, 0xFF888888);
    sys->draw_rect(x, y, 1, DLG_H, 0xFF888888);
    sys->draw_rect(x+DLG_W, y, 1, DLG_H, 0xFF888888);
    
    // 3. Header
    sys->draw_rect_rounded(x+10, y+10, 30, 20, 0xFFDDDDDD, 4); // Up Button
    sys->draw_text(x+20, y+16, "^", 0xFF000000);
    
    sys->draw_text(x+50, y+16, cm_picker.title, 0xFF000000);
    sys->draw_text_clipped(x+150, y+16, cm_picker.current_dir, 0xFF666666, 230);
    
    // 4. File List
    int list_y = y + 40;
    int list_h = 200;
    if (cm_picker.mode == FP_MODE_SAVE) list_h -= 30; // Make room for input
    
    sys->draw_rect(x+10, list_y, DLG_W-20, list_h, DLG_LIST_BG);
    sys->draw_rect(x+10, list_y, DLG_W-20, 1, 0xFFAAAAAA);
    sys->draw_rect(x+10, list_y+list_h, DLG_W-20, 1, 0xFFAAAAAA);
    
    // Draw items
    int item_h = 20;
    int visible_items = list_h / item_h;
    
    for (int i = 0; i < visible_items; i++) {
        int idx = cm_picker.scroll_offset + i;
        if (idx >= cm_picker.entry_count) break;
        
        int iy = list_y + (i * item_h);
        
        // Selection
        if (idx == cm_picker.selected_index) {
            sys->draw_rect(x+11, iy, DLG_W-22, item_h, DLG_SEL);
        }
        
        // Icon
        const char* icon = cm_picker.entries[idx].is_dir ? "folder" : "file";
        cm_draw_image(0, icon, x+14, iy+2, 16, 16);
        
        // Text
        sys->draw_text(x+35, iy+6, cm_picker.entries[idx].name, 0xFF000000);
    }
    
    // 5. Footer (Input / Buttons)
    int fy = y + DLG_H - 40;
    
    if (cm_picker.mode == FP_MODE_SAVE) {
        // Filename Input
        sys->draw_text(x+15, fy-25, "Name:", 0xFF000000);
        sys->draw_rect(x+60, fy-30, 200, 20, 0xFFFFFFFF);
        sys->draw_rect(x+60, fy-30, 200, 1, 0xFF000000);
        sys->draw_text(x+65, fy-25, cm_picker.filename_input, 0xFF000000);
    }
    
    // Cancel
    sys->draw_rect_rounded(x + DLG_W - 150, fy, 60, 24, 0xFFCCCCCC, 4);
    sys->draw_text(x + DLG_W - 140, fy+8, "Cancel", 0xFF000000);
    
    // Open/Save
    sys->draw_rect_rounded(x + DLG_W - 80, fy, 60, 24, 0xFF007AFF, 4);
    sys->draw_text(x + DLG_W - 65, fy+8, (cm_picker.mode == FP_MODE_SAVE) ? "Save" : "Open", 0xFFFFFFFF);
    
    return 1; // Consumed
}

int cm_dialog_handle_mouse(int mx, int my, int btn) {
    if (!cm_picker.active || !sys) return 0;
    
    // Simple bounds check: if click outside, consume it (modal) but do nothing?
    // Or close? Let's just block interactions.
    // Center calc again
    // In a real system, these would be cached.
    // int win_w = 1024; int win_h = 768; // Assumption or passed in? 
    // We actually need the window context. 
    // For now, assume centered on screen or passed coordinates. 
    // Limitation: The render function calculates X/Y based on window. 
    // We will assume 0,0 relative to window content area for simplicity in this snippet,
    // OR we assume the caller handles the offset. 
    // The previous app implementation passes local coordinates (relative to window).
    
    // Let's assume standard dialog rect within the window:
    // extern int win_w_global; // Hack: app needs to expose its size or we store it
    // Better: Hardcode center logic based on assumptions of the caller.
    // We will rely on the app passing correct relative coords.
    
    // Hit test logic omitted for brevity in "mouse move", but critical for "click".
    
    if (btn == 1) {
        // Check buttons
        // Logic similar to rendering...
    }
    
    return 1; // Block underlying app
}

// More precise input handling moved to specific app implementation for cleaner integration
// The function below provides the logic for the app to call.

int cm_dialog_click(int win_w, int win_h, int mx, int my) {
    if (!cm_picker.active) return 0;
    
    int x = safe_div2(win_w - DLG_W);
    int y = safe_div2(win_h - DLG_H);
    
    if (mx < x || mx > x + DLG_W || my < y || my > y + DLG_H) return 1; // Modal block
    
    // Up Button
    if (mx >= x+10 && mx <= x+40 && my >= y+10 && my <= y+30) {
        cm_dialog_up_dir();
        return 1;
    }
    
    // List Click
    int list_y = y + 40;
    int list_h = 200;
    if (cm_picker.mode == FP_MODE_SAVE) list_h -= 30;
    
    if (mx >= x+10 && mx <= x+DLG_W-20 && my >= list_y && my <= list_y+list_h) {
        int idx = (my - list_y) / 20;
        int real_idx = cm_picker.scroll_offset + idx;
        
        if (real_idx >= 0 && real_idx < cm_picker.entry_count) {
            
            // If clicking the same item (Double Click logic sim)
            if (cm_picker.selected_index == real_idx) {
                if (cm_picker.entries[real_idx].is_dir) {
                    cm_dialog_select_dir(cm_picker.entries[real_idx].name);
                } else {
                    if (cm_picker.mode == FP_MODE_OPEN) cm_dialog_submit();
                    else {
                        // Double click file in Save mode -> Overwrite name
                        sys->strcpy(cm_picker.filename_input, cm_picker.entries[real_idx].name);
                    }
                }
            } else {
                // Single Click -> Select
                cm_picker.selected_index = real_idx;
                
                // If it's a file in save mode, populate name. If folder, DON'T change name.
                if (cm_picker.mode == FP_MODE_SAVE && !cm_picker.entries[real_idx].is_dir) {
                    sys->strcpy(cm_picker.filename_input, cm_picker.entries[real_idx].name);
                }
            }
        }
        return 1;
    }
    
    // Footer Buttons
    int fy = y + DLG_H - 40;
    
    // Cancel
    if (mx >= x + DLG_W - 150 && mx <= x + DLG_W - 90 && my >= fy && my <= fy + 24) {
        cm_picker.active = 0;
        return 1;
    }
    
    // OK
    if (mx >= x + DLG_W - 80 && mx <= x + DLG_W - 20 && my >= fy && my <= fy + 24) {
        cm_dialog_submit();
        return 1;
    }
    
    return 1;
}

int cm_dialog_input(int key) {
    if (!cm_picker.active) return 0;
    
    if (key == 27) { // ESC
        cm_picker.active = 0;
        return 1;
    }
    
    if (key == '\n') {
        cm_dialog_submit();
        return 1;
    }
    
    if (cm_picker.mode == FP_MODE_SAVE) {
        // Typing into filename box
        int len = sys->strlen(cm_picker.filename_input);
        if (key == '\b') {
            if (len > 0) cm_picker.filename_input[len-1] = 0;
        } else if (key >= 32 && key <= 126 && len < 63) {
            cm_picker.filename_input[len] = (char)key;
            cm_picker.filename_input[len+1] = 0;
        }
    }
    
    return 1;
}