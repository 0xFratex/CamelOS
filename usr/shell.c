#include "../sys/api.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../core/http.h"
#include "../core/dns.h"
#include "../core/package_manager.h"
#include "../core/app_registry.h"
#include "../core/sys_dirs.h"
#include <string.h>
#include "../core/scheduler.h"
#include "../core/process.h"
#include "../core/signal.h"
#include "../fs/vfs.h"
#include "../core/theme.h"

// CDL loader declaration
extern int sys_load_library(const char* path);

// Watermark allocator externs
extern unsigned int k_get_heap_mark();
extern void k_rewind_heap(unsigned int m);

// App installer for DMG mounting
extern int app_installer_open_dmg(const char* dmg_path);

// Desktop installer for .app/.cdl auto-install
extern void desktop_install_app(const char* source_path);

// System info externs
extern uint32_t k_get_free_mem(void);
extern uint32_t k_get_total_mem(void);
extern uint32_t timer_ticks;

// Theme externs
extern void theme_set(int theme_id);
extern int theme_get_id(void);

// Disk/filesystem externs
extern void disk_flush_cache(void);
extern uint32_t disk_total_blocks;

// stdlib
extern int atoi(const char* str);

// Helper: print an integer with zero-padded width
static void print_zpad(int val, int width) {
    if (val < 0) { sys_print("-"); val = -val; }
    char tmp[16];
    int_to_str(val, tmp);
    int len = strlen(tmp);
    for (int i = len; i < width; i++) sys_print("0");
    sys_print(tmp);
}

// Helper: print an integer right-aligned with spaces
static void print_rpad(int val, int width) {
    char tmp[16];
    int_to_str(val, tmp);
    int len = strlen(tmp);
    for (int i = len; i < width; i++) sys_print(" ");
    sys_print(tmp);
}

// Environment variables (in-memory, not persistent)
#define MAX_ENV_VARS 32
#define MAX_ENV_NAME 32
#define MAX_ENV_VALUE 128
static char env_names[MAX_ENV_VARS][MAX_ENV_NAME];
static char env_values[MAX_ENV_VARS][MAX_ENV_VALUE];
static int env_count = 0;

// Command history
#define MAX_HISTORY 50
static char cmd_history[MAX_HISTORY][128];
static int history_count = 0;
static int history_pos = 0;

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

// ============================================================================
// curl command - Download files via HTTP/HTTPS
// Usage:
//   curl <url>                  - Download and auto-detect filename from URL
//   curl <url> -o <filename>    - Download and save as <filename>
// ============================================================================

void cmd_curl(const char* args) {
    if (strlen(args) == 0) {
        sys_print("Usage: curl <url> [-o <filename>]\n");
        sys_print("  Downloads a file via HTTP/HTTPS.\n");
        sys_print("  Without -o, saves to current directory using URL filename.\n");
        return;
    }

    // Parse: url and optional -o filename
    char url[256] = {0};
    char output_path[128] = {0};
    int has_output = 0;

    // Extract URL (first token)
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 255) url[j++] = args[i++];
    url[j] = 0;

    // Skip whitespace
    while (args[i] == ' ') i++;

    // Check for -o flag
    if (args[i] == '-' && args[i+1] == 'o') {
        i += 2;
        while (args[i] == ' ') i++;
        j = 0;
        while (args[i] && args[i] != ' ' && j < 127) output_path[j++] = args[i++];
        output_path[j] = 0;
        has_output = 1;
    }

    if (strlen(url) == 0) {
        sys_print("Error: No URL specified.\n");
        return;
    }

    // Validate URL has a scheme
    if (strstr(url, "://") == NULL) {
        // Default to http:// if no scheme provided
        char prefixed_url[270];
        strcpy(prefixed_url, "http://");
        strcat(prefixed_url, url);
        strncpy(url, prefixed_url, 255);
        url[255] = 0;
    }

    // Auto-detect filename from URL if -o not specified
    if (!has_output) {
        // Find the last path component after the final '/'
        const char* last_slash = strrchr(url, '/');
        const char* fname = last_slash ? last_slash + 1 : url;

        // Strip query string if present
        int fname_len = strlen(fname);
        for (int k = 0; k < fname_len; k++) {
            if (fname[k] == '?' || fname[k] == '#') { fname_len = k; break; }
        }

        if (fname_len == 0) {
            // No filename in URL, use "index.html"
            strcpy(output_path, "index.html");
        } else {
            if (fname_len > 127) fname_len = 127;
            memcpy(output_path, fname, fname_len);
            output_path[fname_len] = 0;
        }

        // Prepend current directory
        char full_path[256];
        if (output_path[0] == '/') {
            strncpy(full_path, output_path, 255);
        } else {
            strcpy(full_path, current_path);
            int plen = strlen(full_path);
            if (plen > 1 && full_path[plen-1] != '/') {
                strcat(full_path, "/");
            }
            strcat(full_path, output_path);
        }
        strncpy(output_path, full_path, 127);
        output_path[127] = 0;
    } else if (output_path[0] != '/') {
        // Relative path - prepend current directory
        char full_path[256];
        strcpy(full_path, current_path);
        int plen = strlen(full_path);
        if (plen > 1 && full_path[plen-1] != '/') {
            strcat(full_path, "/");
        }
        strcat(full_path, output_path);
        strncpy(output_path, full_path, 127);
        output_path[127] = 0;
    }

    sys_print("Downloading: ");
    sys_print(url);
    sys_print("\n  Saving to: ");
    sys_print(output_path);
    sys_print("\n");

    // Allocate response buffer (64KB max, on heap to avoid stack overflow)
    #define CURL_MAX_RESPONSE 65536
    char* response = (char*)kmalloc(CURL_MAX_RESPONSE);
    if (!response) {
        sys_print("Error: Out of memory for download buffer.\n");
        return;
    }
    memset(response, 0, CURL_MAX_RESPONSE);

    sys_print("  Resolving host...\n");

    // Use the kernel's http_get_simple() for the download
    int total_len = http_get_simple(url, response, CURL_MAX_RESPONSE);

    if (total_len <= 0) {
        sys_print("Error: Download failed.\n");
        sys_print("  Possible causes: DNS failure, connection refused, timeout.\n");
        kfree(response);
        return;
    }

    // Find the body (skip HTTP headers)
    char* body = strstr(response, "\r\n\r\n");
    int body_len = 0;
    if (body) {
        body += 4;
        body_len = total_len - (body - response);
    } else {
        body = response;
        body_len = total_len;
    }

    // Write body to file
    int result = sys_fs_write(output_path, body, body_len);

    if (result >= 0) {
        sys_print("  Downloaded ");
        char size_str[16];
        int_to_str(body_len, size_str);
        sys_print(size_str);
        sys_print(" bytes -> ");
        sys_print(output_path);
        sys_print("\n");

        // Check if the file is an installable app (.app, .cdl, .dmg)
        int outlen = strlen(output_path);
        if (outlen > 4) {
            const char* ext = output_path + outlen - 4;
            if (strcmp(ext, ".cdl") == 0 || strcmp(ext, ".app") == 0) {
                sys_print("  Installable app detected. Installing...\n");
                desktop_install_app(output_path);
                sys_print("  Installation complete.\n");
            } else if (strcmp(ext, ".dmg") == 0) {
                sys_print("  DMG image detected. Mounting...\n");
                app_installer_open_dmg(output_path);
            }
        }
    } else {
        sys_print("Error: Could not save file to disk.\n");
    }

    kfree(response);
}

