#include "../sys/api.h"
#include "../core/string.h"
#include "../core/memory.h"
#include <string.h>

// CDL loader declaration
extern int sys_load_library(const char* path);

// Watermark allocator externs
extern unsigned int k_get_heap_mark();
extern void k_rewind_heap(unsigned int m);

// Simple file concatenation
void cmd_cat(const char* arg) {
    if(strlen(arg) == 0) { sys_print("Usage: cat <file> or cat >> <file>\n"); return; }

    // Handle Append Mode ">> filename"
    if (strncmp(arg, ">> ", 3) == 0) {
        char* filename = (char*)arg + 3;
        while(*filename == ' ') filename++; // skip spaces
        
        sys_print("Interactive Append Mode (Type text, press Ctrl+D or ~ to save):\n");
        
        // 1. Read existing content
        char* file_buf = (char*)kmalloc(4096); // 4KB limit for this demo
        if (!file_buf) {
            sys_print("Error: OOM\n");
            return;
        }
        memset(file_buf, 0, 4096);
        
        int current_size = 0;
        if (sys_fs_exists(filename)) {
            current_size = sys_fs_read(filename, file_buf, 4095);
            if(current_size < 0) current_size = 0;
        } else {
            sys_fs_create(filename, 0); // Create file
        }

        // 2. Input loop
        int pos = current_size;
        while(pos < 4095) {
            char c = sys_wait_key();
            if (c == '~') break; // EOF simulation
            
            char t[2] = {c, 0};
            sys_print(t);
            
            if (c == '\b') {
                if(pos > current_size) pos--; 
            } else {
                file_buf[pos++] = c;
            }
            if (c == '\n') { /* Handle visual newline handled by sys_print */ }
        }
        
        // 3. Write back
        sys_fs_write(filename, file_buf, pos);
        sys_print("\nSaved.\n");
        kfree(file_buf); // Assuming kfree exists or simple heap
    } 
    // Read Mode
    else {
        char* buf = (char*)kmalloc(2048);
        if (!buf) {
            sys_print("Error: Out of memory.\n");
            return;
        }
        memset(buf, 0, 2048);
        int len = sys_fs_read(arg, buf, 2047);
        if (len >= 0) {
            sys_print(buf);
            sys_print("\n");
        } else {
            sys_print("File not found or error.\n");
        }
        kfree(buf);
    }
}

char current_path[128];

// Fix logic to prevent "//" or trailing slashes on file paths
void update_path(const char* new_part) {
    // Special case: Root
    if (strcmp(new_part, "/") == 0) {
        strcpy(current_path, "/");
        return;
    }

    // Handle ".."
    if (strcmp(new_part, "..") == 0) {
        if (strcmp(current_path, "/") == 0) return;
        
        int len = strlen(current_path);
        // Remove trailing slash if present (safety)
        if (len > 1 && current_path[len-1] == '/') {
            current_path[len-1] = 0;
            len--;
        }
        // Find last separator
        while(len > 0 && current_path[len] != '/') {
            len--;
        }
        
        if (len == 0) { // Back to root
            strcpy(current_path, "/");
        } else {
            current_path[len] = 0; // Cut at the slash
        }
    } else {
        // Regular cd <folder>
        int len = strlen(current_path);
        if (len > 1 && current_path[len-1] != '/') strcat(current_path, "/");
        else if (len == 0) strcpy(current_path, "/");
        
        // Don't append slash at root if path is just "/"
        if(strcmp(current_path, "/") != 0 && current_path[strlen(current_path)-1] != '/')
             strcat(current_path, "/");
             
        strcat(current_path, new_part);
    }
}

void get_abs_path(const char* filename, char* dest) {
    if (filename[0] == '/') {
        strcpy(dest, filename);
    } else {
        strcpy(dest, current_path);
        // Add slash if not at root (root is "/")
        int len = strlen(dest);
        if (len > 1 && dest[len-1] != '/') strcat(dest, "/");
        else if (len == 0) strcpy(dest, "/");

        if (len == 1 && dest[0] == '/') { /* Do nothing, it's root */ }
        else if (dest[len-1] != '/') strcat(dest, "/");

        strcat(dest, filename);
    }
}

