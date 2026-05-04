#include "../usr/framework.h"
#include "../core/string.h"
#include "../core/http.h"
#include "../../sys/api.h"
#include "../../hal/video/gfx_hal.h"
#include "../../core/window_server.h"

#define TERM_COLS 80
#define TERM_ROWS 24
#define CHAR_W 8
#define CHAR_H 16
#define PAD 8

char term_buffer[TERM_ROWS][TERM_COLS + 1];
int terminal_row = 0;
int terminal_col = 0;
char current_term_path[64] = "/";
int term_fg_color = 0xFFCCCCCC;  // Light gray on black (modern terminal look)

void term_reset() {
    for(int y=0; y<TERM_ROWS; y++) {
        for(int x=0; x<TERM_COLS; x++) term_buffer[y][x] = 0;
        term_buffer[y][TERM_COLS] = 0;
    }
    terminal_row = 0;
    terminal_col = 0;
    strcpy(current_term_path, "/");
    
    char prompt[] = "camelos: / $ ";
    for(int i=0; prompt[i] && i < TERM_COLS; i++) {
        term_buffer[0][i] = prompt[i];
    }
    terminal_col = strlen(prompt);
    if (terminal_col >= TERM_COLS) terminal_col = TERM_COLS - 1;
}

void term_scroll() {
    for(int r=0; r < TERM_ROWS-1; r++) {
        memcpy(term_buffer[r], term_buffer[r+1], TERM_COLS + 1);
    }
    memset(term_buffer[TERM_ROWS-1], 0, TERM_COLS + 1);
    terminal_row = TERM_ROWS-1;
}

void term_print(const char* str) {
    int i = 0;
    while(str[i]) {
        if (terminal_col >= TERM_COLS) {
            terminal_row++;
            terminal_col = 0;
            if (terminal_row >= TERM_ROWS) term_scroll();
        }

        if (str[i] == '\n') {
            terminal_row++;
            terminal_col = 0;
            if (terminal_row >= TERM_ROWS) term_scroll();
            i++;
            continue;
        }
        
        if (str[i] == '\t') {
            // Tab to next 8-char boundary
            int spaces = 8 - (terminal_col % 8);
            for (int s = 0; s < spaces && terminal_col < TERM_COLS; s++) {
                term_buffer[terminal_row][terminal_col++] = ' ';
            }
            i++;
            continue;
        }
        
        term_buffer[terminal_row][terminal_col++] = str[i++];
    }
    term_buffer[terminal_row][terminal_col] = 0;
}

void term_clear() {
    for(int y=0; y<TERM_ROWS; y++)
        for(int x=0; x<=TERM_COLS; x++) term_buffer[y][x] = 0;
    terminal_row = 0;
    terminal_col = 0;
    term_print("camelos: "); 
    term_print(current_term_path); 
    term_print(" $ ");
}

void term_on_paint(window_t* win, int x, int y, int w, int h) {
    // Draw dark background with slight blue tint (modern terminal look)
    gfx_fill_rect(x, y, w, h, 0xFF1E1E2E);

    // Calculate how many chars/rows fit in the window
    int max_cols = (w - PAD*2) / CHAR_W;
    int max_rows = (h - PAD*2) / CHAR_H;
    if (max_cols > TERM_COLS) max_cols = TERM_COLS;
    if (max_rows > TERM_ROWS) max_rows = TERM_ROWS;

    // Draw text first (behind cursor)
    for(int r=0; r<max_rows && r<TERM_ROWS; r++) {
        if(term_buffer[r][0] != 0) {
            int len = 0;
            while(term_buffer[r][len] && len < max_cols) len++;
            for(int c = 0; c < len; c++) {
                // Color: prompt is accent colored, command input is bright white
                uint32_t ch_color = term_fg_color; // default gray for output
                if (r == terminal_row) {
                    // Current input line - find where prompt ends
                    // Format: "camelos: /path $ command"
                    // Everything up to and including "$ " is prompt
                    char* dollar = strchr(term_buffer[r], '$');
                    if (dollar) {
                        int prompt_end = (int)(dollar - term_buffer[r]) + 2; // Include "$ "
                        if (c < prompt_end) ch_color = 0xFF89B4FA;  // Purple for prompt
                        else ch_color = 0xFFFFFFFF;  // Bright white for command input
                    }
                }
                gfx_draw_char_scaled(x + PAD + c * CHAR_W, 
                                     y + PAD + r * CHAR_H, 
                                     term_buffer[r][c], ch_color, 1);
            }
        }
    }

    // Blinking cursor (drawn after text so it overlays)
    static int blink = 0; blink++;
    if(blink % 60 < 30) {
        if (terminal_row < max_rows && terminal_col < max_cols) {
            // Use a semi-transparent block cursor
            gfx_fill_rect(x + PAD + terminal_col * CHAR_W, 
                          y + PAD + terminal_row * CHAR_H, 
                          CHAR_W, CHAR_H, 0xFF89B4FA); // Purple cursor matching prompt
        }
    }
}

