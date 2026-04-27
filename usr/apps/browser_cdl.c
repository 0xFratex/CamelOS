// ============================================================================
// VERSION INFO
// ============================================================================
#define BROWSER_VERSION "4.9"
#define BROWSER_VERSION_NUM 490

#include "../../sys/cdl_defs.h"
#include "../lib/camel_framework.h"

kernel_api_t* sys = 0;

// JS Engine wrappers
typedef int   (*jscore_init_t)();
typedef const char* (*jscore_eval_t)(const char*);
typedef void  (*jscore_cleanup_t)();

static jscore_init_t    js_init = 0;
static jscore_eval_t    js_eval = 0;
static jscore_cleanup_t js_cleanup = 0;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

// Soft-float removed to reduce size

// ============================================================================
// CONFIGURATION
// ============================================================================
#define MAX_URL             512
#define MAX_CONTENT         (5 * 1024 * 1024)  // Increased to 5MB for modern sites
#define MAX_TITLE           128
#define CHAR_W              8
#define CHAR_H              16

// ============================================================================
// BROWSER UI & NETWORK PROXY
// ============================================================================
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in_t;

#define AF_INET 2
#define SOCK_STREAM 1

#define BROWSER_W 800
#define BROWSER_H 600
#define CONTENT_W 800
#define CONTENT_H 530
#define UI_H 70

typedef struct {
    char url[MAX_URL];
    int url_len;
    char title[MAX_TITLE];
    int node_count;
    int is_active;
} browser_tab_t;

#define MAX_TABS 4
static browser_tab_t tabs[MAX_TABS];
static int active_tab = 0;
static int tab_count = 1;

static uint32_t* frame_buffer = 0;
static int is_loading = 0;
static void* main_win = 0;

static uint32_t last_cursor_toggle = 0;
static int cursor_visible = 1;

static char* html_buffer = 0;
static char* parse_text_buffer = 0; 
static char* current_tag_buf = 0;

static int meta_refresh_triggered = 0;
static char meta_refresh_url[MAX_URL];

// Debugging depth for redirects
static int redirect_depth = 0;

// ============================================================================
// STACK-SAFE BUFFERS (Moved here to prevent Kernel Panics from Stack Overflows)
// ============================================================================
static char f_final_url[MAX_URL];
static char f_temp[MAX_URL];
static char f_debug_buf[513];
static char f_first_link[256];
static char f_next_url[MAX_URL];
static char f_old_url[MAX_URL];
static char f_base_url[MAX_URL];
static char mouse_new_url[MAX_URL];

// JavaScript Engine State
// JS engine loaded separately

// JS functions moved to jscore.c

typedef struct dom_node {
    int type; // 0=text, 1=element, 2=closing element
    char tag[16];
    char text[128];
    char attr[128];
    char attr2[32];
    
    int x, y, w, h;
    uint32_t color;
    int font_size; 
} dom_node_t;

#define MAX_NODES 8192
static dom_node_t* nodes = 0;
static dom_node_t* temp_nodes = 0;
static int node_count = 0;

// JS functions moved to jscore.c

// Case insensitive tag comparison
static int is_tag(const char* tag, const char* name) {
    int i = 0;
    while (tag[i] == ' ') tag++; 
    while (tag[i] && tag[i] != ' ' && tag[i] != '>' && tag[i] != '/') {
        char c1 = tag[i];
        char c2 = name[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        i++;
    }
    return (name[i] == 0);
}

// Highly robust, case-insensitive attribute extractor 
static void get_attribute(const char* tag, const char* name, char* out, int max_len) {
    out[0] = 0;
    int name_len = sys->strlen(name);
    int tag_len = sys->strlen(tag);
    if (tag_len < name_len) return;
    
    for (int i = 0; i <= tag_len - name_len; i++) {
        int match = 1;
        for (int j = 0; j < name_len; j++) {
            char c1 = tag[i+j];
            char c2 = name[j];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) { match = 0; break; }
        }
        if (match && (tag[i+name_len] == '=' || tag[i+name_len] == ' ')) {
            int p = i + name_len;
            while (p < tag_len && tag[p] == ' ') p++;
            if (p < tag_len && tag[p] == '=') {
                p++;
                while (p < tag_len && tag[p] == ' ') p++;
                char quote = ' ';
                if (p < tag_len && (tag[p] == '"' || tag[p] == '\'')) {
                    quote = tag[p];
                    p++;
                }
                int out_idx = 0;
                while (p < tag_len && tag[p] && tag[p] != quote && tag[p] != '>' && out_idx < max_len - 1) {
                    if (quote == ' ' && tag[p] == ' ') break;
                    out[out_idx++] = tag[p++];
                }
                out[out_idx] = 0;
                return;
            }
        }
    }
}