// ============================================================================
// open command - Open URLs in browser, launch apps, mount DMGs
// Usage:
//   open http://example.com       - Opens URL in browser
//   open https://example.com      - Opens URL in browser (HTTPS)
//   open /Applications/Foo.app    - Launches an app
//   open ~/Downloads/app.dmg      - Mounts a DMG
// ============================================================================

// Forward declaration
void execute_program(const char* path);

void cmd_open(const char* args) {
    if (strlen(args) == 0) {
        sys_print("Usage: open <url|app|dmg>\n");
        sys_print("  open http://example.com     - Open URL in browser\n");
        sys_print("  open /Applications/Foo.app  - Launch application\n");
        sys_print("  open ~/Downloads/app.dmg    - Mount DMG image\n");
        return;
    }

    // Resolve path (handle ~/ prefix)
    char resolved_path[256] = {0};
    if (strncmp(args, "~/", 2) == 0) {
        // Expand ~ to /home/user
        strcpy(resolved_path, "/home/user/");
        strcat(resolved_path, args + 2);
    } else if (args[0] != '/') {
        // Relative path - prepend current directory
        strcpy(resolved_path, current_path);
        int plen = strlen(resolved_path);
        if (plen > 1 && resolved_path[plen-1] != '/') {
            strcat(resolved_path, "/");
        }
        strcat(resolved_path, args);
    } else {
        strncpy(resolved_path, args, 255);
    }

    // Check if the argument is a URL (starts with http:// or https://)
    if (strncmp(args, "http://", 7) == 0 || strncmp(args, "https://", 8) == 0) {
        sys_print("Opening URL in browser: ");
        sys_print(args);
        sys_print("\n");

        // Launch browser with URL argument
        extern void init_browser_app_with_url(const char* url);
        init_browser_app_with_url(args);
        return;
    }

    // Check if it's a .app bundle
    int path_len = strlen(resolved_path);
    if (path_len > 4 && strcmp(resolved_path + path_len - 4, ".app") == 0) {
        sys_print("Launching app: ");
        sys_print(resolved_path);
        sys_print("\n");
        execute_program(resolved_path);
        return;
    }

    // Check if it's a .dmg file
    if (path_len > 4 && strcmp(resolved_path + path_len - 4, ".dmg") == 0) {
        if (!sys_fs_exists(resolved_path)) {
            sys_print("Error: DMG file not found: ");
            sys_print(resolved_path);
            sys_print("\n");
            return;
        }
        sys_print("Mounting DMG: ");
        sys_print(resolved_path);
        sys_print("\n");
        int result = app_installer_open_dmg(resolved_path);
        if (result < 0) {
            sys_print("Error: Could not mount DMG.\n");
        }
        return;
    }

    // Check if it's a .cdl file
    if (path_len > 4 && strcmp(resolved_path + path_len - 4, ".cdl") == 0) {
        sys_print("Loading CDL app: ");
        sys_print(resolved_path);
        sys_print("\n");
        int handle = sys_load_library(resolved_path);
        if (handle >= 0) {
            sys_print("CDL app loaded successfully.\n");
        } else {
            sys_print("Error: Could not load CDL app.\n");
        }
        return;
    }

    // Fallback: try to execute it as a program
    sys_print("Attempting to launch: ");
    sys_print(resolved_path);
    sys_print("\n");
    execute_program(resolved_path);
}