void execute_term_cmd() {
    char* line = term_buffer[terminal_row];
    char* cmd_ptr = strchr(line, '$');
    if (!cmd_ptr) { terminal_row++; return; }
    cmd_ptr += 2; 

    char cmd[32]={0}, arg[64]={0};
    int i=0, j=0;
    
    while(cmd_ptr[i] && cmd_ptr[i] != ' ') {
        if(j<31) cmd[j++] = cmd_ptr[i];
        i++;
    }
    if(cmd_ptr[i] == ' ') i++;
    
    j=0;
    while(cmd_ptr[i]) {
        if(j<63) arg[j++] = cmd_ptr[i];
        i++;
    }

    terminal_row++;
    if (terminal_row >= TERM_ROWS) term_scroll();
    terminal_col = 0;

    if (strcmp(cmd, "help") == 0) {
        term_print("Available commands:\n");
        term_print("  ls       - List directory contents\n");
        term_print("  cd       - Change directory\n");
        term_print("  pwd      - Print working directory\n");
        term_print("  clear    - Clear screen\n");
        term_print("  echo     - Print text\n");
        term_print("  curl     - Download file via HTTP\n");
        term_print("  open     - Open URL/app/DMG\n");
        term_print("  ping     - Ping a host\n");
        term_print("  exit     - Close terminal\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        term_clear();
        return; 
    }
    else if (strcmp(cmd, "pwd") == 0) {
        term_print(current_term_path);
    }
    else if (strcmp(cmd, "echo") == 0) {
        term_print(arg);
    }
    else if (strcmp(cmd, "ls") == 0) {
        char target_path[128];
        if (strlen(arg) > 0) {
            if(arg[0]=='/') strcpy(target_path, arg);
            else { 
                strcpy(target_path, current_term_path); 
                if(strcmp(current_term_path,"/")!=0) strcat(target_path,"/"); 
                strcat(target_path, arg); 
            }
        } else {
            strcpy(target_path, current_term_path);
        }

        uint32_t blk;
        extern int get_dir_block(const char*, uint32_t*);
        if(get_dir_block(target_path, &blk) != 0) {
            term_print("Dir not found.");
        } else {
            pfs32_direntry_t entries[8];
            int c = pfs32_listdir(blk, entries, 8);
            int col = 0;
            for(int k=0; k<c; k++) {
                int name_len = strlen(entries[k].filename);
                if(entries[k].attributes & 0x10) name_len++; // for /
                
                // Wrap to next line if not enough space
                if (col + name_len + 2 > TERM_COLS) {
                    term_print("\n");
                    col = 0;
                }
                
                term_print(entries[k].filename);
                if(entries[k].attributes & 0x10) term_print("/");
                term_print("  ");
                col += name_len + 2;
            }
        }
    }
    else if (strcmp(cmd, "cd") == 0) {
        if (strlen(arg) == 0) {
            term_print("Usage: cd <path>");
        } else {
            char new_path[128];
            if (strcmp(arg, "..") == 0) {
                strcpy(new_path, current_term_path);
                if (strcmp(new_path, "/") != 0) {
                    int len = strlen(new_path);
                    if (len > 1 && new_path[len-1] == '/') new_path[len-1] = 0;
                    char* last = strrchr(new_path, '/');
                    if (last && last != new_path) *last = 0;
                    else strcpy(new_path, "/");
                }
            } else {
                if (arg[0] == '/') strcpy(new_path, arg);
                else {
                    strcpy(new_path, current_term_path);
                    if (strcmp(current_term_path, "/") != 0) strcat(new_path, "/");
                    strcat(new_path, arg);
                }
            }
            
            if (sys_fs_is_dir(new_path)) {
                strcpy(current_term_path, new_path);
            } else {
                term_print("Invalid directory.");
            }
        }
    }
    else if (strcmp(cmd, "curl") == 0) {
        if (strlen(arg) == 0) {
            term_print("Usage: curl <url> [-o <file>]\n");
            term_print("Downloads a file via HTTP/HTTPS.");
        } else {
            // Parse URL and optional -o flag
            char curl_url[256] = {0};
            char curl_output[128] = {0};
            int curl_has_output = 0;

            // Extract URL (first token in arg)
            int ci = 0, cj = 0;
            while (arg[ci] && arg[ci] != ' ' && cj < 255) curl_url[cj++] = arg[ci++];
            curl_url[cj] = 0;

            // Skip whitespace, check for -o flag
            while (arg[ci] == ' ') ci++;
            if (arg[ci] == '-' && arg[ci+1] == 'o') {
                ci += 2;
                while (arg[ci] == ' ') ci++;
                cj = 0;
                while (arg[ci] && arg[ci] != ' ' && cj < 127) curl_output[cj++] = arg[ci++];
                curl_output[cj] = 0;
                curl_has_output = 1;
            }

            // Default to http:// if no scheme
            if (strstr(curl_url, "://") == NULL) {
                char prefixed[270];
                strcpy(prefixed, "http://");
                strcat(prefixed, curl_url);
                strncpy(curl_url, prefixed, 255);
                curl_url[255] = 0;
            }

            // Auto-detect filename if -o not specified
            if (!curl_has_output) {
                const char* last_slash = strrchr(curl_url, '/');
                const char* fname = last_slash ? last_slash + 1 : curl_url;
                int fname_len = strlen(fname);
                for (int k = 0; k < fname_len; k++) {
                    if (fname[k] == '?' || fname[k] == '#') { fname_len = k; break; }
                }
                if (fname_len == 0) {
                    strcpy(curl_output, "index.html");
                } else {
                    if (fname_len > 127) fname_len = 127;
                    memcpy(curl_output, fname, fname_len);
                    curl_output[fname_len] = 0;
                }

                // Prepend current directory
                char full_path[256];
                if (curl_output[0] == '/') {
                    strncpy(full_path, curl_output, 255);
                } else {
                    strcpy(full_path, current_term_path);
                    int plen = strlen(full_path);
                    if (plen > 1 && full_path[plen-1] != '/') strcat(full_path, "/");
                    strcat(full_path, curl_output);
                }
                strncpy(curl_output, full_path, 127);
                curl_output[127] = 0;
            } else if (curl_output[0] != '/') {
                char full_path[256];
                strcpy(full_path, current_term_path);
                int plen = strlen(full_path);
                if (plen > 1 && full_path[plen-1] != '/') strcat(full_path, "/");
                strcat(full_path, curl_output);
                strncpy(curl_output, full_path, 127);
                curl_output[127] = 0;
            }

            term_print("Downloading: ");
            term_print(curl_url);
            term_print("\n  Saving to: ");
            term_print(curl_output);
            term_print("\n");

            // Allocate response buffer on heap
            #define TERM_CURL_MAX 65536
            char* response = (char*)kmalloc(TERM_CURL_MAX);
            if (!response) {
                term_print("Error: Out of memory.\n");
            } else {
                memset(response, 0, TERM_CURL_MAX);
                int total_len = http_get_simple(curl_url, response, TERM_CURL_MAX);

                if (total_len <= 0) {
                    term_print("Error: Download failed.\n");
                } else {
                    // Skip HTTP headers
                    char* body = strstr(response, "\r\n\r\n");
                    int body_len = 0;
                    if (body) { body += 4; body_len = total_len - (body - response); }
                    else { body = response; body_len = total_len; }

                    int result = sys_fs_write(curl_output, body, body_len);
                    if (result >= 0) {
                        term_print("  Downloaded ");
                        char size_str[16];
                        int_to_str(body_len, size_str);
                        term_print(size_str);
                        term_print(" bytes -> ");
                        term_print(curl_output);
                        term_print("\n");

                        // Auto-install .app/.cdl/.dmg files
                        int outlen = strlen(curl_output);
                        if (outlen > 4) {
                            const char* ext = curl_output + outlen - 4;
                            if (strcmp(ext, ".cdl") == 0 || strcmp(ext, ".app") == 0) {
                                term_print("  Installable app detected. Installing...\n");
                                extern void desktop_install_app(const char*);
                                desktop_install_app(curl_output);
                                term_print("  Installation complete.\n");
                            } else if (strcmp(ext, ".dmg") == 0) {
                                term_print("  DMG image detected. Mounting...\n");
                                extern int app_installer_open_dmg(const char*);
                                app_installer_open_dmg(curl_output);
                            }
                        }
                    } else {
                        term_print("Error: Could not save file.\n");
                    }
                }
                kfree(response);
            }
        }
    }
    else if (strcmp(cmd, "open") == 0) {
        if (strlen(arg) == 0) {
            term_print("Usage: open <url|app|dmg>\n");
            term_print("  open http://example.com     - Open URL in browser\n");
            term_print("  open /Applications/Foo.app  - Launch application\n");
            term_print("  open ~/Downloads/app.dmg    - Mount DMG image\n");
        } else {
            // Resolve path
            char resolved[256] = {0};
            if (strncmp(arg, "~/", 2) == 0) {
                strcpy(resolved, "/home/user/");
                strcat(resolved, arg + 2);
            } else if (arg[0] != '/') {
                strcpy(resolved, current_term_path);
                int plen = strlen(resolved);
                if (plen > 1 && resolved[plen-1] != '/') strcat(resolved, "/");
                strcat(resolved, arg);
            } else {
                strncpy(resolved, arg, 255);
            }

            // URL detection
            if (strncmp(arg, "http://", 7) == 0 || strncmp(arg, "https://", 8) == 0) {
                term_print("Opening URL in browser: ");
                term_print(arg);
                term_print("\n");
                extern void init_browser_app_with_url(const char* url);
                init_browser_app_with_url(arg);
            }
            // .app bundle
            else if (strlen(resolved) > 4 && strcmp(resolved + strlen(resolved) - 4, ".app") == 0) {
                term_print("Launching app: ");
                term_print(resolved);
                term_print("\n");
                extern int wrap_exec(const char*);
                int result = wrap_exec(resolved);
                if (result < 0) term_print("Error: Could not launch app.\n");
            }
            // .dmg file
            else if (strlen(resolved) > 4 && strcmp(resolved + strlen(resolved) - 4, ".dmg") == 0) {
                if (!sys_fs_exists(resolved)) {
                    term_print("Error: DMG file not found.\n");
                } else {
                    term_print("Mounting DMG: ");
                    term_print(resolved);
                    term_print("\n");
                    extern int app_installer_open_dmg(const char*);
                    int result = app_installer_open_dmg(resolved);
                    if (result < 0) term_print("Error: Could not mount DMG.\n");
                }
            }
            // .cdl file
            else if (strlen(resolved) > 4 && strcmp(resolved + strlen(resolved) - 4, ".cdl") == 0) {
                term_print("Loading CDL app: ");
                term_print(resolved);
                term_print("\n");
                extern int sys_load_library(const char*);
                int handle = sys_load_library(resolved);
                if (handle >= 0) term_print("CDL app loaded.\n");
                else term_print("Error: Could not load CDL app.\n");
            }
            // Fallback: try to execute
            else {
                term_print("Launching: ");
                term_print(resolved);
                term_print("\n");
                extern int wrap_exec(const char*);
                int result = wrap_exec(resolved);
                if (result < 0) term_print("Error: Could not launch.\n");
            }
        }
    }
    else if (strcmp(cmd, "ping") == 0) {
        const char* target = (strlen(arg) > 0) ? arg : "8.8.8.8";
        term_print("Pinging ");
        term_print(target);
        term_print("...\n");

        for (int pi = 0; pi < 4; pi++) {
            char ping_buf[128];
            memset(ping_buf, 0, 128);
            int status = sys_net_ping(target, ping_buf, 128);
            if (status >= 0) {
                term_print(ping_buf);
                term_print("\n");
            } else {
                term_print("Ping failed.\n");
            }
            sys_delay(200);
        }
        term_print("Ping complete.\n");
    }
    else if (strlen(cmd) > 0) {
        term_print("Unknown command: ");
        term_print(cmd);
        term_print("\nType 'help' for available commands.");
    }

    if(terminal_col != 0) {
        terminal_row++;
        if (terminal_row >= TERM_ROWS) term_scroll();
    }
    
    terminal_col = 0;
    term_print("camelos: ");
    term_print(current_term_path);
    term_print(" $ ");
}

void term_on_input(window_t* win, int key) {
    if(key == 0) return;
    if(key == '\n') { execute_term_cmd(); }
    else if (key == '\b') {
        int prompt_len = 11 + strlen(current_term_path); 
        if(terminal_col > prompt_len) {
            terminal_col--;
            term_buffer[terminal_row][terminal_col] = 0;
        }
    }
    // Arrow keys for terminal
    else if (key == 128) { /* KEY_UP - future: command history */ }
    else if (key == 129) { /* KEY_DOWN - future: command history */ }
    else if (key == 130) { /* KEY_LEFT */
        int prompt_len = 11 + strlen(current_term_path);
        if(terminal_col > prompt_len) terminal_col--;
    }
    else if (key == 131) { /* KEY_RIGHT */
        if(terminal_col < TERM_COLS - 1 && term_buffer[terminal_row][terminal_col] != 0) terminal_col++;
    }
    // Ignore other special keys (128-159 range includes arrow keys, F-keys, etc.)
    else if (key >= 32 && key != 127 && key < 128) {
        // Accept only ASCII printable chars (32-126)
        if (terminal_col < TERM_COLS - 1) {
            term_buffer[terminal_row][terminal_col] = (char)key;
            term_buffer[terminal_row][terminal_col+1] = 0;
            terminal_col++;
        }
    }
    else if (key >= 160) {
        // Accept Latin-1 Supplement chars (accented chars like á,é,ç etc.)
        // These are produced by dead key composition
        if (terminal_col < TERM_COLS - 1) {
            term_buffer[terminal_row][terminal_col] = (char)key;
            term_buffer[terminal_row][terminal_col+1] = 0;
            terminal_col++;
        }
    }
}

void term_on_scroll(window_t* win, int delta) {
    // Scroll the terminal buffer
    for (int i = 0; i < (delta > 0 ? delta : -delta); i++) {
        if (delta > 0) {
            // Scroll up - show older content (we can't go back, so just move viewport)
            // For now, treat scroll up as no-op since we don't have scrollback buffer
        } else {
            // Scroll down - just add a blank line at bottom
            term_scroll();
        }
    }
}

void init_terminal_app() {
    term_reset();
    Window* w = fw_create_window("Terminal", 550, 320, term_on_paint, term_on_input, 0);
    w->min_w = 300;
    w->scroll_callback = (void*)term_on_scroll;
    
    w->menu_count = 2;
    strcpy(w->menus[0].name, "Shell");
    strcpy(w->menus[0].items[0].label, "Clear");
    strcpy(w->menus[0].items[1].label, "Close");
    w->menus[0].item_count = 2;
    
    strcpy(w->menus[1].name, "Edit");
    strcpy(w->menus[1].items[0].label, "Copy");
    strcpy(w->menus[1].items[1].label, "Paste");
    w->menus[1].item_count = 2;

    fw_register_dock("Term", 0, w);
}