static void parse_html(const char* html) {
    node_count = 0;
    int i = 0;
    int in_tag = 0;
    int in_script = 0;
    int in_hidden = 0;
    char hidden_tag_name[16] = {0};
    int in_title = 0;
    int in_comment = 0;
    int in_noscript = 0;  // Track <noscript> separately - content should be VISIBLE
    int tag_len = 0;
    int text_len = 0;
    
    sys->memset(parse_text_buffer, 0, MAX_CONTENT);
    
    while(html[i] && i < MAX_CONTENT && node_count < MAX_NODES) {
        // Handle <!-- comments -->
        if (!in_tag && html[i] == '<' && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-') {
            in_comment = 1;
            i += 4;
            continue;
        }
        if (in_comment) {
            if (html[i] == '-' && html[i+1] == '-' && html[i+2] == '>') {
                in_comment = 0;
                i += 2;
            }
            i++;
            continue;
        }
        // Handle <!DOCTYPE ...> and <![CDATA[ ... ]]> - skip them entirely
        if (!in_tag && html[i] == '<' && html[i+1] == '!') {
            // Skip until closing >
            i += 2;
            while(html[i] && html[i] != '>') i++;
            if (html[i] == '>') i++;
            continue;
        }

        if(html[i] == '<') {
            if(text_len > 0) {
                if (in_script) {
                    // Execute JavaScript from <script> tags
                    // Only try simple scripts - skip minified/complex ones that Elk can't handle
                    int looks_simple = 1;
                    if (text_len > 4096) looks_simple = 0;  // Too long for Elk
                    if (text_len > 50) {
                        // Check for patterns that indicate complex JS Elk can't handle
                        if (sys->strstr(parse_text_buffer, "class ")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "=>")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "async ")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "await ")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Promise")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Symbol")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Map(")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Set(")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "WeakRef")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Proxy")) looks_simple = 0;
                        if (sys->strstr(parse_text_buffer, "Reflect")) looks_simple = 0;
                    }
                    
                    if (js_eval && text_len > 0 && looks_simple) {
                        sys->print("[JS] Evaluating script (");
                        char num_buf[16];
                        sys->itoa(text_len, num_buf);
                        sys->print(num_buf);
                        sys->print(" bytes)\n");
                        const char* result = js_eval(parse_text_buffer);
                        if (result && sys->strcmp(result, "ok") != 0) {
                            sys->print("[JS] Script execution error: ");
                            sys->print(result);
                            sys->print("\n");
                        }
                    } else if (text_len > 0) {
                        sys->print("[JS] Skipping complex script (");
                        char num_buf[16];
                        sys->itoa(text_len, num_buf);
                        sys->print(num_buf);
                        sys->print(" bytes - too complex for Elk)\n");
                    }
                } else if (in_title) {
                    sys->strncpy(tabs[active_tab].title, parse_text_buffer, MAX_TITLE-1);
                } else if (!in_hidden) {
                    // noscript content IS shown (in_noscript just tracks we're inside it)
                    if (node_count < MAX_NODES) {
                        dom_node_t* n = &nodes[node_count++];
                        sys->memset(n, 0, sizeof(dom_node_t));
                        n->type = 0;
                        sys->strncpy(n->text, parse_text_buffer, 127);
                        sys->strcpy(n->tag, "text");
                        n->color = 0xFF000000;
                        n->font_size = 16;
                    }
                }
                text_len = 0;
                sys->memset(parse_text_buffer, 0, MAX_CONTENT);
            }
            in_tag = 1;
            tag_len = 0;
            if (current_tag_buf) sys->memset(current_tag_buf, 0, 8192);
        } else if(html[i] == '>') {
            in_tag = 0;
            if(tag_len > 0 && current_tag_buf[0] != '/') {
                // CRITICAL FIX: Implicit <head> closure — HTML5 allows omitting </head>.
                // If we encounter <body> (or other body-level tags) while still
                // inside <head>, automatically close the head section so body
                // content is not silently discarded.
                if (in_hidden && hidden_tag_name[0] != 0 &&
                    (is_tag(current_tag_buf, "body") || is_tag(current_tag_buf, "div") ||
                     is_tag(current_tag_buf, "p") || is_tag(current_tag_buf, "table") ||
                     is_tag(current_tag_buf, "h1") || is_tag(current_tag_buf, "h2") ||
                     is_tag(current_tag_buf, "h3") || is_tag(current_tag_buf, "h4") ||
                     is_tag(current_tag_buf, "h5") || is_tag(current_tag_buf, "h6") ||
                     is_tag(current_tag_buf, "ul") || is_tag(current_tag_buf, "ol") ||
                     is_tag(current_tag_buf, "form") || is_tag(current_tag_buf, "main") ||
                     is_tag(current_tag_buf, "section") || is_tag(current_tag_buf, "article") ||
                     is_tag(current_tag_buf, "nav") || is_tag(current_tag_buf, "footer") ||
                     is_tag(current_tag_buf, "header"))) {
                    sys->print("[PARSE] Implicit </head> closure - body-level tag <");
                    sys->print(current_tag_buf);
                    sys->print("> encountered while in_hidden\n");
                    in_hidden = 0;
                    hidden_tag_name[0] = 0;
                }

                if (is_tag(current_tag_buf, "script")) in_script = 1;
                if (is_tag(current_tag_buf, "title")) in_title = 1;
                
                // Detect <meta http-equiv="refresh" content="...url=...">
                if (is_tag(current_tag_buf, "meta")) {
                    char equiv[64] = {0};
                    char content[256] = {0};
                    get_attribute(current_tag_buf, "http-equiv", equiv, 63);
                    get_attribute(current_tag_buf, "content", content, 255);
                    
                    for(int k=0; equiv[k]; k++) if(equiv[k] >= 'A' && equiv[k] <= 'Z') equiv[k] += 32;
                    
                    if (sys->strstr(equiv, "refresh") != 0) {
                        char* url_ptr = (char*)sys->strstr(content, "url=");
                        if (!url_ptr) url_ptr = (char*)sys->strstr(content, "URL=");
                        if (url_ptr) {
                            url_ptr += 4;
                            if (*url_ptr == '\'' || *url_ptr == '"') url_ptr++;
                            int u_idx = 0;
                            while(url_ptr[u_idx] && url_ptr[u_idx] != '\'' && url_ptr[u_idx] != '"' && u_idx < MAX_URL - 1) {
                                meta_refresh_url[u_idx] = url_ptr[u_idx];
                                u_idx++;
                            }
                            meta_refresh_url[u_idx] = 0;
                            meta_refresh_triggered = 1;
                        }
                    }
                }
                
                // CRITICAL FIX: <noscript> content must be VISIBLE in non-JS browsers!
                // Only hide: <style>, <head>, <svg> - NOT <noscript>
                // <noscript> is specifically designed for browsers without JS support
                if (!in_hidden && !in_noscript && (is_tag(current_tag_buf, "style") || is_tag(current_tag_buf, "head") || 
                    is_tag(current_tag_buf, "svg"))) {
                    
                    int is_self_closing = 0;
                    if (tag_len > 0 && current_tag_buf[tag_len-1] == '/') is_self_closing = 1;
                    
                    if (!is_self_closing) {
                        in_hidden = 1;
                        int k=0;
                        while(current_tag_buf[k] && current_tag_buf[k] != ' ' && k < 15) {
                            hidden_tag_name[k] = current_tag_buf[k];
                            k++;
                        }
                        hidden_tag_name[k] = 0;
                    }
                }
                
                // Track <noscript> separately - its content should be SHOWN (not hidden)
                // since our browser has limited/no JS capability.
                // IMPORTANT: <noscript> OVERRIDES <head> hiding - content inside
                // <head><noscript> should still be visible in a non-JS browser
                if (is_tag(current_tag_buf, "noscript")) {
                    in_noscript = 1;
                    // If we're inside <head> (in_hidden), save the hidden state
                    // so we can restore it after </noscript>
                    if (in_hidden) {
                        // Temporarily unhide so noscript content is visible
                        // The hidden_tag_name is preserved for restoration
                        in_hidden = 0;
                    }
                    // Don't add <noscript> tag as a visible element - skip it
                    // but continue to process its children as visible content
                    i++;
                    continue;
                }
                
                if (node_count < MAX_NODES) {
                    dom_node_t* n = &nodes[node_count++];
                    sys->memset(n, 0, sizeof(dom_node_t));
                    n->type = 1;
                    
                    int k = 0;
                    while(current_tag_buf[k] && current_tag_buf[k] != ' ' && k < 15) {
                        n->tag[k] = current_tag_buf[k];
                        k++;
                    }
                    n->tag[k] = 0;

                    n->color = 0xFF000000;
                    n->font_size = 16;
                    
                    if(is_tag(n->tag, "h1")) n->font_size = 24;
                    if(is_tag(n->tag, "h2")) n->font_size = 20;
                    if(is_tag(n->tag, "h3")) n->font_size = 18;
                    if(is_tag(n->tag, "a")) {
                        n->color = 0xFF0000FF; 
                        get_attribute(current_tag_buf, "href", n->attr, 127);
                    }
                    if(is_tag(n->tag, "img")) {
                        get_attribute(current_tag_buf, "src", n->attr, 127);
                        get_attribute(current_tag_buf, "alt", n->attr2, 31);
                    }
                    if(is_tag(n->tag, "form")) {
                        get_attribute(current_tag_buf, "action", n->attr, 127);
                        get_attribute(current_tag_buf, "method", n->attr2, 31);
                    }
                    if(is_tag(n->tag, "input")) {
                        get_attribute(current_tag_buf, "type", n->attr, 127);
                        get_attribute(current_tag_buf, "name", n->attr2, 31);
                    }
                    if(is_tag(n->tag, "select")) {
                        get_attribute(current_tag_buf, "name", n->attr, 127);
                    }
                    if(is_tag(n->tag, "option")) {
                        get_attribute(current_tag_buf, "value", n->attr, 127);
                    }
                }
            } else if(tag_len > 0 && current_tag_buf[0] == '/') {
                if (is_tag(current_tag_buf + 1, "script")) in_script = 0;
                if (is_tag(current_tag_buf + 1, "title")) in_title = 0;
                if (is_tag(current_tag_buf + 1, "noscript")) {
                    in_noscript = 0;
                    // If we were inside <head> before <noscript>, restore hidden state.
                    // But ONLY restore if we haven't already left the hidden section
                    // via an explicit </head> or implicit closure (in_hidden already 0).
                    if (in_hidden == 0 && hidden_tag_name[0] != 0 && is_tag(hidden_tag_name, "head")) {
                        in_hidden = 1;
                    }
                }
                // Un-hide: closing tag matches the hidden tag name.
                // The +1 skips the '/' prefix in the closing tag buffer.
                // This handles </head>, </style>, </svg> etc.
                if (in_hidden && is_tag(current_tag_buf + 1, hidden_tag_name)) {
                    sys->print("[PARSE] Un-hiding on </");
                    sys->print(hidden_tag_name);
                    sys->print("> tag\n");
                    in_hidden = 0;
                    sys->memset(hidden_tag_name, 0, sizeof(hidden_tag_name));
                }
                
                // Skip </noscript> closing tag from being added as node
                if (is_tag(current_tag_buf + 1, "noscript")) {
                    i++;
                    continue;
                }
                
                if (node_count < MAX_NODES) {
                    dom_node_t* n = &nodes[node_count++];
                    n->type = 2; 
                    sys->strncpy(n->tag, current_tag_buf + 1, 14);
                }
            }
        } else {
            if(in_tag) {
                if(tag_len < 8191) current_tag_buf[tag_len++] = html[i];
            } else {
                if(text_len < MAX_CONTENT - 1 && html[i] != '\r' && html[i] != '\n') {
                    parse_text_buffer[text_len++] = html[i];
                }
            }
        }
        i++;
    }
    
    if(text_len > 0 && node_count < MAX_NODES && !in_hidden && !in_script) {
        dom_node_t* n = &nodes[node_count++];
        n->type = 0;
        sys->strncpy(n->text, parse_text_buffer, 127);
        sys->strcpy(n->tag, "text");
        n->color = 0xFF000000;
        n->font_size = 16;
    }
    
    // Count visual (renderable) nodes - comprehensive element type coverage
    int visual_nodes = 0;
    for(int k=0; k<node_count; k++) {
        if (nodes[k].type == 0) {
            int len = sys->strlen(nodes[k].text);
            int is_whitespace = 1;
            for (int m=0; m<len; m++) {
                if (nodes[k].text[m] != ' ' && nodes[k].text[m] != '\n' && nodes[k].text[m] != '\r' && nodes[k].text[m] != '\t') is_whitespace = 0;
            }
            if (!is_whitespace) visual_nodes++;
        }
        if (nodes[k].type == 1 && (is_tag(nodes[k].tag, "img") || is_tag(nodes[k].tag, "input") || 
            is_tag(nodes[k].tag, "button") || is_tag(nodes[k].tag, "form") ||
            is_tag(nodes[k].tag, "h1") || is_tag(nodes[k].tag, "h2") ||
            is_tag(nodes[k].tag, "h3") || is_tag(nodes[k].tag, "h4") ||
            is_tag(nodes[k].tag, "textarea") || is_tag(nodes[k].tag, "a") ||
            is_tag(nodes[k].tag, "p") || is_tag(nodes[k].tag, "center") ||
            is_tag(nodes[k].tag, "font") || is_tag(nodes[k].tag, "span") ||
            is_tag(nodes[k].tag, "table") || is_tag(nodes[k].tag, "td") ||
            is_tag(nodes[k].tag, "tr") || is_tag(nodes[k].tag, "th") ||
            is_tag(nodes[k].tag, "li") || is_tag(nodes[k].tag, "ul") ||
            is_tag(nodes[k].tag, "ol") || is_tag(nodes[k].tag, "b") ||
            is_tag(nodes[k].tag, "i") || is_tag(nodes[k].tag, "strong") ||
            is_tag(nodes[k].tag, "em") || is_tag(nodes[k].tag, "pre") ||
            is_tag(nodes[k].tag, "code") || is_tag(nodes[k].tag, "select") ||
            is_tag(nodes[k].tag, "option"))) visual_nodes++;
    }
    
    // Only show fallback when there are truly NO visual nodes at all
    // CRITICAL: Replace existing nodes, don't append - to prevent overlap
    if (visual_nodes == 0 && node_count < MAX_NODES) {
        // Save the page title before clearing nodes
        char saved_title[MAX_TITLE];
        sys->strncpy(saved_title, tabs[active_tab].title, MAX_TITLE - 1);
        saved_title[MAX_TITLE - 1] = 0;
        
        // CLEAR all existing nodes first to prevent overlap with fallback
        node_count = 0;
        
        // First, show the page title if we got one
        if (saved_title[0] != 0) {
            dom_node_t* tn = &nodes[node_count++];
            sys->memset(tn, 0, sizeof(dom_node_t));
            tn->type = 0;
            sys->strcpy(tn->tag, "text");
            tn->color = 0xFF000000;
            tn->font_size = 24;
            sys->strncpy(tn->text, saved_title, 127);
        }
        
        dom_node_t* n = &nodes[node_count++];
        sys->memset(n, 0, sizeof(dom_node_t));
        n->type = 0;
        sys->strcpy(n->text, "This page requires JavaScript to render its content.");
        sys->strcpy(n->tag, "text");
        n->color = 0xFF888888;
        n->font_size = 16;
        
        n = &nodes[node_count++];
        sys->memset(n, 0, sizeof(dom_node_t));
        n->type = 0;
        sys->strcpy(n->text, "The server sent a page that needs a JS-capable browser.");
        sys->strcpy(n->tag, "text");
        n->color = 0xFF888888;
        n->font_size = 16;
    }
}