void execute_program(const char* path) {
    sys_print("Launching App: "); sys_print(path); sys_print("\n");
    
    // Use the unified resolve_and_load path from cdl_loader
    // This ensures consistent app resolution across dock, desktop, and shell
    extern int wrap_exec(const char*);
    int result = wrap_exec(path);
    
    if (result >= 0) {
        sys_print("App loaded successfully (Handle "); 
        char n[4]; int_to_str(result, n); sys_print(n); 
        sys_print(")\n");
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

        // Record command in history
        if (pos > 0) {
            strncpy(cmd_history[history_pos], cmd_buffer, 127);
            cmd_history[history_pos][127] = 0;
            history_pos = (history_pos + 1) % MAX_HISTORY;
            if (history_count < MAX_HISTORY) history_count++;
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
            sys_print("CamelOS Shell Commands:\n");
            sys_print("  --- File Operations ---\n");
            sys_print("  ls [dir]          - List directory contents\n");
            sys_print("  cd <path>         - Change directory\n");
            sys_print("  cat <file>        - Display file contents\n");
            sys_print("  mkdir <dir>       - Create directory\n");
            sys_print("  rm <path>         - Delete file or directory\n");
            sys_print("  cp <src> <dst>    - Copy file\n");
            sys_print("  mv <old> <new>    - Move/rename file\n");
            sys_print("  pwd               - Print working directory\n");
            sys_print("  touch <file>      - Create empty file or update timestamp\n");
            sys_print("  head <file>       - Show first 10 lines of file\n");
            sys_print("  tail <file>       - Show last 10 lines of file\n");
            sys_print("  wc <file>         - Count lines, words, bytes\n");
            sys_print("  grep <pat> <file> - Search for pattern in file\n");
            sys_print("  find <dir> <name> - Find files matching name\n");
            sys_print("  chmod <mode> <path> - Change file permissions\n");
            sys_print("  hexdump <file>    - Hex dump a file\n");
            sys_print("  --- System Info ---\n");
            sys_print("  date              - Show current date/time\n");
            sys_print("  whoami            - Show current username\n");
            sys_print("  hostname          - Show system hostname\n");
            sys_print("  uname             - Show kernel version info\n");
            sys_print("  uptime            - Show system uptime\n");
            sys_print("  df                - Show disk usage\n");
            sys_print("  free              - Show memory info\n");
            sys_print("  sysinfo           - Show detailed system info\n");
            sys_print("  --- Process Management ---\n");
            sys_print("  ps                - List processes\n");
            sys_print("  kill <pid> <sig>  - Send signal to process\n");
            sys_print("  top               - Show process list with CPU\n");
            sys_print("  --- Environment ---\n");
            sys_print("  env               - Show environment variables\n");
            sys_print("  export VAR=val    - Set environment variable\n");
            sys_print("  echo <text>       - Print text\n");
            sys_print("  history           - Show command history\n");
            sys_print("  --- System Control ---\n");
            sys_print("  reboot            - Reboot system\n");
            sys_print("  shutdown          - Halt system\n");
            sys_print("  theme dark|light  - Switch dark/light mode\n");
            sys_print("  --- Applications ---\n");
            sys_print("  gui               - Start graphical environment\n");
            sys_print("  clear             - Clear screen\n");
            sys_print("  ./<file>          - Execute program\n");
            sys_print("  run <app>         - Run an application\n");
            sys_print("  open <url|app>    - Open URL or app\n");
            sys_print("  ping <host>       - Ping a host\n");
            sys_print("  curl <url>        - Download from URL\n");
            sys_print("  caml <cmd> [args] - Package manager\n");
            sys_print("    caml install <pkg>  - Install a .cpkg package\n");
            sys_print("    caml remove <name>  - Remove installed app\n");
            sys_print("    caml list           - List installed packages\n");
            sys_print("    caml search <query> - Search for packages\n");
            sys_print("    caml info <name>    - Show package info\n");
            sys_print("    caml verify <pkg>   - Verify package file\n");
            sys_print("    caml rebuild        - Rebuild package database\n");
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
        else if (strcmp(cmd, "curl") == 0) {
            // Get the raw args after "curl " to preserve URL and -o flag
            char* curl_args = strstr(cmd_buffer, "curl ");
            if(curl_args) {
                curl_args += 5; // Skip "curl "
                cmd_curl(curl_args);
            } else {
                cmd_curl("");
            }
        }
        else if (strcmp(cmd, "open") == 0) {
            // Get the raw args after "open "
            char* open_args = strstr(cmd_buffer, "open ");
            if(open_args) {
                open_args += 5; // Skip "open "
                cmd_open(open_args);
            } else {
                cmd_open("");
            }
        }
        // ====================================================================
        // Package Manager: caml install/remove/list/search/info/verify/rebuild
        // ====================================================================
        else if (strcmp(cmd, "caml") == 0) {
            // Parse subcommand and its argument
            char subcmd[32] = {0};
            char subarg[128] = {0};
            int si = 0, sj = 0;
            // Skip to after "caml "
            char* caml_rest = strstr(cmd_buffer, "caml ");
            if (caml_rest) {
                caml_rest += 5; // skip "caml "
                // Parse subcommand
                while(caml_rest[si] && caml_rest[si] != ' ' && sj < 31) subcmd[sj++] = caml_rest[si++];
                if(caml_rest[si] == ' ') si++;
                // Parse sub-argument (rest of line)
                sj = 0;
                while(caml_rest[si] && sj < 127) subarg[sj++] = caml_rest[si++];
            }

            if (strcmp(subcmd, "install") == 0) {
                if (strlen(subarg) == 0) {
                    sys_print("Usage: caml install <package.cpkg|package.dmg>\n");
                } else {
                    // Resolve path
                    char pkg_path[256];
                    if (subarg[0] != '/') {
                        strcpy(pkg_path, current_path);
                        int plen = strlen(pkg_path);
                        if (plen > 1 && pkg_path[plen-1] != '/') strcat(pkg_path, "/");
                        strcat(pkg_path, subarg);
                    } else {
                        strcpy(pkg_path, subarg);
                    }

                    int result;
                    int plen2 = strlen(pkg_path);
                    if (plen2 > 4 && strcmp(pkg_path + plen2 - 4, ".dmg") == 0) {
                        sys_print("Installing from DMG: ");
                        sys_print(pkg_path);
                        sys_print("\n");
                        result = pkg_install_dmg(pkg_path);
                    } else {
                        sys_print("Installing package: ");
                        sys_print(pkg_path);
                        sys_print("\n");
                        result = pkg_install(pkg_path);
                    }

                    if (result == 0) {
                        sys_print("Installation successful.\n");
                    } else {
                        sys_print("Installation failed: ");
                        sys_print(pkg_get_error());
                        sys_print("\n");
                    }
                }
            }
            else if (strcmp(subcmd, "remove") == 0) {
                if (strlen(subarg) == 0) {
                    sys_print("Usage: caml remove <app-name>\n");
                } else {
                    sys_print("Removing: ");
                    sys_print(subarg);
                    sys_print("\n");
                    int result = pkg_remove(subarg);
                    if (result == 0) {
                        sys_print("Package removed successfully.\n");
                    } else {
                        sys_print("Removal failed: ");
                        sys_print(pkg_get_error());
                        sys_print("\n");
                    }
                }
            }
            else if (strcmp(subcmd, "list") == 0) {
                AppBundleInfo apps[64];
                int count = pkg_list_installed(apps, 64);
                if (count <= 0) {
                    sys_print("No packages installed.\n");
                } else {
                    char count_str[8];
                    int_to_str(count, count_str);
                    sys_print("Installed packages (");
                    sys_print(count_str);
                    sys_print("):\n");
                    for (int i = 0; i < count; i++) {
                        sys_print("  ");
                        sys_print(apps[i].name);
                        if (apps[i].version[0]) {
                            sys_print(" ");
                            sys_print(apps[i].version);
                        }
                        if (apps[i].type[0]) {
                            sys_print(" [");
                            sys_print(apps[i].type);
                            sys_print("]");
                        }
                        sys_print("\n");
                    }
                }
            }
            else if (strcmp(subcmd, "search") == 0) {
                if (strlen(subarg) == 0) {
                    sys_print("Usage: caml search <query>\n");
                } else {
                    char results[2048];
                    int count = pkg_search(subarg, results, sizeof(results));
                    if (count <= 0) {
                        sys_print("No packages found matching '");
                        sys_print(subarg);
                        sys_print("'\n");
                    } else {
                        char cstr[8];
                        int_to_str(count, cstr);
                        sys_print("Found ");
                        sys_print(cstr);
                        sys_print(" package(s):\n");
                        sys_print(results);
                    }
                }
            }
            else if (strcmp(subcmd, "info") == 0) {
                if (strlen(subarg) == 0) {
                    sys_print("Usage: caml info <app-name>\n");
                } else {
                    AppBundleInfo info;
                    int result = pkg_get_info(subarg, &info);
                    if (result == 0) {
                        sys_print("Name:       "); sys_print(info.name); sys_print("\n");
                        sys_print("Identifier: "); sys_print(info.identifier); sys_print("\n");
                        sys_print("Version:    "); sys_print(info.version); sys_print("\n");
                        sys_print("Type:       "); sys_print(info.type); sys_print("\n");
                        sys_print("Executable: "); sys_print(info.executable); sys_print("\n");
                        if (info.icon_file[0]) {
                            sys_print("Icon:       "); sys_print(info.icon_file); sys_print("\n");
                        }
                        if (info.min_os_version[0]) {
                            sys_print("Min OS:     "); sys_print(info.min_os_version); sys_print("\n");
                        }
                    } else {
                        sys_print("Package not found: ");
                        sys_print(subarg);
                        sys_print("\n");
                    }
                }
            }
            else if (strcmp(subcmd, "verify") == 0) {
                if (strlen(subarg) == 0) {
                    sys_print("Usage: caml verify <package.cpkg>\n");
                } else {
                    char vpath[256];
                    if (subarg[0] != '/') {
                        strcpy(vpath, current_path);
                        int plen = strlen(vpath);
                        if (plen > 1 && vpath[plen-1] != '/') strcat(vpath, "/");
                        strcat(vpath, subarg);
                    } else {
                        strcpy(vpath, subarg);
                    }
                    int result = pkg_verify(vpath);
                    if (result == 0) {
                        sys_print("Package is valid.\n");
                    } else {
                        sys_print("Package verification failed: ");
                        sys_print(pkg_get_error());
                        sys_print("\n");
                    }
                }
            }
            else if (strcmp(subcmd, "rebuild") == 0) {
                sys_print("Rebuilding package database...\n");
                int result = pkg_rebuild_database();
                if (result == 0) {
                    sys_print("Package database rebuilt.\n");
                } else {
                    sys_print("Database rebuild failed.\n");
                }
            }
            else if (strcmp(subcmd, "apps") == 0) {
                // List all apps in the registry
                int count = app_registry_get_count();
                char cstr[8];
                int_to_str(count, cstr);
                sys_print("Registered apps (");
                sys_print(cstr);
                sys_print("):\n");
                for (int i = 0; i < count; i++) {
                    const app_registry_entry_t* entry = app_registry_get(i);
                    if (entry) {
                        sys_print("  ");
                        sys_print(entry->bundle_info.name);
                        if (entry->is_builtin) sys_print(" [builtin]");
                        else sys_print(" [installed]");
                        sys_print("\n");
                    }
                }
            }
            else {
                sys_print("CamelOS Package Manager (caml)\n");
                sys_print("Usage: caml <command> [args]\n");
                sys_print("Commands:\n");
                sys_print("  install <pkg>   - Install a package\n");
                sys_print("  remove <name>   - Remove an installed app\n");
                sys_print("  list            - List installed packages\n");
                sys_print("  search <query>  - Search for packages\n");
                sys_print("  info <name>     - Show package info\n");
                sys_print("  verify <pkg>    - Verify a package file\n");
                sys_print("  rebuild         - Rebuild package database\n");
                sys_print("  apps            - List all registered apps\n");
            }
        }
        // ====================================================================
        // Additional file commands: mkdir, rm, cp, mv, pwd, echo, hexdump
        // ====================================================================
        else if (strcmp(cmd, "mkdir") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: mkdir <directory>\n");
            } else {
                char mpath[256];
                if (arg1[0] != '/') {
                    strcpy(mpath, current_path);
                    int plen = strlen(mpath);
                    if (plen > 1 && mpath[plen-1] != '/') strcat(mpath, "/");
                    strcat(mpath, arg1);
                } else {
                    strcpy(mpath, arg1);
                }
                if (sys_fs_create(mpath, 1) == 0) {
                    sys_print("Created directory: ");
                    sys_print(mpath);
                    sys_print("\n");
                } else {
                    sys_print("Failed to create directory.\n");
                }
            }
        }
        else if (strcmp(cmd, "rm") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: rm <file|directory>\n");
            } else {
                char rpath[256];
                if (arg1[0] != '/') {
                    strcpy(rpath, current_path);
                    int plen = strlen(rpath);
                    if (plen > 1 && rpath[plen-1] != '/') strcat(rpath, "/");
                    strcat(rpath, arg1);
                } else {
                    strcpy(rpath, arg1);
                }
                if (sys_fs_is_dir(rpath)) {
                    sys_fs_delete_recursive(rpath);
                    sys_print("Removed: ");
                    sys_print(rpath);
                    sys_print("\n");
                } else {
                    sys_fs_delete(rpath);
                    sys_print("Deleted: ");
                    sys_print(rpath);
                    sys_print("\n");
                }
            }
        }
        else if (strcmp(cmd, "cp") == 0) {
            // Parse second argument
            char arg2[64] = {0};
            int ci = 0, cj = 0;
            // Skip cmd and arg1
            char* cp_rest = cmd_buffer;
            while(cp_rest[ci] && cp_rest[ci] != ' ') ci++; // skip cp
            if(cp_rest[ci] == ' ') ci++; // skip space
            while(cp_rest[ci] && cp_rest[ci] != ' ') ci++; // skip arg1
            if(cp_rest[ci] == ' ') ci++; // skip space
            while(cp_rest[ci] && cp_rest[ci] != ' ' && cj < 63) arg2[cj++] = cp_rest[ci++];

            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                sys_print("Usage: cp <source> <destination>\n");
            } else {
                sys_fs_copy(arg1, arg2);
                sys_print("Copied.\n");
            }
        }
        else if (strcmp(cmd, "mv") == 0) {
            // Parse second argument
            char m_arg2[64] = {0};
            int mi = 0, mj = 0;
            char* mv_rest = cmd_buffer;
            while(mv_rest[mi] && mv_rest[mi] != ' ') mi++;
            if(mv_rest[mi] == ' ') mi++;
            while(mv_rest[mi] && mv_rest[mi] != ' ') mi++;
            if(mv_rest[mi] == ' ') mi++;
            while(mv_rest[mi] && mv_rest[mi] != ' ' && mj < 63) m_arg2[mj++] = mv_rest[mi++];

            if (strlen(arg1) == 0 || strlen(m_arg2) == 0) {
                sys_print("Usage: mv <old> <new>\n");
            } else {
                sys_fs_rename(arg1, m_arg2);
                sys_print("Moved.\n");
            }
        }
        else if (strcmp(cmd, "pwd") == 0) {
            sys_print(current_path);
            sys_print("\n");
        }
        else if (strcmp(cmd, "echo") == 0) {
            char* echo_text = strstr(cmd_buffer, "echo ");
            if (echo_text) {
                echo_text += 5;
                sys_print(echo_text);
            }
            sys_print("\n");
        }
        else if (strcmp(cmd, "hexdump") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: hexdump <file>\n");
            } else {
                char* hbuf = (char*)kmalloc(4096);
                if (hbuf) {
                    memset(hbuf, 0, 4096);
                    int hlen = sys_fs_read(arg1, hbuf, 4095);
                    if (hlen > 0) {
                        for (int off = 0; off < hlen; off += 16) {
                            // Offset
                            char line[80];
                            int pos2 = 0;
                            pos2 += sprintf(line + pos2, "%04x: ", off);
                            // Hex bytes
                            for (int b = 0; b < 16 && off + b < hlen; b++) {
                                pos2 += sprintf(line + pos2, "%02x ", (unsigned char)hbuf[off + b]);
                            }
                            // Pad
                            for (int b = hlen - off < 16 ? hlen - off : 0; b < 16; b++) {
                                pos2 += sprintf(line + pos2, "   ");
                            }
                            // ASCII
                            pos2 += sprintf(line + pos2, " ");
                            for (int b = 0; b < 16 && off + b < hlen; b++) {
                                unsigned char c = hbuf[off + b];
                                line[pos2++] = (c >= 0x20 && c < 0x7F) ? c : '.';
                            }
                            line[pos2] = 0;
                            sys_print(line);
                            sys_print("\n");
                        }
                    } else {
                        sys_print("File not found or empty.\n");
                    }
                    kfree(hbuf);
                }
            }
        }
        // ====================================================================
        // System information commands
        // ====================================================================
        else if (strcmp(cmd, "date") == 0) {
            int h, m, s, year, month, day;
            sys_get_time(&h, &m, &s);
            sys_get_date(&year, &month, &day);
            int tz = sys_get_tz_offset();
            print_zpad(year, 4); sys_print("-");
            print_zpad(month, 2); sys_print("-");
            print_zpad(day, 2); sys_print(" ");
            print_zpad(h, 2); sys_print(":");
            print_zpad(m, 2); sys_print(":");
            print_zpad(s, 2); sys_print(" UTC");
            if (tz >= 0) sys_print("+");
            else { sys_print("-"); tz = -tz; }
            print_zpad(tz / 60, 2); sys_print(":");
            print_zpad(tz % 60, 2);
            sys_print("\n");
        }
        else if (strcmp(cmd, "whoami") == 0) {
            char ubuf[256];
            memset(ubuf, 0, 256);
            int ulen = sys_fs_read("/Library/Preferences/system.conf", ubuf, 255);
            if (ulen > 0) {
                char* line = strstr(ubuf, "username=");
                if (line) {
                    line += 9;
                    char* end = line;
                    while(*end && *end != '\n' && *end != '\r') end++;
                    *end = 0;
                    sys_print(line);
                } else {
                    sys_print("user");
                }
            } else {
                sys_print("user");
            }
            sys_print("\n");
        }
        else if (strcmp(cmd, "hostname") == 0) {
            char hbuf[64];
            memset(hbuf, 0, 64);
            int hlen = sys_fs_read("/etc/hostname", hbuf, 63);
            if (hlen > 0) {
                char* end = hbuf;
                while(*end && *end != '\n' && *end != '\r') end++;
                *end = 0;
                sys_print(hbuf);
            } else {
                sys_print("camelos");
            }
            sys_print("\n");
        }
        else if (strcmp(cmd, "uname") == 0) {
            if (strlen(arg1) > 0 && strcmp(arg1, "-a") == 0) {
                sys_print("CamelOS 1.2.0 camelos i386\n");
            } else if (strlen(arg1) > 0 && strcmp(arg1, "-r") == 0) {
                sys_print("1.2.0\n");
            } else if (strlen(arg1) > 0 && strcmp(arg1, "-s") == 0) {
                sys_print("CamelOS\n");
            } else {
                sys_print("CamelOS\n");
            }
        }
        else if (strcmp(cmd, "uptime") == 0) {
            uint32_t ticks = timer_ticks;
            uint32_t secs = ticks / 100;
            uint32_t mins = secs / 60;
            uint32_t hrs = mins / 60;
            mins = mins % 60;
            secs = secs % 60;
            sys_print("up ");
            print_zpad(hrs, 1); sys_print(":");
            print_zpad(mins, 2); sys_print(":");
            print_zpad(secs, 2);
            sched_stats_t* stats = scheduler_get_stats();
            if (stats) {
                sys_print(", ");
                print_rpad(stats->total_tasks, 0);
                sys_print(" tasks, ");
                print_rpad(stats->context_switches, 0);
                sys_print(" switches");
            }
            sys_print("\n");
        }
        else if (strcmp(cmd, "df") == 0) {
            pfs32_stats_t dstats;
            memset(&dstats, 0, sizeof(dstats));
            pfs32_get_stats(&dstats);
            uint32_t total_kb = (dstats.blocks_free + dstats.total_sectors_used) / 2;
            uint32_t used_kb = dstats.total_sectors_used / 2;
            uint32_t free_kb = dstats.blocks_free / 2;
            int pct = 0;
            if (total_kb > 0) pct = (used_kb * 100) / total_kb;
            sys_print("Filesystem     1K-blocks       Used   Available  Use%  Mounted on\n");
            sys_print("/dev/pfs32     ");
            print_rpad(total_kb, 8); sys_print("  ");
            print_rpad(used_kb, 8); sys_print("  ");
            print_rpad(free_kb, 8); sys_print("   ");
            print_rpad(pct, 2); sys_print("%  /\n");
        }
        else if (strcmp(cmd, "free") == 0) {
            uint32_t free_mem = k_get_free_mem();
            uint32_t total_mem = k_get_total_mem();
            uint32_t used_mem = total_mem - free_mem;
            sys_print("              total        used        free\n");
            sys_print("Mem:     ");
            print_rpad(total_mem / 1024, 8); sys_print("  ");
            print_rpad(used_mem / 1024, 8); sys_print("  ");
            print_rpad(free_mem / 1024, 8); sys_print("\n");
        }
        else if (strcmp(cmd, "ps") == 0) {
            process_info_t pinfo[64];
            int count = process_list(pinfo, 64);
            sys_print("  PID  PPID  UID  STATE        NAME\n");
            for (int pi = 0; pi < count; pi++) {
                const char* state_str;
                switch (pinfo[pi].state) {
                    case 0: state_str = "READY"; break;
                    case 1: state_str = "RUNNING"; break;
                    case 2: state_str = "BLOCKED"; break;
                    case 3: state_str = "ZOMBIE"; break;
                    case 4: state_str = "SLEEPING"; break;
                    default: state_str = "UNKNOWN"; break;
                }
                print_rpad(pinfo[pi].pid, 5); sys_print(" ");
                print_rpad(pinfo[pi].ppid, 5); sys_print(" ");
                print_rpad(pinfo[pi].uid, 4); sys_print("  ");
                sys_print(state_str);
                for (int pad = strlen(state_str); pad < 12; pad++) sys_print(" ");
                sys_print(pinfo[pi].name);
                sys_print("\n");
            }
        }
        else if (strcmp(cmd, "kill") == 0) {
            char k_arg2[16] = {0};
            int ki = 0, kj = 0;
            char* k_rest = cmd_buffer;
            while(k_rest[ki] && k_rest[ki] != ' ') ki++;
            if(k_rest[ki] == ' ') ki++;
            while(k_rest[ki] && k_rest[ki] != ' ') ki++;
            if(k_rest[ki] == ' ') ki++;
            while(k_rest[ki] && k_rest[ki] != ' ' && kj < 15) k_arg2[kj++] = k_rest[ki++];

            if (strlen(arg1) == 0 || strlen(k_arg2) == 0) {
                sys_print("Usage: kill <pid> <signal>\n");
            } else {
                int pid = atoi(arg1);
                int sig = atoi(k_arg2);
                int result = process_kill(pid, sig);
                if (result == 0) {
                    sys_print("Signal sent.\n");
                } else {
                    sys_print("Failed to send signal.\n");
                }
            }
        }
        else if (strcmp(cmd, "top") == 0) {
            sys_print("CamelOS Top - ");
            char tmbuf[16];
            int_to_str(k_get_total_mem() / 1024, tmbuf);
            sys_print(tmbuf);
            sys_print(" KB total memory\n\n");
            process_info_t pinfo[64];
            int count = process_list(pinfo, 64);
            sys_print("  PID  STATE          TICKS  NAME\n");
            for (int ti = 0; ti < count; ti++) {
                const char* state_str;
                switch (pinfo[ti].state) {
                    case 0: state_str = "READY"; break;
                    case 1: state_str = "RUNNING"; break;
                    case 2: state_str = "BLOCKED"; break;
                    case 3: state_str = "ZOMBIE"; break;
                    case 4: state_str = "SLEEPING"; break;
                    default: state_str = "UNKNOWN"; break;
                }
                print_rpad(pinfo[ti].pid, 5); sys_print("  ");
                sys_print(state_str);
                for (int pad = strlen(state_str); pad < 12; pad++) sys_print(" ");
                print_rpad(pinfo[ti].cpu_ticks, 6); sys_print("  ");
                sys_print(pinfo[ti].name);
                sys_print("\n");
            }
        }
        else if (strcmp(cmd, "env") == 0) {
            // Read from /etc/profile
            char ebuf[2048];
            memset(ebuf, 0, 2048);
            int elen = sys_fs_read("/etc/profile", ebuf, 2047);
            if (elen > 0) {
                char* line = ebuf;
                while (line < ebuf + elen) {
                    char* eol = line;
                    while (*eol && *eol != '\n') eol++;
                    int was_n = (*eol == '\n');
                    *eol = 0;
                    if (strncmp(line, "export ", 7) == 0) {
                        sys_print(line + 7); sys_print("\n");
                    } else if (strstr(line, "=") && line[0] != '#') {
                        sys_print(line); sys_print("\n");
                    }
                    if (was_n) line = eol + 1; else break;
                }
            }
            // Also show in-memory env vars
            for (int ei = 0; ei < env_count; ei++) {
                sys_print(env_names[ei]);
                sys_print("=");
                sys_print(env_values[ei]);
                sys_print("\n");
            }
            if (elen <= 0 && env_count == 0) {
                sys_print("No environment variables set.\n");
            }
        }
        else if (strcmp(cmd, "export") == 0) {
            char* rest = strstr(cmd_buffer, "export ");
            if (rest) {
                rest += 7;
                char* eq = 0;
                const char* p = rest;
                while (*p && *p != ' ') {
                    if (*p == '=') { eq = (char*)p; break; }
                    p++;
                }
                if (eq) {
                    int nlen = eq - rest;
                    char ename[32] = {0};
                    if (nlen > 31) nlen = 31;
                    memcpy(ename, rest, nlen);
                    char* val = eq + 1;
                    if (*val == '"') val++;
                    int vlen = strlen(val);
                    if (vlen > 0 && val[vlen-1] == '"') vlen--;
                    if (env_count < MAX_ENV_VARS) {
                        strncpy(env_names[env_count], ename, MAX_ENV_NAME - 1);
                        if (vlen > MAX_ENV_VALUE - 1) vlen = MAX_ENV_VALUE - 1;
                        memcpy(env_values[env_count], val, vlen);
                        env_values[env_count][vlen] = 0;
                        env_count++;
                    } else {
                        sys_print("Environment table full.\n");
                    }
                } else {
                    sys_print("Usage: export VAR=value\n");
                }
            } else {
                sys_print("Usage: export VAR=value\n");
            }
        }
        else if (strcmp(cmd, "touch") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: touch <file>\n");
            } else {
                char tpath[256];
                if (arg1[0] != '/') {
                    strcpy(tpath, current_path);
                    int plen = strlen(tpath);
                    if (plen > 1 && tpath[plen-1] != '/') strcat(tpath, "/");
                    strcat(tpath, arg1);
                } else {
                    strcpy(tpath, arg1);
                }
                if (sys_fs_exists(tpath)) {
                    sys_print("Timestamp updated: ");
                    sys_print(tpath);
                    sys_print("\n");
                } else {
                    if (sys_fs_create(tpath, 0) == 0) {
                        sys_print("Created: ");
                        sys_print(tpath);
                        sys_print("\n");
                    } else {
                        sys_print("Failed to create file.\n");
                    }
                }
            }
        }
        else if (strcmp(cmd, "head") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: head <file>\n");
            } else {
                char* hbuf2 = (char*)kmalloc(8192);
                if (hbuf2) {
                    memset(hbuf2, 0, 8192);
                    int hlen2 = sys_fs_read(arg1, hbuf2, 8191);
                    if (hlen2 > 0) {
                        int lc = 0;
                        for (int hi = 0; hi < hlen2 && lc < 10; hi++) {
                            char t[2] = {hbuf2[hi], 0};
                            sys_print(t);
                            if (hbuf2[hi] == '\n') lc++;
                        }
                        if (hlen2 > 0 && hbuf2[hlen2-1] != '\n') sys_print("\n");
                    } else {
                        sys_print("File not found or empty.\n");
                    }
                    kfree(hbuf2);
                }
            }
        }
        else if (strcmp(cmd, "tail") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: tail <file>\n");
            } else {
                char* tbuf2 = (char*)kmalloc(8192);
                if (tbuf2) {
                    memset(tbuf2, 0, 8192);
                    int tlen2 = sys_fs_read(arg1, tbuf2, 8191);
                    if (tlen2 > 0) {
                        int lc2 = 0;
                        int start = tlen2;
                        for (int ti = tlen2 - 1; ti >= 0; ti--) {
                            if (tbuf2[ti] == '\n') {
                                lc2++;
                                if (lc2 >= 10) { start = ti + 1; break; }
                            }
                        }
                        if (lc2 < 10) start = 0;
                        for (int ti = start; ti < tlen2; ti++) {
                            char t[2] = {tbuf2[ti], 0};
                            sys_print(t);
                        }
                        if (tlen2 > 0 && tbuf2[tlen2-1] != '\n') sys_print("\n");
                    } else {
                        sys_print("File not found or empty.\n");
                    }
                    kfree(tbuf2);
                }
            }
        }
        else if (strcmp(cmd, "wc") == 0) {
            if (strlen(arg1) == 0) {
                sys_print("Usage: wc <file>\n");
            } else {
                char* wbuf = (char*)kmalloc(8192);
                if (wbuf) {
                    memset(wbuf, 0, 8192);
                    int wlen = sys_fs_read(arg1, wbuf, 8191);
                    if (wlen > 0) {
                        int lines = 0, words = 0, in_word = 0;
                        for (int wi = 0; wi < wlen; wi++) {
                            if (wbuf[wi] == '\n') lines++;
                            if (wbuf[wi] == ' ' || wbuf[wi] == '\t' || wbuf[wi] == '\n' || wbuf[wi] == '\r') {
                                in_word = 0;
                            } else {
                                if (!in_word) { words++; in_word = 1; }
                            }
                        }
                        print_rpad(lines, 7); sys_print(" ");
                        print_rpad(words, 7); sys_print(" ");
                        print_rpad(wlen, 7); sys_print(" ");
                        sys_print(arg1);
                        sys_print("\n");
                    } else {
                        sys_print("File not found or empty.\n");
                    }
                    kfree(wbuf);
                }
            }
        }
        else if (strcmp(cmd, "grep") == 0) {
            char g_arg2[64] = {0};
            int gi = 0, gj = 0;
            char* g_rest = cmd_buffer;
            while(g_rest[gi] && g_rest[gi] != ' ') gi++;
            if(g_rest[gi] == ' ') gi++;
            while(g_rest[gi] && g_rest[gi] != ' ') gi++;
            if(g_rest[gi] == ' ') gi++;
            while(g_rest[gi] && g_rest[gi] != ' ' && gj < 63) g_arg2[gj++] = g_rest[gi++];

            if (strlen(arg1) == 0 || strlen(g_arg2) == 0) {
                sys_print("Usage: grep <pattern> <file>\n");
            } else {
                char* gbuf = (char*)kmalloc(8192);
                if (gbuf) {
                    memset(gbuf, 0, 8192);
                    int glen = sys_fs_read(g_arg2, gbuf, 8191);
                    if (glen > 0) {
                        char* gline = gbuf;
                        while (gline < gbuf + glen) {
                            char* eol = gline;
                            while (*eol && *eol != '\n') eol++;
                            int was_n = (*eol == '\n');
                            *eol = 0;
                            if (strstr(gline, arg1)) {
                                sys_print(gline); sys_print("\n");
                            }
                            if (was_n) gline = eol + 1; else break;
                        }
                    } else {
                        sys_print("File not found.\n");
                    }
                    kfree(gbuf);
                }
            }
        }
        else if (strcmp(cmd, "find") == 0) {
            char f_arg2[64] = {0};
            int fi = 0, fj = 0;
            char* f_rest = cmd_buffer;
            while(f_rest[fi] && f_rest[fi] != ' ') fi++;
            if(f_rest[fi] == ' ') fi++;
            while(f_rest[fi] && f_rest[fi] != ' ') fi++;
            if(f_rest[fi] == ' ') fi++;
            while(f_rest[fi] && f_rest[fi] != ' ' && fj < 63) f_arg2[fj++] = f_rest[fi++];

            if (strlen(arg1) == 0 || strlen(f_arg2) == 0) {
                sys_print("Usage: find <dir> <name_pattern>\n");
            } else {
                pfs32_direntry_t entries[64];
                int fcount = sys_fs_list_dir(arg1, entries, 64);
                if (fcount > 0) {
                    for (int fei = 0; fei < fcount; fei++) {
                        if (entries[fei].filename[0] == 0) continue;
                        if (strstr(entries[fei].filename, f_arg2)) {
                            sys_print(arg1);
                            if (arg1[strlen(arg1)-1] != '/') sys_print("/");
                            sys_print(entries[fei].filename);
                            sys_print("\n");
                        }
                    }
                } else {
                    sys_print("Directory not found or empty.\n");
                }
            }
        }
        else if (strcmp(cmd, "chmod") == 0) {
            char c_arg2[64] = {0};
            int ci2 = 0, cj2 = 0;
            char* c_rest = cmd_buffer;
            while(c_rest[ci2] && c_rest[ci2] != ' ') ci2++;
            if(c_rest[ci2] == ' ') ci2++;
            while(c_rest[ci2] && c_rest[ci2] != ' ') ci2++;
            if(c_rest[ci2] == ' ') ci2++;
            while(c_rest[ci2] && c_rest[ci2] != ' ' && cj2 < 63) c_arg2[cj2++] = c_rest[ci2++];

            if (strlen(arg1) == 0 || strlen(c_arg2) == 0) {
                sys_print("Usage: chmod <mode> <path>\n");
            } else {
                pfs32_direntry_t entry;
                if (pfs32_stat(c_arg2, &entry) == 0) {
                    int mode = 0;
                    const char* mp = arg1;
                    while(*mp >= '0' && *mp <= '7') mode = mode * 8 + (*mp++ - '0');
                    entry.permissions = (uint8_t)mode;
                    sys_print("Mode changed: ");
                    sys_print(c_arg2);
                    sys_print("\n");
                } else {
                    sys_print("File not found.\n");
                }
            }
        }
        else if (strcmp(cmd, "reboot") == 0) {
            sys_print("Rebooting system...\n");
            sys_reboot();
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            sys_print("Shutting down...\n");
            sys_shutdown();
        }
        else if (strcmp(cmd, "theme") == 0) {
            if (strcmp(arg1, "dark") == 0) {
                theme_set(THEME_DARK);
                sys_print("Theme set to dark mode.\n");
            } else if (strcmp(arg1, "light") == 0) {
                theme_set(THEME_LIGHT);
                sys_print("Theme set to light mode.\n");
            } else {
                int current = theme_get_id();
                sys_print("Current theme: ");
                sys_print(current == THEME_DARK ? "dark" : "light");
                sys_print("\nUsage: theme dark|light\n");
            }
        }
        else if (strcmp(cmd, "sysinfo") == 0) {
            sys_print("=== CamelOS System Information ===\n");
            sys_print("Kernel:    CamelOS 1.2.0 i386\n");
            sys_print("CPU:       x86 (i386 compatible)\n");

            uint32_t total_mem = k_get_total_mem();
            uint32_t free_mem = k_get_free_mem();
            char sbuf[16];
            int_to_str(total_mem / 1024, sbuf);
            sys_print("Memory:    "); sys_print(sbuf); sys_print(" KB total, ");
            int_to_str(free_mem / 1024, sbuf);
            sys_print(sbuf); sys_print(" KB free\n");

            pfs32_stats_t dstats;
            memset(&dstats, 0, sizeof(dstats));
            pfs32_get_stats(&dstats);
            int_to_str((dstats.blocks_free + dstats.total_sectors_used) / 2, sbuf);
            sys_print("Disk:      "); sys_print(sbuf); sys_print(" KB total, ");
            int_to_str(dstats.blocks_free / 2, sbuf);
            sys_print(sbuf); sys_print(" KB free\n");

            uint32_t ticks = timer_ticks;
            uint32_t secs = ticks / 100;
            uint32_t mins = secs / 60;
            uint32_t hrs = mins / 60;
            mins = mins % 60;
            secs = secs % 60;
            sys_print("Uptime:    ");
            print_zpad(hrs, 1); sys_print(":");
            print_zpad(mins, 2); sys_print(":");
            print_zpad(secs, 2); sys_print("\n");

            sched_stats_t* sstats = scheduler_get_stats();
            if (sstats) {
                int_to_str(sstats->total_tasks, sbuf);
                sys_print("Tasks:     "); sys_print(sbuf); sys_print(" running, ");
                int_to_str(sstats->context_switches, sbuf);
                sys_print(sbuf); sys_print(" context switches\n");
            }
            sys_print("Scheduler: Priority-based preemptive (round-robin)\n");
            sys_print("===================================\n");
        }
        else if (strcmp(cmd, "history") == 0) {
            int start = 0;
            if (history_count >= MAX_HISTORY) {
                start = history_pos;
            }
            for (int hi = 0; hi < history_count; hi++) {
                int idx = (start + hi) % MAX_HISTORY;
                print_rpad(hi + 1, 4); sys_print("  ");
                sys_print(cmd_history[idx]);
                sys_print("\n");
            }
        }
        else if(strlen(cmd) > 0) {
            sys_print("Unknown command. Type 'help' for available commands.\n");
        }

        // 2. Heap cleanup after command finishes.
        // NOTE: We used to k_rewind_heap(mark) here, but that's extremely dangerous —
        // it frees ALL memory allocated during command execution, including GUI windows
        // created by wrap_exec() (open, run, etc.), causing use-after-free crashes
        // (Int 13 GPF, Int 14 Page Fault). Instead, each command that allocates
        // temporary buffers should kfree() them explicitly. The heap mark/rewind
        // is now only used for commands that are known to NOT create persistent objects.
        //
        // Commands that create persistent objects (windows, installed apps, etc.):
        //   open, run, curl (with .app/.dmg auto-install)
        // These must NOT rewind the heap.
        //
        // Commands that are safe to rewind:
        //   ls, cd, pwd, echo, clear, cat, help, ping
        int safe_to_rewind = 1;
        if (strcmp(cmd, "open") == 0 || strcmp(cmd, "run") == 0 ||
            (strcmp(cmd, "curl") == 0) || strncmp(cmd, "./", 2) == 0) {
            safe_to_rewind = 0;
        }
        if (safe_to_rewind) {
            k_rewind_heap(mark);
        }
    }
}
