// usr/apps/terminal_cdl.c
#include "../../sys/cdl_defs.h"
#include "../lib/camel_framework.h"

static kernel_api_t* sys = 0;

#define TERM_ROWS 25
#define TERM_COLS 80
#define CHAR_W 6
#define CHAR_H 10

char term_buffer[TERM_ROWS][TERM_COLS + 1];
int cur_row = 0;
int cur_col = 0;
int blink_state = 0;

// Command buffer state
char cmd_line[128];
int cmd_pos = 0;
char current_path[128] = "/";

void term_scroll() {
    for(int y = 0; y < TERM_ROWS - 1; y++) {
        sys->memcpy(term_buffer[y], term_buffer[y+1], TERM_COLS);
        // Ensure null term
        term_buffer[y][TERM_COLS] = 0;
    }
    sys->memset(term_buffer[TERM_ROWS-1], 0, TERM_COLS);
    cur_row = TERM_ROWS - 1;
}

void term_print(const char* str) {
    while(*str) {
        char c = *str++;
        if (c == '\n') {
            cur_row++; cur_col = 0;
            if (cur_row >= TERM_ROWS) term_scroll();
        } else {
            if (cur_col >= TERM_COLS) {
                cur_row++; cur_col = 0;
                if (cur_row >= TERM_ROWS) term_scroll();
            }
            term_buffer[cur_row][cur_col++] = c;
        }
    }
}

void term_prompt() {
    term_print("user@camel:");
    term_print(current_path);
    term_print("$ ");
}

void execute() {
    term_print("\n");
    
    // Simple parser
    if (sys->strcmp(cmd_line, "help") == 0) {
        term_print(" Camel OS Terminal\n Commands: help, clear, ls, cd, exit\n");
    } 
    else if (sys->strcmp(cmd_line, "clear") == 0) {
        for(int i=0; i<TERM_ROWS; i++) sys->memset(term_buffer[i], 0, TERM_COLS);
        cur_row = 0; cur_col = 0;
        // Prompt will be reprinted
        term_prompt();
        cmd_pos = 0;
        sys->memset(cmd_line, 0, 128);
        return;
    }
    else if (sys->strcmp(cmd_line, "exit") == 0) {
        sys->exit(); 
        return;
    }
    else if (sys->strcmp(cmd_line, "ls") == 0) {
        // Simple LS simulation (In real app, use fs_list)
        term_print("Listing "); term_print(current_path); term_print(":\n");
        // TODO: Implement fs_list integration here
        term_print(" (Directory listing not fully linked in this demo)\n");
    }
    else if (sys->strlen(cmd_line) > 0) {
        term_print(" Command not found: ");
        term_print(cmd_line);
        term_print("\n");
    }

    term_prompt();
    cmd_pos = 0;
    sys->memset(cmd_line, 0, 128);
}

void on_input(int key) {
    if (key == '\n') {
        execute();
    }
    else if (key == '\b') {
        if (cmd_pos > 0) {
            cmd_pos--;
            cmd_line[cmd_pos] = 0;
            // Remove from buffer
            if (cur_col > 0) { 
                cur_col--;
                term_buffer[cur_row][cur_col] = 0;
            }
        }
    }
    else if (key >= 32 && key <= 126 && cmd_pos < 127) {
        cmd_line[cmd_pos++] = (char)key;
        // Add to buffer
        if (cur_col < TERM_COLS) {
            term_buffer[cur_row][cur_col++] = (char)key;
        }
    }
}

void on_paint(int x, int y, int w, int h) {
    sys->draw_rect(x, y, w, h, 0xFF101010); // Dark BG

    blink_state++;

    for(int r=0; r<TERM_ROWS; r++) {
        if(term_buffer[r][0]) {
            sys->draw_text(x+4, y+4+(r*CHAR_H), term_buffer[r], 0xFFEEEEEE);
        }
    }

    // Cursor
    if ((blink_state % 20) < 10) {
        sys->draw_rect(x+4 + (cur_col*CHAR_W), y+4 + (cur_row*CHAR_H), CHAR_W, CHAR_H, 0xFF00FF00);
    }
}

void menu_cb(int m, int i) {
    if (m == 0 && i == 0) {
        // Clear
        for(int i=0; i<TERM_ROWS; i++) sys->memset(term_buffer[i], 0, TERM_COLS);
        cur_row = 0; cur_col = 0;
        term_prompt();
    }
}

static cdl_exports_t exports = { .lib_name = "Terminal", .version = 10 };

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;
    for(int i=0; i<TERM_ROWS; i++) sys->memset(term_buffer[i], 0, TERM_COLS);

    // Arg handling
    char args[256];
    sys->get_launch_args(args, 256);
    
    // Default path
    sys->strcpy(current_path, "/home");

    // Check if opened with arguments
    if(sys->strlen(args) > 0) {
        // Check if directory
        // fs_exists is mapped, fs_is_dir might not be directly in struct if not updated
        // But we can check via stat logic wrapper or just try listing
        // Assuming file structure: if it doesn't end in /, treat as file?
        // Actually, let's assume if it has no extension, it's a dir for now, or rely on API.
        
        // Simple logic: If user right-clicks Folder -> Open with Terminal, we want to CD there.
        // If user right-clicks File -> Open with Terminal, we want to view it?
        
        // For now, let's treat it as a path to switch to.
        sys->strcpy(current_path, args);
        
        term_print("Directory changed to: ");
        term_print(current_path);
        term_print("\n");
    } else {
        term_print("Camel OS Terminal v1.0\n");
    }

    term_prompt();

    void* win = sys->create_window("Terminal", 500, 300, on_paint, on_input, 0);

    static menu_def_t menus[1];
    sys->strcpy(menus[0].name, "Shell");
    sys->strcpy(menus[0].items[0].label, "Clear");
    menus[0].item_count = 1;
    sys->set_window_menu(win, menus, 1, menu_cb);

    return &exports;
}