static void layout_html() {
    int cx = 10;
    int cy = 10;
    int current_font = 16;
    int max_x = CONTENT_W - 20;
    
    int original_count = node_count;
    node_count = 0;
    
    for (int i=0; i<original_count; i++) temp_nodes[i] = nodes[i];
    
    for(int i=0; i<original_count; i++) {
        dom_node_t* n = &temp_nodes[i];
        if(n->type == 1) {
            if(sys->strcmp(n->tag, "h1") == 0) {
                cy += 30; cx = 10;
                current_font = 24;
            } else if(sys->strcmp(n->tag, "h2") == 0) {
                cy += 24; cx = 10;
                current_font = 20;
            } else if(sys->strcmp(n->tag, "h3") == 0) {
                cy += 20; cx = 10;
                current_font = 18;
            } else if(sys->strcmp(n->tag, "h4") == 0) {
                cy += 18; cx = 10;
                current_font = 17;
            } else if(sys->strcmp(n->tag, "br") == 0 || sys->strcmp(n->tag, "p") == 0 || sys->strcmp(n->tag, "div") == 0 || sys->strcmp(n->tag, "li") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "center") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "ul") == 0 || sys->strcmp(n->tag, "ol") == 0) {
                cy += current_font + 4;
                cx = 30;  // Indent lists
            } else if(sys->strcmp(n->tag, "table") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "tr") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "td") == 0 || sys->strcmp(n->tag, "th") == 0) {
                // Table cells: keep inline, add padding
                cx += 8;
            } else if(sys->strcmp(n->tag, "hr") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "blockquote") == 0 || sys->strcmp(n->tag, "pre") == 0) {
                cy += current_font + 4;
                cx = 30;  // Indent blockquotes
            } else if (is_tag(n->tag, "img") || is_tag(n->tag, "video") || is_tag(n->tag, "audio")) {
                if (cx > 10) { cx = 10; cy += 20; }
                n->x = cx;
                n->y = cy;
                n->w = 100;
                n->h = 60;
                cy += 70;
            } else if (is_tag(n->tag, "form")) {
                cy += current_font + 8; cx = 10;
                n->x = cx; n->y = cy;
                n->w = max_x; n->h = 40;
                cy += 50;
            } else if (is_tag(n->tag, "input")) {
                // Render input fields as placeholder boxes
                n->x = cx;
                n->y = cy;
                n->w = 150;
                n->h = 22;
                cx += 160;
            } else if (is_tag(n->tag, "button")) {
                n->x = cx;
                n->y = cy;
                n->w = 80;
                n->h = 22;
                cx += 90;
            } else if (is_tag(n->tag, "select")) {
                n->x = cx;
                n->y = cy;
                n->w = 120;
                n->h = 22;
                cx += 130;
            }
            // Tags like <a>, <font>, <span>, <b>, <i>, <em>, <strong>, <code>
            // are inline - they don't cause line breaks, just flow with text
            if (node_count < MAX_NODES) nodes[node_count++] = *n;
        } else if(n->type == 2) { 
            if(sys->strcmp(n->tag, "h1") == 0) {
                cy += current_font + 4;
                cx = 10;
                current_font = 16;
            } else if(sys->strcmp(n->tag, "h2") == 0) {
                cy += current_font + 4;
                cx = 10;
                current_font = 16;
            } else if(sys->strcmp(n->tag, "h3") == 0) {
                cy += current_font + 4;
                cx = 10;
                current_font = 16;
            } else if(sys->strcmp(n->tag, "h4") == 0) {
                cy += current_font + 4;
                cx = 10;
                current_font = 16;
            } else if(sys->strcmp(n->tag, "p") == 0 || sys->strcmp(n->tag, "div") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "center") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "ul") == 0 || sys->strcmp(n->tag, "ol") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "table") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "tr") == 0) {
                cy += 2;
                cx = 10;
            } else if(sys->strcmp(n->tag, "blockquote") == 0 || sys->strcmp(n->tag, "pre") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if(sys->strcmp(n->tag, "form") == 0) {
                cy += 10; cx = 10;
            }
            if (node_count < MAX_NODES) nodes[node_count++] = *n;
        } else if(n->type == 0) { 
            char* text = n->text;
            int len = sys->strlen(text);
            int word_start = 0;
            char current_line[256];
            int line_pos = 0;
            
            for (int k = 0; k <= len; k++) {
                if (text[k] == ' ' || text[k] == 0) {
                    int word_len = k - word_start;
                    int word_px = word_len * 8;
                    
                    if (cx + word_px > max_x && line_pos > 0) {
                        if (node_count < MAX_NODES) {
                            dom_node_t* new_node = &nodes[node_count++];
                            new_node->type = 0;
                            new_node->x = 10;
                            new_node->y = cy;
                            new_node->w = line_pos * 8;
                            new_node->h = current_font;
                            new_node->font_size = current_font;
                            new_node->color = n->color;
                            current_line[line_pos] = 0;
                            sys->strcpy(new_node->text, current_line);
                        }
                        cx = 10;
                        cy += current_font + 4;
                        line_pos = 0;
                    }
                    
                    for (int m=word_start; m<k; m++) {
                        if (line_pos < 254) current_line[line_pos++] = text[m];
                    }
                    if (text[k] == ' ') {
                        if (line_pos < 254) current_line[line_pos++] = ' ';
                    }
                    
                    cx += (word_len + 1) * 8;
                    word_start = k + 1;
                }
            }
            
            if (line_pos > 0) {
                if (node_count < MAX_NODES) {
                    dom_node_t* new_node = &nodes[node_count++];
                    new_node->type = 0;
                    new_node->x = 10;
                    new_node->y = cy;
                    new_node->w = line_pos * 8;
                    new_node->h = current_font;
                    new_node->font_size = current_font;
                    new_node->color = n->color;
                    current_line[line_pos] = 0;
                    sys->strcpy(new_node->text, current_line);
                }
            }
        }
    }
}

