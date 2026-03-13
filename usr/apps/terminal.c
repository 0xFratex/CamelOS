#include "../usr/framework.h"
#include "../core/string.h"
#include "../../sys/api.h" 
// api.h includes pfs32.h, so pfs32_direntry_t and pfs32_listdir are known.

#define TERM_COLS 33 
#define TERM_ROWS 15

char term_buffer[TERM_ROWS][50]; 
int terminal_row = 0;
int terminal_col = 0;
char current_term_path[64] = "/";

void term_reset() {
    for(int y=0; y<TERM_ROWS; y++) {
        for(int x=0; x<50; x++) term_buffer[y][x] = 0;
    }
    terminal_row = 0;
    terminal_col = 0;
    strcpy(current_term_path, "/");
    
    char prompt[] = "camel@pro: /$ ";
    for(int i=0; i<strlen(prompt); i++) {
        term_buffer[0][i] = prompt[i];
    }
    terminal_col = strlen(prompt);
}

void term_scroll() {
    for(int r=0; r < TERM_ROWS-1; r++) {
        strcpy(term_buffer[r], term_buffer[r+1]);
    }
    for(int i=0; i<50; i++) term_buffer[TERM_ROWS-1][i] = 0;
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
        
        term_buffer[terminal_row][terminal_col++] = str[i++];
    }
}

void term_clear() {
    for(int y=0; y<15; y++)
        for(int x=0; x<50; x++) term_buffer[y][x] = 0;
    terminal_row = 0;
    terminal_col = 0;
    term_print("camel@pro: "); 
    term_print(current_term_path); 
    term_print("$ ");
}

void term_on_paint(int x, int y, int w, int h) {
    extern void fw_draw_rect(int,int,int,int,int);
    extern void fw_draw_text_clipped(int,int,const char*,int,int);

    fw_draw_rect(x, y, w, h, 0); 

    static int blink = 0; blink++;
    if(blink % 60 < 30) {
        fw_draw_rect(x + 4 + (terminal_col * 6), y + 4 + (terminal_row * 10), 7, 9, 2);
    }

    for(int r=0; r<TERM_ROWS; r++) {
        if(term_buffer[r][0] != 0) {
            fw_draw_text_clipped(x + 4, y + 4 + (r * 10), term_buffer[r], 15, w - 4);
        }
    }
}

void execute_term_cmd() {
    char* line = term_buffer[terminal_row];
    char* cmd_ptr = strchr(line, '$');
    if (!cmd_ptr) { terminal_row++; return; }
    cmd_ptr += 2; 

    char cmd[16]={0}, arg[32]={0};
    int i=0, j=0;
    
    while(cmd_ptr[i] && cmd_ptr[i] != ' ') {
        if(j<15) cmd[j++] = cmd_ptr[i];
        i++;
    }
    if(cmd_ptr[i] == ' ') i++;
    
    j=0;
    while(cmd_ptr[i]) {
        if(j<31) arg[j++] = cmd_ptr[i];
        i++;
    }

    terminal_row++;
    if (terminal_row >= 15) term_scroll();
    terminal_col = 0;

    if (strcmp(cmd, "help") == 0) {
        term_print("Available: ls, cd, clear, exit");
    }
    else if (strcmp(cmd, "clear") == 0) {
        term_clear();
        return; 
    }
    else if (strcmp(cmd, "ls") == 0) {
        char target_path[64];
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
            // Use packed struct or local array, then CAST to expected pointer type.
            // NOTE: This relies on 'struct { ... }' being binary compatible 
            // with pfs32_direntry_t. Ideally, we would use pfs32_direntry_t directly.
            // Since we included api.h, we HAVE pfs32_direntry_t!
            
            pfs32_direntry_t entries[8]; // Clean usage
            int c = pfs32_listdir(blk, entries, 8);
            
            for(int k=0; k<c; k++) {
                term_print(entries[k].filename);
                if(entries[k].attributes & 0x10) term_print("/");
                term_print("  ");
            }
        }
    }
    else if (strcmp(cmd, "cd") == 0) {
        if (strlen(arg) == 0) {
            term_print("Usage: cd <path>");
        } else {
            char new_path[64];
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
        term_print("Unknown command.");
    }

    if(terminal_col != 0) {
        terminal_row++;
        if (terminal_row >= 15) term_scroll();
    }
    
    terminal_col = 0;
    term_print("camel@pro: ");
    term_print(current_term_path);
    term_print("$ ");
}

void term_on_input(int key) {
    if(key == 0) return;
    if(key == '\n') { execute_term_cmd(); }
    else if (key == '\b') {
        int prompt_len = 13 + strlen(current_term_path); 
        if(terminal_col > prompt_len) {
            terminal_col--;
            term_buffer[terminal_row][terminal_col] = 0;
        }
    }
    else if (key >= 32 && key <= 126) {
        if(terminal_col < TERM_COLS) {
            term_buffer[terminal_row][terminal_col] = (char)key;
            term_buffer[terminal_row][terminal_col+1] = 0;
            terminal_col++;
        }
    }
}

void init_terminal_app() {
    term_reset();
    Window* w = fw_create_window("Terminal", 220, 150, term_on_paint, term_on_input, 0);
    // w->is_resizable = 1; // Not supported in window_t
    w->min_w = 150;
    
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