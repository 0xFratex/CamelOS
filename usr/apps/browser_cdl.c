// ============================================================================
// VERSION INFO
// ============================================================================
#define BROWSER_VERSION "4.7"
#define BROWSER_VERSION_NUM 470

#include "../../sys/cdl_defs.h"
#include "../lib/camel_framework.h"
#include "elk.h"

kernel_api_t* sys = 0;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

// --- Soft-float stubs for Elk JS Engine ---
double __floatunsidf(unsigned int i) { return (double)((int)i); }
double __adddf3(double a, double b) { return a; }
double __subdf3(double a, double b) { return a; }
double __muldf3(double a, double b) { return a; }
double __divdf3(double a, double b) { return a; }
int __ltdf2(double a, double b) { return 0; }
int __gedf2(double a, double b) { return 0; }
int __gtdf2(double a, double b) { return 0; }
int __ledf2(double a, double b) { return 0; }
int __eqdf2(double a, double b) { return 0; }
int __nedf2(double a, double b) { return 0; }
int __unorddf2(double a, double b) { return 0; }
int __fixdfsi(double a) { return 0; }
unsigned int __fixunsdfsi(double a) { return 0; }
double __floatsidf(int a) { return 0; }

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
static struct js *js_vm = 0;
static uint8_t js_mem[32768];