void draw_ui(int x, int y, int w, int h) {
    sys->draw_rect(x, y, w, UI_H, 0xFFE0E0E0);
    
    int tab_w = 150;
    for (int i = 0; i < tab_count; i++) {
        int tx = x + (i * tab_w);
        int active = (i == active_tab);
        sys->draw_rect(tx, y, tab_w - 2, 25, active ? 0xFFFFFFFF : 0xFFB0B0B0);
        char tab_label[24];
        if (tabs[i].title[0]) {
             sys->strncpy(tab_label, tabs[i].title, 20);
             tab_label[20] = 0;
        } else if (tabs[i].url_len > 0) {
            sys->strncpy(tab_label, tabs[i].url, 20);
            tab_label[20] = 0;
        } else {
            sys->sprintf(tab_label, "New Tab");
        }
        sys->draw_text(tx + 10, y + 5, tab_label, 0xFF000000);
    }
    
    if (tab_count < MAX_TABS) {
        int ax = x + (tab_count * tab_w);
        sys->draw_rect(ax, y, 30, 25, 0xFFD0D0D0);
        sys->draw_text(ax + 10, y + 5, "+", 0xFF000000);
    }
    
    sys->draw_rect_rounded(x + 10, y + 30, 30, 30, 0xFFC0C0C0, 5);
    sys->draw_text(x + 18, y + 38, "<", 0xFF000000); 
    
    sys->draw_rect_rounded(x + 50, y + 30, 30, 30, 0xFFC0C0C0, 5);
    sys->draw_text(x + 58, y + 38, ">", 0xFF000000); 
    
    sys->draw_rect_rounded(x + 90, y + 30, 30, 30, 0xFFC0C0C0, 5);
    sys->draw_text(x + 98, y + 38, "R", 0xFF000000); 
    
    sys->draw_rect_rounded(x + 130, y + 30, w - 210, 30, 0xFFFFFFFF, 5);
    sys->draw_text(x + 135, y + 38, tabs[active_tab].url, 0xFF000000);
    
    uint32_t ticks = sys->get_ticks();
    if (ticks - last_cursor_toggle > 500) {
        cursor_visible = !cursor_visible;
        last_cursor_toggle = ticks;
    }
    if (cursor_visible && !is_loading) {
        int cursor_x = x + 135 + (tabs[active_tab].url_len * 8);
        sys->draw_rect(cursor_x, y + 35, 2, 20, 0xFF007AFF);
    }
    
    sys->draw_rect_rounded(x + w - 70, y + 30, 60, 30, 0xFF007AFF, 5);
    sys->draw_text(x + w - 60, y + 38, is_loading ? "Wait" : "Go", 0xFFFFFFFF);
    
    sys->draw_rect(x, y + UI_H, CONTENT_W, CONTENT_H, 0xFFFFFFFF);
    if (is_loading) {
        sys->draw_text(x + 20, y + UI_H + 20, "Loading...", 0xFF888888);
    } else if (tabs[active_tab].node_count == 0) {
        sys->draw_text(x + 20, y + UI_H + 20, "CamelOS Native Browser. Enter URL and press Go!", 0xFF888888);
    } else {
        int in_link = 0;
        for(int i=0; i<node_count; i++) {
            dom_node_t* n = &nodes[i];
            
            if (n->y > CONTENT_H) continue;
            
            if (n->type == 1 && is_tag(n->tag, "a")) in_link = 1;
            if (n->type == 2 && is_tag(n->tag, "a")) in_link = 0;
            
            if(n->type == 0 && sys->strlen(n->text) > 0) {
                uint32_t color = in_link ? 0xFF0000AA : n->color;
                sys->draw_text(x + n->x, y + UI_H + n->y, n->text, color);
                if (in_link) {
                    int text_w = sys->strlen(n->text) * 8;
                    sys->draw_rect(x + n->x, y + UI_H + n->y + 14, text_w, 1, color);
                }
            } else if (n->type == 1) {
                if (is_tag(n->tag, "img")) {
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFCCCCCC);
                    sys->draw_text(x + n->x + 5, y + UI_H + n->y + 5, "IMG", 0xFF000000);
                    if (n->attr2[0]) sys->draw_text(x + n->x + 5, y + UI_H + n->y + 25, n->attr2, 0xFF444444);
                } else if (is_tag(n->tag, "hr")) {
                    // Horizontal rule
                    sys->draw_rect(x + n->x, y + UI_H + n->y + 8, CONTENT_W - 40, 1, 0xFFCCCCCC);
                } else if (is_tag(n->tag, "form")) {
                    // Draw form container border
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFEEEEEE);
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, 1, 0xFFCCCCCC);
                } else if (is_tag(n->tag, "input")) {
                    // Draw input field placeholder
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFFFFFFF);
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFAAAAAA);
                    // Show input name/type as label
                    if (n->attr[0]) {
                        sys->draw_text(x + n->x + 3, y + UI_H + n->y + 4, n->attr, 0xFF888888);
                    }
                } else if (is_tag(n->tag, "select")) {
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFFFFFFF);
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFAAAAAA);
                    sys->draw_text(x + n->x + 3, y + UI_H + n->y + 4, "[select]", 0xFF888888);
                } else if (is_tag(n->tag, "button")) {
                    // Draw button placeholder
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFFDDDDDD);
                    sys->draw_rect(x + n->x, y + UI_H + n->y, n->w, n->h, 0xFF888888);
                    sys->draw_text(x + n->x + 10, y + UI_H + n->y + 4, "Button", 0xFF444444);
                }
            }
        }
    }
}