extern void start_bubble_view();

// Helper to determine if a path ends in .app
int is_app_bundle(const char* filename) {
    int len = strlen(filename);
    if (len > 4 && strcmp(filename + len - 4, ".app") == 0) return 1;
    return 0;
}

void execute_program(const char* path) {
    char binary_path[128];
    memset(binary_path, 0, 128);
    
    if (is_app_bundle(path)) {
        sys_print("Launching App: "); sys_print(path); sys_print("\n");

        // Flat binary format: /usr/apps/Name.app -> /usr/apps/Name.cdl
        strncpy(binary_path, path, 128);
        
        // Remove .app extension
        int len = strlen(binary_path);
        if (len > 4 && strcmp(binary_path + len - 4, ".app") == 0) {
            binary_path[len - 4] = '\0';
        }
        
        // Append .cdl extension
        strcat(binary_path, ".cdl");
    } else {
        strcpy(binary_path, path);
    }

    // Try to load it as a CDL (Dynamic App)
    sys_print("Loading executable: "); sys_print(binary_path); sys_print("\n");
    
    int handle = sys_load_library(binary_path);
    if (handle >= 0) {
        sys_print("App loaded successfully (Handle "); 
        char n[4]; int_to_str(handle, n); sys_print(n); 
        sys_print(")\n");
        // The CDL entry point (cdl_main) was already called by sys_load_library.
        // That entry point typically creates the window and registers callbacks.
    } else {
        sys_print("Failed to execute. File not found or invalid format.\n");
    }
}