static jsval_t js_alert(struct js *js, jsval_t *args, int nargs) {
    if (nargs > 0) {
        size_t len;
        char *str = js_getstr(js, args[0], &len);
        if (str) {
            sys->print("JS ALERT: ");
            sys->print(str);
            sys->print("\n");
        }
    }
    return js_mkundef();
}

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
    int tag_len = 0;
    int text_len = 0;
    
    sys->memset(parse_text_buffer, 0, MAX_CONTENT);
    
    while(html[i] && i < MAX_CONTENT && node_count < MAX_NODES) {
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

        if(html[i] == '<') {
            if(text_len > 0) {
                if (in_script) {
                    // Temporarily disable JS eval due to broken soft-float stubs
                    // if (js_vm && text_len < 4096) js_eval(js_vm, parse_text_buffer, text_len);
                } else if (in_title) {
                    sys->strncpy(tabs[active_tab].title, parse_text_buffer, MAX_TITLE-1);
                } else if (!in_hidden) {
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
                
                if (!in_hidden && (is_tag(current_tag_buf, "style") || is_tag(current_tag_buf, "head") || 
                    is_tag(current_tag_buf, "noscript") || is_tag(current_tag_buf, "svg"))) {
                    
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
                    if(is_tag(n->tag, "a")) {
                        n->color = 0xFF0000FF; 
                        get_attribute(current_tag_buf, "href", n->attr, 127);
                    }
                    if(is_tag(n->tag, "img")) {
                        get_attribute(current_tag_buf, "src", n->attr, 127);
                        get_attribute(current_tag_buf, "alt", n->attr2, 31);
                    }
                }
            } else if(tag_len > 0 && current_tag_buf[0] == '/') {
                if (is_tag(current_tag_buf + 1, "script")) in_script = 0;
                if (is_tag(current_tag_buf + 1, "title")) in_title = 0;
                if (in_hidden && is_tag(current_tag_buf + 1, hidden_tag_name)) {
                    in_hidden = 0;
                    hidden_tag_name[0] = 0;
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
    
    int visual_nodes = 0;
    for(int k=0; k<node_count; k++) {
        if (nodes[k].type == 0) {
            int len = sys->strlen(nodes[k].text);
            int is_whitespace = 1;
            for (int m=0; m<len; m++) {
                if (nodes[k].text[m] != ' ' && nodes[k].text[m] != '\n' && nodes[k].text[m] != '\r') is_whitespace = 0;
            }
            if (!is_whitespace) visual_nodes++;
        }
        if (nodes[k].type == 1 && (is_tag(nodes[k].tag, "img") || is_tag(nodes[k].tag, "input") || is_tag(nodes[k].tag, "button"))) visual_nodes++;
    }
    
    if (visual_nodes == 0 && node_count < MAX_NODES) {
        dom_node_t* n = &nodes[node_count++];
        sys->memset(n, 0, sizeof(dom_node_t));
        n->type = 0;
        sys->strcpy(n->text, "The site requires JavaScript (Single Page App) to render content.");
        sys->strcpy(n->tag, "text");
        n->color = 0xFFFF0000;
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
            } else if(sys->strcmp(n->tag, "br") == 0 || sys->strcmp(n->tag, "p") == 0 || sys->strcmp(n->tag, "div") == 0) {
                cy += current_font + 4;
                cx = 10;
            } else if (is_tag(n->tag, "img") || is_tag(n->tag, "video") || is_tag(n->tag, "audio")) {
                if (cx > 10) { cx = 10; cy += 20; }
                n->x = cx;
                n->y = cy;
                n->w = 100;
                n->h = 60;
                cy += 70;
            }
            if (node_count < MAX_NODES) nodes[node_count++] = *n;
        } else if(n->type == 2) { 
            if(sys->strcmp(n->tag, "h1") == 0 || sys->strcmp(n->tag, "p") == 0 || sys->strcmp(n->tag, "div") == 0) {
                cy += current_font + 4;
                cx = 10;
                if (sys->strcmp(n->tag, "h1") == 0) current_font = 16;
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
    if (sys->strstr(tabs[active_tab].url, "://") == 0) {
        sys->sprintf(f_final_url, "https://%s", tabs[active_tab].url);
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
    
    is_loading = 1;
    meta_refresh_triggered = 0;
    meta_refresh_url[0] = 0;
    
    // --- PROACTIVE GOOGLE BYPASS ---
    if (sys->strstr(f_final_url, "google.") != 0 && sys->strstr(f_final_url, "gbv=1") == 0) {
        if (sys->strstr(f_final_url, "?")) {
            if (sys->strlen(f_final_url) < MAX_URL - 7) sys->sprintf(f_temp, "%s&gbv=1", f_final_url);
        } else {
            if (sys->strlen(f_final_url) < MAX_URL - 7) sys->sprintf(f_temp, "%s?gbv=1", f_final_url);
        }
        sys->strcpy(f_final_url, f_temp);
        sys->strcpy(tabs[active_tab].url, f_final_url); 
        tabs[active_tab].url_len = sys->strlen(f_final_url);
        sys->print("[BYPASS] Auto-injected Google Basic Version (gbv=1) to prevent Cookie loops.\n");
    }
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
            } else if (has_moved_text && f_first_link[0] && node_count < 30) {
                // If it's a tiny page saying "Moved" and it has a link, follow it immediately
                sys->strcpy(f_next_url, f_first_link);
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
                            int split = 0;
                            for(int i=0; f_final_url[i]; i++) {
                                if(f_final_url[i] == '/') slashes++;
                                if(slashes == 3) { split = i; break; }
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
                        sys->strcpy(err->text, "Error: The site redirected to itself in an infinite loop.");
                        sys->strcpy(err->tag, "text");
                        err->color = 0xFFFF0000;
                        err->font_size = 16;
                        
                        err = &nodes[node_count++];
                        sys->memset(err, 0, sizeof(dom_node_t));
                        err->type = 0;
                        sys->strcpy(err->text, "(This typically happens because the site requires browser cookies to continue)");
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
    
    sys->strcpy(tabs[0].url, "google.com");
    tabs[0].url_len = sys->strlen(tabs[0].url);
    active_tab = 0;
    tab_count = 1;
    
    frame_buffer = (uint32_t*)sys->malloc(CONTENT_W * CONTENT_H * 4);
    if (frame_buffer) {
        sys->memset(frame_buffer, 0, CONTENT_W * CONTENT_H * 4);
    }
    
    main_win = sys->create_window("Camel Browser", BROWSER_W, BROWSER_H, on_paint, on_input, on_mouse);
    
    js_vm = js_create(js_mem, sizeof(js_mem));
    if (js_vm) {
        js_set(js_vm, js_glob(js_vm), "alert", js_mkfun(js_alert));
    }
    
    return &exports;
}