void on_paint(int x, int y, int w, int h) {
    draw_ui(x, y, w, h);
}

void fetch_page() {
    redirect_depth = 0;
    
start_fetch:
    // Short pause to let the OS network stack breathe between redirects
    if (redirect_depth > 0) {
        for(volatile int d=0; d<10000000; d++) asm volatile("pause");
    }

    // Using the static global f_final_url instead of stack array
    // Default to HTTP since CamelOS TLS has limited ECDH support.
    // HTTPS URLs will still be attempted but will gracefully fall back.
    if (sys->strstr(tabs[active_tab].url, "://") == 0) {
        sys->sprintf(f_final_url, "http://%s", tabs[active_tab].url);
    } else {
        sys->strcpy(f_final_url, tabs[active_tab].url);
    }
    
    // --- MISSING ROOT SLASH FIX ---
    char* proto_ptr = sys->strstr(f_final_url, "://");
    if (proto_ptr) {
        char* host_start = proto_ptr + 3;
        char* slash_ptr = sys->strstr(host_start, "/");
        char* query_ptr = sys->strstr(host_start, "?");
        
        if (!slash_ptr || (query_ptr && query_ptr < slash_ptr)) {
            if (query_ptr) {
                int host_len = query_ptr - f_final_url;
                if (host_len < MAX_URL - 2) {
                    for(int k=0; k<host_len; k++) f_temp[k] = f_final_url[k];
                    f_temp[host_len] = 0;
                    sys->sprintf(f_temp + host_len, "/%s", query_ptr);
                    sys->strncpy(f_final_url, f_temp, MAX_URL-1);
                }
            } else {
                if (sys->strlen(f_final_url) < MAX_URL - 2) {
                    sys->sprintf(f_temp, "%s/", f_final_url);
                    sys->strcpy(f_final_url, f_temp);
                }
            }
        }
    }
    // ------------------------------
    
    // --- CRITICAL FIX: Set f_base_url for proper relative URL resolution ---
    // Extract protocol + host (e.g. "https://www.google.com") from f_final_url
    f_base_url[0] = 0;
    {
        char* bp = sys->strstr(f_final_url, "://");
        if (bp) {
            int idx = (bp + 3) - f_final_url;  // Start after "://"
            // Find end of host (before first / or ? or end of string)
            while(f_final_url[idx] && f_final_url[idx] != '/' && 
                  f_final_url[idx] != '?' && idx < MAX_URL - 1) {
                idx++;
            }
            sys->strncpy(f_base_url, f_final_url, idx);
            f_base_url[idx] = 0;
        }
    }
    // ---------------------------------------------------------------------------
    
    is_loading = 1;
    meta_refresh_triggered = 0;
    meta_refresh_url[0] = 0;
    
    // --- GOOGLE gbv=1 REMOVED ---
    // Google dropped basic-view (?gbv=1) support around 2019.
    // The parameter is now silently ignored and Google serves the full
    // JS-required page regardless. Prepending it provides zero benefit
    // and only clutters the URL.
    // -------------------------------
    
    sys->print("FETCHING: "); sys->print(f_final_url); sys->print("\n");
    sys->print("[DEBUG] User-Agent & Headers are managed securely by the core network layer (http.c).\n");
    
    sys->memset(html_buffer, 0, MAX_CONTENT);
    node_count = 0;
    
    if(sys->http_get != 0) {
        int bytes = sys->http_get(f_final_url, html_buffer, MAX_CONTENT - 1, 0, 0);
        
        char log[64];
        sys->sprintf(log, "RECEIVED: %d bytes\n", bytes);
        sys->print(log);
        
        if(bytes > 0) {
            
            // --- DEBUG LOGGING ---
            sys->print("--- RAW PAYLOAD START (First 512 bytes) ---\n");
            sys->strncpy(f_debug_buf, html_buffer, 512);
            f_debug_buf[512] = 0;
            sys->print(f_debug_buf);
            sys->print("\n--- RAW PAYLOAD END ---\n");
            // ---------------------

            parse_html(html_buffer);
            
            // --- ENHANCED DEBUG LOGGING ---
            {
                int text_nodes = 0, elem_nodes = 0, close_nodes = 0;
                for(int k=0; k<node_count; k++) {
                    if (nodes[k].type == 0) text_nodes++;
                    else if (nodes[k].type == 1) elem_nodes++;
                    else if (nodes[k].type == 2) close_nodes++;
                }
                char dbuf[128];
                sys->sprintf(dbuf, "[PARSE] %d nodes: %d text, %d elements, %d closing\n", 
                            node_count, text_nodes, elem_nodes, close_nodes);
                sys->print(dbuf);
                if (tabs[active_tab].title[0]) {
                    sys->print("[TITLE] "); sys->print(tabs[active_tab].title); sys->print("\n");
                }
            }
            // --------------------------------
            
            // --- ANTI-BOT BYPASS: Check for "Moved" manual HTML link redirect ---
            int has_moved_text = 0;
            f_first_link[0] = 0; // Clear the static buffer 
            for(int k=0; k<node_count; k++) {
                if (nodes[k].type == 0 && (sys->strstr(nodes[k].text, "Moved") || sys->strstr(nodes[k].text, "moved") || sys->strstr(nodes[k].text, "document has moved"))) {
                    has_moved_text = 1;
                }
                if (nodes[k].type == 1 && is_tag(nodes[k].tag, "a") && f_first_link[0] == 0) {
                    sys->strcpy(f_first_link, nodes[k].attr);
                }
            }
            
            f_next_url[0] = 0;
            if (meta_refresh_triggered && meta_refresh_url[0]) {
                sys->strcpy(f_next_url, meta_refresh_url);
                // Also downconvert meta refresh HTTPS→HTTP for same reason
                if (sys->strstr(f_next_url, "https://") != 0) {
                    char* https_ptr = sys->strstr(f_next_url, "https://");
                    if (https_ptr) {
                        int idx = https_ptr - f_next_url;
                        for (int k = idx + 5; f_next_url[k]; k++) {
                            f_next_url[k - 1] = f_next_url[k];
                        }
                        int len = sys->strlen(f_next_url);
                        f_next_url[len] = 0;
                        sys->print("[REDIR] Downconverted meta-refresh to HTTP: ");
                        sys->print(f_next_url); sys->print("\n");
                    }
                }
            } else if (has_moved_text && f_first_link[0] && node_count < 30) {
                // If it's a tiny page saying "Moved" and it has a link, follow it immediately
                sys->strcpy(f_next_url, f_first_link);
                
                // CRITICAL FIX: TLS is broken in CamelOS (limited ECDH support).
                // When a redirect URL points to https://, convert it to http://
                // to avoid the TLS→HTTP fallback→302 loop. This mirrors the
                // http.c tried_http_fallback guard but at the browser level.
                if (sys->strstr(f_next_url, "https://") != 0) {
                    // Replace "https://" with "http://" in-place
                    char* https_ptr = sys->strstr(f_next_url, "https://");
                    if (https_ptr) {
                        // Shift everything left by 1 to shrink "https" to "http"
                        int idx = https_ptr - f_next_url;
                        // Move 's' position: shift chars after 's' one position left
                        for (int k = idx + 5; f_next_url[k]; k++) {
                            f_next_url[k - 1] = f_next_url[k];
                        }
                        // Find the new end and null-terminate
                        int len = sys->strlen(f_next_url);
                        f_next_url[len] = 0;
                        sys->print("[REDIR] Downconverted redirect to HTTP: ");
                        sys->print(f_next_url); sys->print("\n");
                    }
                }
                
                // Strip gws_rd=ssl (and similar) from URL — this parameter
                // tells Google to force-redirect to HTTPS, creating an
                // infinite loop when TLS is broken.
                char* gws_ptr = sys->strstr(f_next_url, "gws_rd=ssl");
                if (gws_ptr) {
                    // Remove the parameter: &gws_rd=ssl or ?gws_rd=ssl
                    int param_start = gws_ptr - f_next_url;
                    // Check if preceded by & or ?
                    int prefix_len = 0;
                    if (param_start > 0 && (f_next_url[param_start - 1] == '&' || f_next_url[param_start - 1] == '?')) {
                        prefix_len = 1;
                        // If it was '?', this was the only param — strip it entirely
                        if (f_next_url[param_start - 1] == '?' && f_next_url[param_start + 10] != '&') {
                            // ?gws_rd=ssl at end of URL or standalone
                            f_next_url[param_start - 1] = 0;
                        } else {
                            // Shift remaining URL left over the parameter
                            int k = param_start - prefix_len;
                            int remaining = param_start + 10; // past "gws_rd=ssl"
                            // If next char is &, skip it too
                            if (f_next_url[remaining] == '&') remaining++;
                            while (f_next_url[remaining]) {
                                f_next_url[k++] = f_next_url[remaining++];
                            }
                            f_next_url[k] = 0;
                        }
                    } else {
                        // No prefix, just shift over it
                        int remaining = param_start + 10;
                        while (f_next_url[remaining]) {
                            f_next_url[param_start++] = f_next_url[remaining++];
                        }
                        f_next_url[param_start] = 0;
                    }
                    sys->print("[REDIR] Stripped gws_rd=ssl: ");
                    sys->print(f_next_url); sys->print("\n");
                }
            }
            
            if (f_next_url[0]) {
                if (redirect_depth >= 5) {
                    sys->print("Auto-redirect ABORTED: Max depth (5) reached.\n");
                    
                    // Render UI Error for Max Depth
                    node_count = 0;
                    dom_node_t* err = &nodes[node_count++];
                    sys->memset(err, 0, sizeof(dom_node_t));
                    err->type = 0;
                    sys->strcpy(err->text, "Error: Too many redirects (Max 5).");
                    sys->strcpy(err->tag, "text");
                    err->color = 0xFFFF0000;
                    err->font_size = 16;
                    
                    layout_html();
                    tabs[active_tab].node_count = node_count;
                    is_loading = 0;
                    return;
                } else {
                    sys->print("Auto-redirecting to: "); sys->print(f_next_url); sys->print("\n");
                    
                    sys->strcpy(f_old_url, tabs[active_tab].url);

                    if (sys->strstr(f_next_url, "://") == 0) {
                        if (f_next_url[0] == '/') {
                            int slashes = 0;
                            for(int i=0; f_final_url[i]; i++) {
                                if(f_final_url[i] == '/') slashes++;
                                if(slashes == 3) break;
                            }
                            if (sys->strlen(f_base_url) + sys->strlen(f_next_url) < MAX_URL) {
                                sys->sprintf(tabs[active_tab].url, "%s%s", f_base_url, f_next_url);
                            } else {
                                sys->sprintf(tabs[active_tab].url, "Error: URL too long");
                            }
                        } else {
                            if (sys->strlen(f_final_url) + sys->strlen(f_next_url) < MAX_URL) {
                                sys->sprintf(tabs[active_tab].url, "%s%s", f_final_url, f_next_url);
                            } else {
                                sys->sprintf(tabs[active_tab].url, "Error: URL too long");
                            }
                        }
                    } else {
                        if (sys->strlen(tabs[active_tab].url) + sys->strlen(f_next_url) + 1 < MAX_URL) {
                            sys->strncpy(tabs[active_tab].url, f_next_url, MAX_URL-1);
                        } else {
                             sys->sprintf(tabs[active_tab].url, "Error: URL too long");
                        }
                    }
                    
                    // --- LOOP PROTECTION CHECK ---
                    if (sys->strcmp(f_old_url, tabs[active_tab].url) == 0) {
                        sys->print("Auto-redirect ABORTED: Loop detected to exact same URL.\n");
                        
                        node_count = 0;
                        dom_node_t* err = &nodes[node_count++];
                        sys->memset(err, 0, sizeof(dom_node_t));
                        err->type = 0;
                        sys->strcpy(err->text, "Error: Redirect loop detected.");
                        sys->strcpy(err->tag, "text");
                        err->color = 0xFFFF0000;
                        err->font_size = 16;
                        
                        err = &nodes[node_count++];
                        sys->memset(err, 0, sizeof(dom_node_t));
                        err->type = 0;
                        sys->strcpy(err->text, "This site enforces HTTPS, but CamelOS TLS has limited ECDH support.");
                        sys->strcpy(err->tag, "text");
                        err->color = 0xFF888888;
                        err->font_size = 16;
                        
                        err = &nodes[node_count++];
                        sys->memset(err, 0, sizeof(dom_node_t));
                        err->type = 0;
                        sys->strcpy(err->text, "Try a site that works over HTTP, e.g. http://lite.duckduckgo.com/");
                        sys->strcpy(err->tag, "text");
                        err->color = 0xFF888888;
                        err->font_size = 16;
                        
                        layout_html();
                        tabs[active_tab].node_count = node_count;
                        is_loading = 0;
                        return;
                    } else {
                        tabs[active_tab].url_len = sys->strlen(tabs[active_tab].url);
                        redirect_depth++;
                        goto start_fetch; // JUMP TO START - NO MORE RECURSIVE STACK OVERFLOWS
                    }
                }
            }
            // -----------------------------------------------------------------
            
            layout_html();
            tabs[active_tab].node_count = node_count;
            
            sys->sprintf(log, "Parsed %d nodes.\n", node_count);
            sys->print(log);
        } else if (bytes == 0) {
            // Re-use nodes error reporting 
            node_count = 0;
            dom_node_t* err = &nodes[node_count++];
            sys->memset(err, 0, sizeof(dom_node_t));
            err->type = 0;
            sys->strcpy(err->text, "Connection Dropped (Received 0 bytes)");
            sys->strcpy(err->tag, "text");
            err->color = 0xFFFF0000;
            err->font_size = 24;
            
            err = &nodes[node_count++];
            sys->memset(err, 0, sizeof(dom_node_t));
            err->type = 0;
            sys->strcpy(err->text, "The server abruptly closed the connection.");
            sys->strcpy(err->tag, "text");
            err->color = 0xFF000000;
            err->font_size = 16;
            
            layout_html();
            tabs[active_tab].node_count = node_count;
        } else {
            node_count = 0;
            dom_node_t* err = &nodes[node_count++];
            sys->memset(err, 0, sizeof(dom_node_t));
            err->type = 0;
            sys->strcpy(err->text, "Network Error");
            sys->strcpy(err->tag, "text");
            err->color = 0xFFFF0000;
            err->font_size = 24;
            
            err = &nodes[node_count++];
            sys->memset(err, 0, sizeof(dom_node_t));
            err->type = 0;
            sys->strcpy(err->text, "Failed to connect to the server. (DNS Error, Firewall Block, or Unreachable)");
            sys->strcpy(err->tag, "text");
            err->color = 0xFF000000;
            err->font_size = 16;
            
            layout_html();
            tabs[active_tab].node_count = node_count;
        }
    }
    
    is_loading = 0;
}

