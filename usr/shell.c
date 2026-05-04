#include "../sys/api.h"
#include "../core/string.h"
#include "../core/memory.h"
#include "../core/http.h"
#include "../core/dns.h"
#include <string.h>

// CDL loader declaration
extern int sys_load_library(const char* path);

// Watermark allocator externs
extern unsigned int k_get_heap_mark();
extern void k_rewind_heap(unsigned int m);

// App installer for DMG mounting
extern int app_installer_open_dmg(const char* dmg_path);

// Desktop installer for .app/.cdl auto-install
extern void desktop_install_app(const char* source_path);

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
            sys_print("cmds: ls, cd, cat, gui, reboot, ./<file>, run <app>, loadtest, ping, curl, open\n");
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
        else if(strlen(cmd) > 0) {
            sys_print("Unknown command.\n");
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