void shell_main() {
    char cmd_buffer[128];
    int pos = 0;
    strcpy(current_path, "/");

    sys_print("\nCamel OS Shell v2.1 (Stable)\n");

    while (1) {
        sys_print("user@camel:");
        sys_print(current_path);
        sys_print("$ ");

        pos = 0;
        memset(cmd_buffer, 0, 128); // Ensure clean buffer

        while(1) {
            char c = sys_wait_key();
            if(c == '\n') { sys_print("\n"); break; }
            else if(c == '\b') {
                if(pos > 0) { pos--; sys_print("\b \b"); }
            }
            else if(c != 0 && pos < 127) {
                cmd_buffer[pos++] = c;
                char t[2] = {c,0};
                sys_print(t);
            }
        }

        // 1. Mark heap before processing command
        unsigned int mark = k_get_heap_mark();

        // Parser
        char cmd[32]={0}, arg1[64]={0};
        int i=0, j=0;
        while(i<pos && cmd_buffer[i] != ' ') cmd[j++] = cmd_buffer[i++];
        if(i<pos) i++;
        j=0;
        while(i<pos && cmd_buffer[i] != ' ') arg1[j++] = cmd_buffer[i++];

        if (strcmp(cmd, "ls") == 0) {
            // Pass directory to list, or current path
            if (strlen(arg1) > 0) sys_fs_ls(arg1);
            else sys_fs_ls(current_path);
        }
        else if (strcmp(cmd, "cd") == 0) {
            if(strlen(arg1) == 0) {
                 sys_print("Usage: cd <path>\n"); continue;
            }

            char abs_path[128];
            get_abs_path(arg1, abs_path);

            // Fix path logic
            if(strcmp(arg1, ".") == 0) continue;

            // Verify existence BEFORE string manipulation
            if(sys_fs_exists(abs_path) && sys_fs_is_dir(abs_path)) {
                // Handle ".." manually or use update_path
                if(strcmp(arg1, "..") == 0) {
                    // Remove last component from current_path
                     int len = strlen(current_path);
                     if(len > 1) {
                         if(current_path[len-1] == '/') { current_path[len-1]=0; len--; }
                         char* last = strrchr(current_path, '/');
                         if(last && last != current_path) *last = 0;
                         else strcpy(current_path, "/");
                     }
                } else {
                    // Regular cd
                    strcpy(current_path, abs_path);
                }
            } else {
                sys_print("Invalid directory.\n");
            }
        }
        else if(strcmp(cmd, "cat") == 0) {
            // Special parser for ">>" because arg1 might split at space
            // We want the raw rest of buffer after "cat "
            char* raw_args = strstr(cmd_buffer, "cat ");
            if(raw_args) {
                raw_args += 4;
                cmd_cat(raw_args);
            }
        }
        else if (strcmp(cmd, "gui") == 0) {
            sys_clear();
            start_bubble_view();
        }
        else if (strcmp(cmd, "clear") == 0) {
            sys_clear();
        }
        else if (strcmp(cmd, "help") == 0) {
            sys_print("cmds: ls, cd, cat, gui, reboot, ./<file>, run <app>, loadtest, ping\n");
        }
        else if (strcmp(cmd, "./") == 0 || strcmp(cmd, "run") == 0) {
            // Execute program/bundle
            if (strlen(arg1) > 0) {
                execute_program(arg1);
            } else {
                sys_print("Usage: ./<file> or run <program>\n");
            }
        }
        else if (strcmp(cmd, "loadtest") == 0) {
            sys_print("=== CDL Dynamic Library Test ===\n");
            
            // Initialize CDL system if not already done
            sys_cdl_init_system();
            
            // Load the math library
            sys_print("Loading /usr/lib/math.cdl ...\n");
            int handle = sys_load_library("/usr/lib/math.cdl");
            
            if (handle >= 0) {
                sys_print("Library loaded successfully!\n");
                
                // Test the add function
                typedef int (*math_func)(int, int);
                math_func add_func = (math_func)sys_get_proc_address(handle, "add");
                math_func mul_func = (math_func)sys_get_proc_address(handle, "mul");
                
                // For is_even, use the correct signature
                typedef int (*is_even_func_t)(int);
                is_even_func_t is_even_func = (is_even_func_t)sys_get_proc_address(handle, "is_even");
                
                if (add_func) {
                    int result = add_func(10, 20);
                    char res_str[16];
                    int_to_str(result, res_str);
                    sys_print("10 + 20 = "); sys_print(res_str); sys_print("\n");
                } else {
                    sys_print("Error: Could not find 'add' function\n");
                }
                
                if (mul_func) {
                    int result = mul_func(5, 8);
                    char res_str[16];
                    int_to_str(result, res_str);
                    sys_print("5 * 8 = "); sys_print(res_str); sys_print("\n");
                } else {
                    sys_print("Error: Could not find 'mul' function\n");
                }
                
                if (is_even_func) {
                    int result = is_even_func(42);
                    char res_str[16];
                    int_to_str(result, res_str);
                    sys_print("Is 42 even? "); sys_print(res_str); sys_print("\n");
                } else {
                    sys_print("Error: Could not find 'is_even' function\n");
                }
                
                // Unload the library
                sys_unload_library(handle);
                sys_print("Library unloaded.\n");
            } else {
                sys_print("Failed to load library. Make sure /usr/lib/math.cdl exists.\n");
            }
            sys_print("=== Test Complete ===\n");
        }
        else if (strcmp(cmd, "ping") == 0) {
            char result_buf[128];
            const char* target = (strlen(arg1) > 0) ? arg1 : "8.8.8.8";

            sys_print("Pinging "); sys_print(target); sys_print("...\n");

            // Send 4 pings
            for(int i=0; i<4; i++) {
                memset(result_buf, 0, 128);
                int status = sys_net_ping(target, result_buf, 128);
                if(status >= 0) {
                    sys_print(result_buf);
                } else {
                    sys_print("Ping failed.\n");
                }
                // Small delay between pings
                sys_delay(200);
            }
            sys_print("Ping complete.\n");
        }
        else if(strlen(cmd) > 0) {
            sys_print("Unknown command.\n");
        }

        // 2. Rewind heap after command finishes.
        // This effectively "frees" all memory allocated during the command execution.
        // WARNING: Do not use this if you launched a background window/task!
        k_rewind_heap(mark);
    }
}