void on_input(int key) {
    browser_tab_t* tab = &tabs[active_tab];
    if (key == '\n') {
        if (tab->url_len > 0) fetch_page();
    } else if (key == '\b') {
        if (tab->url_len > 0) {
            tab->url_len--;
            tab->url[tab->url_len] = 0;
        }
    } else if (key >= 32 && key <= 126 && tab->url_len < MAX_URL - 1) {
        tab->url[tab->url_len++] = (char)key;
        tab->url[tab->url_len] = 0;
    }
}

void on_mouse(int x, int y, int btn) {
    if (btn != 1) return;
    
    if (y < 25) {
        int tab_w = 150;
        int clicked_tab = x / tab_w;
        if (clicked_tab < tab_count) {
            active_tab = clicked_tab;
        } else if (clicked_tab == tab_count && tab_count < MAX_TABS) {
            if (x >= tab_count * tab_w && x < (tab_count * tab_w) + 30) {
                active_tab = tab_count;
                tab_count++;
                sys->memset(&tabs[active_tab], 0, sizeof(browser_tab_t));
            }
        }
        return;
    }
    
    if (y >= 30 && y <= 60) {
        if (x >= 90 && x <= 120) { 
            if (tabs[active_tab].url_len > 0) fetch_page();
        } else if (x >= BROWSER_W - 70 && x <= BROWSER_W - 10) { 
            if (tabs[active_tab].url_len > 0 && !is_loading) fetch_page();
        }
        return;
    }
    
    if (y > UI_H) {
        int content_x = x;
        int content_y = y - UI_H;
        int current_in_link = 0;
        char current_href[128];
        
        for (int i=0; i<node_count; i++) {
            dom_node_t* n = &nodes[i];
            if (n->type == 1 && is_tag(n->tag, "a")) {
                current_in_link = 1;
                sys->strcpy(current_href, n->attr);
            }
            if (n->type == 2 && is_tag(n->tag, "a")) current_in_link = 0;
            
            if (current_in_link && n->type == 0) {
                if (content_x >= n->x && content_x <= n->x + n->w &&
                    content_y >= n->y && content_y <= n->y + n->h) {
                    
                    if (current_href[0]) {
                        if (sys->strstr(current_href, "://")) {
                            sys->strcpy(tabs[active_tab].url, current_href);
                        } else {
                            if (sys->strlen(tabs[active_tab].url) + sys->strlen(current_href) + 1 < MAX_URL) {
                                sys->sprintf(mouse_new_url, "%s/%s", tabs[active_tab].url, current_href);
                                sys->strcpy(tabs[active_tab].url, mouse_new_url);
                            } else {
                                sys->strcpy(tabs[active_tab].url, "Error: URL too long");
                            }
                        }
                        tabs[active_tab].url_len = sys->strlen(tabs[active_tab].url);
                        fetch_page();
                        return;
                    }
                }
            }
        }
    }
}

