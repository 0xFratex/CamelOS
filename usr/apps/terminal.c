#include "../usr/framework.h"
#include "../core/string.h"
#include "../../sys/api.h"
#include "../../hal/video/gfx_hal.h"

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

void term_on_paint(int x, int y, int w, int h) {
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
                // Color prompt differently from command output
                uint32_t ch_color = term_fg_color;
                // Check if this is the prompt line (contains "$ ")
                char* dollar = strchr(term_buffer[r], '$');
                if (dollar && r == terminal_row) {
                    // Characters before and including $ are accent colored
                    int prompt_end = (int)(dollar - term_buffer[r]) + 1;
                    if (c < prompt_end) ch_color = 0xFF89B4FA; // Purple for prompt
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

void term_on_input(int key) {
    if(key == 0) return;
    if(key == '\n') { execute_term_cmd(); }
    else if (key == '\b') {
        int prompt_len = 11 + strlen(current_term_path); 
        if(terminal_col > prompt_len) {
            terminal_col--;
            term_buffer[terminal_row][terminal_col] = 0;
        }
    }
    else if (key >= 32 && key <= 126) {
        if(terminal_col < TERM_COLS - 1) {
            term_buffer[terminal_row][terminal_col] = (char)key;
            term_buffer[terminal_row][terminal_col+1] = 0;
            terminal_col++;
        }
    }
}

void init_terminal_app() {
    term_reset();
    Window* w = fw_create_window("Terminal", 550, 320, term_on_paint, term_on_input, 0);
    w->min_w = 300;
    
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