static cdl_exports_t exports = { .lib_name = "Browser", .version = BROWSER_VERSION_NUM };

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;

    // Load JS engine
    void* jsmod = sys->cdl_load("/usr/libs/jscore.cdl");
    if (jsmod) {
        js_init = (jscore_init_t) sys->cdl_sym(jsmod, "jscore_init");
        js_eval = (jscore_eval_t) sys->cdl_sym(jsmod, "jscore_eval");
        js_cleanup = (jscore_cleanup_t) sys->cdl_sym(jsmod, "jscore_cleanup");
        if (js_init) js_init();
    }
    
    nodes = (dom_node_t*)sys->malloc(MAX_NODES * sizeof(dom_node_t));
    temp_nodes = (dom_node_t*)sys->malloc(MAX_NODES * sizeof(dom_node_t));
    html_buffer = (char*)sys->malloc(MAX_CONTENT);
    parse_text_buffer = (char*)sys->malloc(MAX_CONTENT);
    current_tag_buf = (char*)sys->malloc(8192);

    if (!nodes || !temp_nodes || !html_buffer || !parse_text_buffer || !current_tag_buf) {
        sys->print("FATAL: Out of memory during browser initialization!\n");
        sys->exit();
        return 0;
    }

    for (int i=0; i<MAX_TABS; i++) {
        sys->memset(&tabs[i], 0, sizeof(browser_tab_t));
    }
    
    sys->strcpy(tabs[0].url, "www.google.com");
    tabs[0].url_len = sys->strlen(tabs[0].url);
    active_tab = 0;
    tab_count = 1;
    
    sys->print("[BROWSER] Default homepage: Google\n");
    
    frame_buffer = (uint32_t*)sys->malloc(CONTENT_W * CONTENT_H * 4);
    if (frame_buffer) {
        sys->memset(frame_buffer, 0, CONTENT_W * CONTENT_H * 4);
    }
    
    main_win = sys->create_window("Camel Browser", BROWSER_W, BROWSER_H, on_paint, on_input, on_mouse);
    
    // JS engine loaded separately
    
    return &exports;
}
