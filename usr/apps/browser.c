// usr/apps/browser.c - CamelOS Browser App (Massively Upgraded v2)
// Safari-style tabs, find on page, view source, dev tools, context menu, zoom,
// beautiful new tab page with greeting, progress bar, reading mode, history sidebar,
// improved rendering with proper heading sizes, image placeholders, table borders,
// enhanced status bar, shortcut hints bar, better error pages with retry,
// improved tab bar with hover close, home button, download indicator
// NO floating point - integer math only (-msoft-float -mno-80387)
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../../hal/cpu/timer.h"
#include "../../core/tcp.h"
#include "../../core/socket.h"
#include "../../core/tls.h"
#include "../../core/http.h"
#include "../dock.h"
#include "../../core/window_server.h"
#include "../libs/browser_dom.h"
// browser_enhanced.c — external resource loader (<link rel=stylesheet>,
// <script src=...>). Was compiled but never called before, which is why
// external CSS/JS were silently dropped and only inline <style>/<script>
// were parsed. We now wire it in after dom_parse_html succeeds.
#include "../libs/browser_bridge.h"
extern void browser_set_current_url_for_resources(const char* url);
extern void browser_process_link_tags(const char* html);
extern void browser_process_script_tags(const char* html, char* inline_scripts, int max_inline_len);
#include "mujs.h"
#include "../../common/serial.h"

// ============================================================
// SECTION 1: Constants and Layout
// ============================================================

#define TAB_BAR_H      34
#define URL_BAR_H      42
#define STATUS_BAR_H   24
#define SHORTCUT_BAR_H 20
#define PAD            8
#define BOOKMARK_BAR_H 26
#define FIND_BAR_H     32
#define DEV_TOOLS_H    200

#define PAGE_LINES     120
#define PAGE_LINE_LEN  256
#define MAX_LINKS      48
#define HISTORY_MAX    32
#define BOOKMARK_MAX   12
#define MAX_TABS       8
#define MAX_FIND_MATCHES 64
#define CTX_MENU_MAX_ITEMS 12
#define DEV_CONSOLE_LINES 20

// Line types for CSS-like visual formatting
#define LINE_NORMAL   0
#define LINE_H1       1
#define LINE_H2       2
#define LINE_H3       3
#define LINE_H4       4
#define LINE_H5       5
#define LINE_H6       6
#define LINE_LI       7
#define LINE_HR       8
#define LINE_QUOTE    9
#define LINE_PRE      10

// Error types
#define ERR_NONE       0
#define ERR_DNS        1
#define ERR_CONNECT    2
#define ERR_TLS        3
#define ERR_SOCKET     4
#define ERR_MEMORY     5
#define ERR_SEND       6
#define ERR_REDIRECT   7

// Context menu action IDs
#define CTX_ACTION_BACK         1
#define CTX_ACTION_FORWARD      2
#define CTX_ACTION_RELOAD       3
#define CTX_ACTION_BOOKMARK     4
#define CTX_ACTION_COPY_URL     5
#define CTX_ACTION_VIEW_SOURCE  6
#define CTX_ACTION_INSPECT      7

// Color palette (macOS Safari-inspired, ARGB uint32_t)
#define COL_TAB_BAR_BG       0xFFE8E8ED
#define COL_TAB_ACTIVE_BG    0xFFFFFFFF
#define COL_TAB_INACTIVE_BG  0xFFD4D4D8
#define COL_URL_BAR_BG       0xFFFFFFFF
#define COL_URL_BAR_BORDER   0xFFC6C6C8
#define COL_URL_BAR_FOCUS    0xFF007AFF
#define COL_ACCENT_BLUE      0xFF007AFF
#define COL_TOOLBAR_BG       0xFFF2F2F7
#define COL_STATUS_BAR_BG    0xFFF2F2F7
#define COL_TEXT_DARK        0xFF333333
#define COL_TEXT_MUTED       0xFF888888
#define COL_LINK_BLUE        0xFF007AFF
#define COL_ERROR_RED        0xFFFF3B30
#define COL_SUCCESS_GREEN    0xFF34C759
#define COL_WARNING_ORANGE   0xFFFF9500
#define COL_FIND_HIGHLIGHT   0xFFFFFF00
#define COL_FIND_CURRENT     0xFFFF9500
#define COL_SEPARATOR        0xFFC6C6C8
#define COL_NTP_BG_TOP       0xFFF5F5FA
#define COL_NTP_BG_BOTTOM    0xFFE8E8ED

// ============================================================
// SECTION 2: Tab Data Structure
// ============================================================

typedef struct {
    int active;
    char url[256];
    char page_title[64];
    char status_text[64];
    char page_lines[PAGE_LINES][PAGE_LINE_LEN];
    int page_line_count;
    int line_types[PAGE_LINES];
    int scroll_offset;
    struct {
        int line;
        int col;
        int len;
        char url[256];
    } links[MAX_LINKS];
    int link_count;
    char history[HISTORY_MAX][256];
    int history_pos;
    int history_count;
    int is_loading;
    int load_progress;
    int redirect_depth;
    dom_document_t* dom_doc;
    int use_dom_rendering;
    int download_progress;
    int download_active;
    char download_filename[64];
    int download_is_app;
    int view_source;
    char* source_html;
    int source_len;
    int url_cursor;
    int error_type;
    char error_detail[128];
} browser_tab_t;

static browser_tab_t tabs[MAX_TABS];
static int active_tab = 0;
static int tab_count = 0;

// ============================================================
// SECTION 3: Global State (mirrors active tab)
// ============================================================

static char url_buf[256] = "";
static int url_cursor = 0;
static int url_active = 1;

static char page_lines[PAGE_LINES][PAGE_LINE_LEN];
static int page_line_count = 0;
static int scroll_offset = 0;
static char status_text[64] = "Ready";

static int line_types[PAGE_LINES];

static int is_loading = 0;
static int load_progress = 0;

static int browser_win_w = 700;
static int browser_win_h = 500;

static int download_progress = 0;
static int download_active = 0;
static char download_filename[64] = "";
static int download_is_app = 0;

static char history[HISTORY_MAX][256];
static int history_pos = -1;
static int history_count = 0;
static int redirect_depth = 0;

static struct {
    char name[32];
    char url[256];
} bookmarks[BOOKMARK_MAX] = {
    {"CamelOS",    "http://camelos.local"},
    {"Example",    "http://example.com"},
    {"GitHub",     "http://github.com"},
    {"Wikipedia",  "http://wikipedia.org"},
};
static int bookmark_count = 4;
static int show_bookmarks = 0;

static char page_title[64] = "";

static Window* browser_window = 0;

static struct {
    int line;
    int col;
    int len;
    char url[256];
} links[MAX_LINKS];
static int link_count = 0;
static int hovered_link = -1;

static dom_document_t* dom_doc = 0;
static js_State* js_state = 0;
static int use_dom_rendering = 0;

static int view_source = 0;
static char* source_html = 0;
static int source_len = 0;

static int error_type = ERR_NONE;
static char error_detail[128] = "";

// --- New Tab Page shortcuts ---
static struct {
    char name[20];
    char url[128];
    uint32_t color;
} shortcuts[8] = {
    {"CamelOS",        "http://camelos.local",           0xFFFF6B35},
    {"Wikipedia",      "http://wikipedia.org",           0xFFEEEEEE},
    {"GitHub",         "http://github.com",              0xFF333333},
    {"Example",        "http://example.com",             0xFF4CAF50},
    {"Hacker News",    "http://news.ycombinator.com",    0xFFFF6600},
    {"Reddit",         "http://reddit.com",              0xFFFF4500},
    {"Stack Overflow", "http://stackoverflow.com",       0xFFF48024},
    {"DuckDuckGo",     "http://duckduckgo.com",          0xFFDE5833},
};

// --- Find on Page state ---
static int find_active = 0;
static char find_query[64] = "";
static int find_cursor = 0;
static struct {
    int line;
    int col;
    int len;
} find_matches[MAX_FIND_MATCHES];
static int find_match_count = 0;
static int find_current_match = -1;

// --- Context Menu state ---
static int ctx_menu_active = 0;
static int ctx_menu_x = 0;
static int ctx_menu_y = 0;
static int ctx_menu_hovered = -1;
static struct {
    char label[32];
    int is_separator;
    int action_id;
} ctx_menu_items[CTX_MENU_MAX_ITEMS];
static int ctx_menu_count = 0;

// --- Developer Tools state ---
static int dev_tools_active = 0;
static int dev_tools_tab = 0;
static char dev_console[DEV_CONSOLE_LINES][128];
static int dev_console_count = 0;

// --- Zoom state ---
static int zoom_level = 1;
static const int zoom_line_h[3] = {12, 16, 20};
static const int zoom_pct[3] = {75, 100, 125};

// --- Reading mode state ---
static int reading_mode = 0;

// --- History sidebar state ---
static int history_sidebar_active = 0;

// --- Page load timer ---
static uint32_t page_load_start = 0;
static uint32_t page_load_time = 0;  // ms
static int page_size_bytes = 0;

// --- Recently visited for NTP ---
static struct {
    char title[48];
    char url[128];
    uint32_t visit_time;
} recent_visits[6];
static int recent_visit_count = 0;

static void browser_navigate(const char* url);
static void browser_load_page(const char* url);

// ============================================================
// SECTION 4: Tab Management
// ============================================================

static void tab_save_state(void) {
    browser_tab_t* t = &tabs[active_tab];
    strncpy(t->url, url_buf, 255); t->url[255] = 0;
    strncpy(t->page_title, page_title, 63); t->page_title[63] = 0;
    strncpy(t->status_text, status_text, 63); t->status_text[63] = 0;
    memcpy(t->page_lines, page_lines, sizeof(page_lines));
    t->page_line_count = page_line_count;
    memcpy(t->line_types, line_types, sizeof(line_types));
    t->scroll_offset = scroll_offset;
    memcpy(t->links, links, sizeof(links));
    t->link_count = link_count;
    memcpy(t->history, history, sizeof(history));
    t->history_pos = history_pos;
    t->history_count = history_count;
    t->is_loading = is_loading;
    t->load_progress = load_progress;
    t->redirect_depth = redirect_depth;
    t->dom_doc = dom_doc;
    t->use_dom_rendering = use_dom_rendering;
    t->download_progress = download_progress;
    t->download_active = download_active;
    strncpy(t->download_filename, download_filename, 63); t->download_filename[63] = 0;
    t->download_is_app = download_is_app;
    t->view_source = view_source;
    t->source_html = source_html;
    t->source_len = source_len;
    t->url_cursor = url_cursor;
    t->error_type = error_type;
    strncpy(t->error_detail, error_detail, 127); t->error_detail[127] = 0;
}

static void tab_load_state(int idx) {
    browser_tab_t* t = &tabs[idx];
    strncpy(url_buf, t->url, 255); url_buf[255] = 0;
    strncpy(page_title, t->page_title, 63); page_title[63] = 0;
    strncpy(status_text, t->status_text, 63); status_text[63] = 0;
    memcpy(page_lines, t->page_lines, sizeof(page_lines));
    page_line_count = t->page_line_count;
    memcpy(line_types, t->line_types, sizeof(line_types));
    scroll_offset = t->scroll_offset;
    memcpy(links, t->links, sizeof(links));
    link_count = t->link_count;
    memcpy(history, t->history, sizeof(history));
    history_pos = t->history_pos;
    history_count = t->history_count;
    is_loading = t->is_loading;
    load_progress = t->load_progress;
    redirect_depth = t->redirect_depth;
    dom_doc = t->dom_doc;
    use_dom_rendering = t->use_dom_rendering;
    download_progress = t->download_progress;
    download_active = t->download_active;
    strncpy(download_filename, t->download_filename, 63); download_filename[63] = 0;
    download_is_app = t->download_is_app;
    view_source = t->view_source;
    source_html = t->source_html;
    source_len = t->source_len;
    url_cursor = t->url_cursor;
    error_type = t->error_type;
    strncpy(error_detail, t->error_detail, 127); error_detail[127] = 0;
}

static void tab_switch(int new_idx) {
    if (new_idx < 0 || new_idx >= MAX_TABS) return;
    if (!tabs[new_idx].active) return;
    if (new_idx == active_tab) return;
    tab_save_state();
    active_tab = new_idx;
    tab_load_state(active_tab);
    // Update window title
    if (browser_window) {
        if (page_title[0]) {
            char wt[80]; strcpy(wt, page_title); strcat(wt, " - Browser");
            ws_set_title((window_t*)browser_window, wt);
        } else {
            ws_set_title((window_t*)browser_window, "Browser");
        }
    }
}

static int tab_create(const char* url) {
    int idx = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;
    tab_save_state();
    memset(&tabs[idx], 0, sizeof(browser_tab_t));
    tabs[idx].active = 1;
    if (url) { strncpy(tabs[idx].url, url, 255); tabs[idx].url[255] = 0; }
    else { tabs[idx].url[0] = 0; }
    strcpy(tabs[idx].status_text, "Ready");
    tabs[idx].history_pos = -1;
    tabs[idx].scroll_offset = 0;
    tab_count++;
    active_tab = idx;
    tab_load_state(idx);
    return idx;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= MAX_TABS) return;
    if (!tabs[idx].active) return;
    if (tab_count <= 1) return;
    if (tabs[idx].dom_doc) { dom_document_destroy(tabs[idx].dom_doc); tabs[idx].dom_doc = 0; }
    if (tabs[idx].source_html) { kfree(tabs[idx].source_html); tabs[idx].source_html = 0; }
    tabs[idx].active = 0;
    tab_count--;
    if (idx == active_tab) {
        int new_idx = -1;
        for (int i = idx - 1; i >= 0; i--) { if (tabs[i].active) { new_idx = i; break; } }
        if (new_idx < 0) { for (int i = idx + 1; i < MAX_TABS; i++) { if (tabs[i].active) { new_idx = i; break; } } }
        if (new_idx >= 0) { active_tab = new_idx; tab_load_state(active_tab); }
    }
}

// ============================================================
// SECTION 5: JavaScript Browser API Callbacks (mujs C functions)
// ============================================================

// console.log() implementation
static void js_browser_console_log(js_State* J) {
    const char* msg = js_tostring(J, 1);
    s_printf("[JS] %s\n", msg ? msg : "undefined");
    js_pushundefined(J);
}

// document.getElementById(id) implementation
static void js_browser_doc_getElementById(js_State* J) {
    const char* id = js_tostring(J, 1);
    if (!id || !dom_doc) { js_pushnull(J); return; }
    char selector[128];
    selector[0] = '#';
    int i = 0;
    while (id[i] && i < 126) { selector[i+1] = id[i]; i++; }
    selector[i+1] = 0;
    dom_node_t* node = dom_query_selector(dom_doc, selector);
    if (!node) { js_pushnull(J); return; }
    js_newobject(J);
    if (node->tag[0]) { js_pushstring(J, node->tag); js_setproperty(J, -2, "tagName"); }
    js_pushstring(J, id); js_setproperty(J, -2, "id");
}

// document.querySelector(selector) implementation
static void js_browser_doc_querySelector(js_State* J) {
    const char* sel = js_tostring(J, 1);
    if (!sel || !dom_doc) { js_pushnull(J); return; }
    dom_node_t* node = dom_query_selector(dom_doc, sel);
    if (!node) { js_pushnull(J); return; }
    js_newobject(J);
    if (node->tag[0]) { js_pushstring(J, node->tag); js_setproperty(J, -2, "tagName"); }
}

// document.createElement(tag) implementation
static void js_browser_doc_createElement(js_State* J) {
    const char* tag = js_tostring(J, 1);
    if (!tag) { js_pushnull(J); return; }
    dom_node_t* node = dom_create_element(tag);
    js_newobject(J);
    js_pushstring(J, tag); js_setproperty(J, -2, "tagName");
    js_pushstring(J, ""); js_setproperty(J, -2, "innerHTML");
    js_pushstring(J, ""); js_setproperty(J, -2, "id");
    // Store native ptr for later DOM manipulation
    if (node) {
        js_pushnumber(J, (double)(uintptr_t)node);
        js_setproperty(J, -2, "__native_ptr");
    }
}

// document.write(html) implementation
static void js_browser_doc_write(js_State* J) {
    const char* html = js_tostring(J, 1);
    if (html) s_printf("[JS] document.write: %s\n", html);
    js_pushundefined(J);
}

// Timer callback system for setTimeout/setInterval
#define MAX_TIMER_SLOTS 16
static struct {
    int active;
    int is_interval;
    uint32_t start_tick;
    uint32_t delay_ticks;  // in system ticks (50Hz)
    char registry_key[32];
    int timer_id;
} browser_timer_slots[MAX_TIMER_SLOTS];
static int browser_next_timer_id = 1;

// Process any pending timer callbacks (call from event loop / after script execution)
static void js_browser_process_timers(js_State* J) {
    if (!J) return;
    uint32_t now = get_tick_count();
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (!browser_timer_slots[i].active) continue;
        if ((now - browser_timer_slots[i].start_tick) >= browser_timer_slots[i].delay_ticks) {
            js_getregistry(J, browser_timer_slots[i].registry_key);
            if (js_iscallable(J, -1)) {
                js_pcall(J, 0);
                js_pop(J, 1);  // pop result
            } else {
                js_pop(J, 1);
            }
            if (browser_timer_slots[i].is_interval) {
                browser_timer_slots[i].start_tick = now;
            } else {
                js_delregistry(J, browser_timer_slots[i].registry_key);
                browser_timer_slots[i].active = 0;
            }
        }
    }
}

// window.setTimeout(fn, ms) — stores callback in mujs registry, fires after delay
static void js_browser_window_setTimeout(js_State* J) {
    int ms = js_tointeger(J, 2);
    if (ms < 0) ms = 0;
    int slot = -1;
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (!browser_timer_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) { js_pushnumber(J, 0); return; }

    int id = browser_next_timer_id++;
    char key[32];
    snprintf(key, sizeof(key), "timer_cb_%d", id);
    js_copy(J, 1);           // copy callback arg to top of stack
    js_setregistry(J, key);  // store in registry for later retrieval

    browser_timer_slots[slot].active = 1;
    browser_timer_slots[slot].is_interval = 0;
    browser_timer_slots[slot].start_tick = get_tick_count();
    // Convert ms to ticks (50Hz => 1 tick = 20ms); ensure at least 1 tick for any non-zero delay
    browser_timer_slots[slot].delay_ticks = (ms + 19) / 20;
    if (browser_timer_slots[slot].delay_ticks == 0) browser_timer_slots[slot].delay_ticks = 1;
    strncpy(browser_timer_slots[slot].registry_key, key, 31);
    browser_timer_slots[slot].registry_key[31] = 0;
    browser_timer_slots[slot].timer_id = id;

    js_pushnumber(J, (double)id);
}

// window.setInterval(fn, ms) — stores callback in mujs registry, fires repeatedly
static void js_browser_window_setInterval(js_State* J) {
    int ms = js_tointeger(J, 2);
    if (ms < 1) ms = 1;
    int slot = -1;
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (!browser_timer_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) { js_pushnumber(J, 0); return; }

    int id = browser_next_timer_id++;
    char key[32];
    snprintf(key, sizeof(key), "interval_cb_%d", id);
    js_copy(J, 1);
    js_setregistry(J, key);

    browser_timer_slots[slot].active = 1;
    browser_timer_slots[slot].is_interval = 1;
    browser_timer_slots[slot].start_tick = get_tick_count();
    browser_timer_slots[slot].delay_ticks = (ms + 19) / 20;
    if (browser_timer_slots[slot].delay_ticks == 0) browser_timer_slots[slot].delay_ticks = 1;
    strncpy(browser_timer_slots[slot].registry_key, key, 31);
    browser_timer_slots[slot].registry_key[31] = 0;
    browser_timer_slots[slot].timer_id = id;

    js_pushnumber(J, (double)id);
}

// alert(msg) implementation
static void js_browser_alert(js_State* J) {
    const char* msg = js_tostring(J, 1);
    s_printf("[JS] alert: %s\n", msg ? msg : "");
    js_pushundefined(J);
}

// parseInt(str) implementation
static void js_browser_parseInt(js_State* J) {
    const char* s = js_tostring(J, 1);
    int val = 0;
    if (s) { while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; } }
    js_pushnumber(J, (double)val);
}

// ============================================================
// SECTION 6: HTTP Fetch with HTTPS support
// ============================================================

#define BROWSER_RESPONSE_SIZE 524288  // 512KB — bumped from 128KB. Many real pages
                                         // (Google, YouTube, news sites) are 200-500KB
                                         // decompressed. The 128KB cap silently truncated
                                         // them mid-body, often losing the entire <head>
                                         // where stylesheets and scripts are referenced.

// Decode chunked transfer-encoding in-place
static int decode_chunked(char* body, int body_len) {
    char* src = body;
    char* dst = body;
    char* end = body + body_len;
    
    while (src < end) {
        // Read chunk size (hex)
        int chunk_size = 0;
        while (src < end && *src != '\r' && *src != '\n') {
            char c = *src++;
            if (c >= '0' && c <= '9') chunk_size = chunk_size * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') chunk_size = chunk_size * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk_size = chunk_size * 16 + (c - 'A' + 10);
        }
        // Skip CRLF after chunk size
        while (src < end && (*src == '\r' || *src == '\n')) src++;
        
        if (chunk_size == 0) break; // Last chunk
        
        // Copy chunk data
        if (src + chunk_size > end) chunk_size = end - src;
        for (int i = 0; i < chunk_size; i++) *dst++ = *src++;
        
        // Skip CRLF after chunk data
        while (src < end && (*src == '\r' || *src == '\n')) src++;
    }
    
    *dst = '\0';
    return dst - body;
}

static void browser_load_page(const char* url) {
    // Empty URL = new tab page
    if (!url || url[0] == 0) {
        page_line_count = 0;
        scroll_offset = 0;
        is_loading = 0;
        load_progress = 100;
        link_count = 0;
        page_title[0] = 0;
        if (source_html) { kfree(source_html); source_html = 0; source_len = 0; }
        view_source = 0;
        error_type = ERR_NONE;
        error_detail[0] = 0;
        strcpy(status_text, "New Tab");
        return;
    }

    page_line_count = 0;
    scroll_offset = 0;
    is_loading = 1;
    load_progress = 5;
    link_count = 0;
    page_title[0] = 0;
    error_type = ERR_NONE;
    error_detail[0] = 0;
    strcpy(status_text, "Loading...");
    page_load_start = get_tick_count();

    if (dom_doc) { dom_document_destroy(dom_doc); dom_doc = 0; }
    use_dom_rendering = 0;
    if (source_html) { kfree(source_html); source_html = 0; source_len = 0; }
    view_source = 0;

    http_process_events();

    int use_tls = 0;
    char host[128] = "";
    char path[128] = "/";
    int port = 80;

    const char* url_start = url;
    if (strncmp(url, "https://", 8) == 0) {
        use_tls = 1; url_start = url + 8; port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        url_start = url + 7;
    }

    int hi = 0;
    while (*url_start && *url_start != '/' && hi < 127) host[hi++] = *url_start++;
    host[hi] = 0;

    char* colon = strchr(host, ':');
    if (colon) {
        *colon = 0; port = 0; colon++;
        while (*colon >= '0' && *colon <= '9') { port = port * 10 + (*colon - '0'); colon++; }
        if (port == 0) port = use_tls ? 443 : 80;
    }

    if (*url_start == '/') { strncpy(path, url_start, 127); path[127] = 0; }

    int path_len = strlen(path);
    if (path_len > 0 && path[path_len - 1] == '/') {
        if (path_len + 10 < 127) strcat(path, "index.html");
    }
    char original_path[128];
    strncpy(original_path, path, 127); original_path[127] = 0;

    // DNS
    char ip_str[16];
    extern int dns_resolve(const char* name, char* ip_buf, int ip_buf_len);
    s_printf("[Browser] DNS resolve: '%s'\n", host);
    int dns_ok = dns_resolve(host, ip_str, sizeof(ip_str));

    if (dns_ok != 0) {
        s_printf("[Browser] DNS FAILED for '%s' (err=%d)\n", host, dns_ok);
        error_type = ERR_DNS;
        strncpy(error_detail, host, 127); error_detail[127] = 0;
        page_line_count = 0;
        strcpy(status_text, "DNS Error");
        is_loading = 0; load_progress = 0;
        return;
    }
    s_printf("[Browser] DNS OK: '%s' -> %s\n", host, ip_str);

    load_progress = 15;

    // Repaint after DNS resolution so user sees progress
    http_process_events();
    if (browser_window) {
        window_t* bw = (window_t*)browser_window;
        if (bw->paint_callback) {
            typedef void (*pcb)(window_t*,int,int,int,int);
            extern uint32_t* gfx_get_active_buffer(void);
            uint32_t* fb = gfx_get_active_buffer();
            if (fb) ((pcb)bw->paint_callback)(bw, bw->x, bw->y + 38, bw->width, bw->height - 38);
        }
        extern void gfx_swap_buffers(void);
        gfx_swap_buffers();
    }

    extern uint32_t ip_parse(const char* str);
    uint32_t ip = ip_parse(ip_str);

    int sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        error_type = ERR_SOCKET;
        strncpy(error_detail, host, 127); error_detail[127] = 0;
        page_line_count = 0;
        strcpy(status_text, "Socket Error");
        is_loading = 0; load_progress = 0;
        return;
    }

    sockaddr_in_t server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = ip;

    strcpy(status_text, "Connecting...");

    // Process GUI events during connection to keep cursor/UI responsive
    http_process_events();
    if (browser_window) {
        window_t* bw = (window_t*)browser_window;
        if (bw->paint_callback) {
            typedef void (*pcb)(window_t*,int,int,int,int);
            extern uint32_t* gfx_get_active_buffer(void);
            uint32_t* fb = gfx_get_active_buffer();
            if (fb) ((pcb)bw->paint_callback)(bw, bw->x, bw->y + 38, bw->width, bw->height - 38);
        }
        extern void gfx_swap_buffers(void);
        gfx_swap_buffers();
    }

    if (k_connect(sockfd, &server_addr) < 0) {
        k_close(sockfd);
        error_type = ERR_CONNECT;
        strncpy(error_detail, host, 127); error_detail[127] = 0;
        page_line_count = 0;
        strcpy(status_text, "Connection Error");
        is_loading = 0; load_progress = 0;
        return;
    }

    load_progress = 25;

    tls_session_t* tls_session = 0;
    if (use_tls) {
        strcpy(status_text, "Establishing secure connection...");
        extern tls_session_t* tls_client_handshake_fd(int sockfd, const char* hostname, uint16_t port);
        tls_session = tls_client_handshake_fd(sockfd, host, port);

        if (!tls_session) {
            k_close(sockfd);
            // TLS handshake failed - warn but do NOT silently fall back to HTTP
            s_printf("[BROWSER] WARNING: TLS handshake failed for %s. Connection not secure.\n", url);
            // Don't automatically fall back to HTTP - return error instead
            // User can explicitly request HTTP if needed
            error_type = ERR_TLS;
            strncpy(error_detail, host, 127); error_detail[127] = 0;
            page_line_count = 0;
            strcpy(status_text, "TLS Error - Connection not secure");
            is_loading = 0; load_progress = 0;
            return;
        }
        load_progress = 35;
        strcpy(status_text, "Secure connection established");
    }

    // Repaint after connection/TLS so user sees progress
    http_process_events();
    if (browser_window) {
        window_t* bw = (window_t*)browser_window;
        if (bw->paint_callback) {
            typedef void (*pcb)(window_t*,int,int,int,int);
            extern uint32_t* gfx_get_active_buffer(void);
            uint32_t* fb = gfx_get_active_buffer();
            if (fb) ((pcb)bw->paint_callback)(bw, bw->x, bw->y + 38, bw->width, bw->height - 38);
        }
        extern void gfx_swap_buffers(void);
        gfx_swap_buffers();
    }

    char request[768];
    int rlen = 0;
    rlen += sprintf(request + rlen, "GET %s HTTP/1.1\r\n", path);
    rlen += sprintf(request + rlen, "Host: %s\r\n", host);
    rlen += sprintf(request + rlen, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n");
    rlen += sprintf(request + rlen, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n");
    rlen += sprintf(request + rlen, "Accept-Language: en-US,en;q=0.9\r\n");
    rlen += sprintf(request + rlen, "Accept-Encoding: gzip, deflate\r\n");
    rlen += sprintf(request + rlen, "Connection: close\r\n");
    rlen += sprintf(request + rlen, "Upgrade-Insecure-Requests: 1\r\n");
    rlen += sprintf(request + rlen, "\r\n");

    int send_result;
    if (use_tls && tls_session) send_result = tls_write(tls_session, request, rlen);
    else send_result = k_sendto(sockfd, request, rlen, 0, NULL);

    if (send_result < 0) {
        if (tls_session) { extern void tls_client_session_close(tls_session_t*); tls_client_session_close(tls_session); }
        k_close(sockfd);
        error_type = ERR_SEND;
        strncpy(error_detail, host, 127); error_detail[127] = 0;
        page_line_count = 0;
        strcpy(status_text, "Send Error");
        is_loading = 0; load_progress = 0;
        return;
    }

    load_progress = 45;

    char* response = (char*)kmalloc(BROWSER_RESPONSE_SIZE);
    if (!response) {
        if (tls_session) { extern void tls_client_session_close(tls_session_t*); tls_client_session_close(tls_session); }
        k_close(sockfd);
        error_type = ERR_MEMORY;
        page_line_count = 0;
        strcpy(status_text, "Memory Error");
        is_loading = 0; load_progress = 0;
        return;
    }
    int total_read = 0;

    uint32_t browser_recv_start = get_tick_count();
    #define BROWSER_RECV_TIMEOUT 30000  // 30 seconds — allow more time for slow connections
    extern int sys_get_key(void);  // from hal/drivers/keyboard.c
    for (int retry = 0; retry < 30000 && total_read < BROWSER_RESPONSE_SIZE - 1; retry++) {
        if (get_tick_count() - browser_recv_start > BROWSER_RECV_TIMEOUT) break;

        // Escape cancels the in-flight request. Previously the input callback
        // (which checks is_loading) was never dispatched during a fetch, so
        // the user could not abort a stuck page load. Now we drain the key
        // queue directly from the recv loop.
        int k = sys_get_key();
        if (k == 27) {  // ESC
            strcpy(status_text, "Stopped");
            s_printf("[Browser] Load cancelled by user (ESC)\n");
            break;
        }

        // Poll NIC in bursts to drain packets faster
        for (int p = 0; p < 8; p++) { extern void rtl8139_poll(); rtl8139_poll(); }
        int n;
        if (use_tls && tls_session) n = tls_read(tls_session, response + total_read, BROWSER_RESPONSE_SIZE - total_read - 1);
        else n = k_recvfrom(sockfd, response + total_read, BROWSER_RESPONSE_SIZE - total_read - 1, 0, NULL);
        if (n > 0) {
            total_read += n;
            s_printf("[Browser] recv: %d bytes (total=%d)\n", n, total_read);
            browser_recv_start = get_tick_count();
            int prog = 45 + (total_read * 45) / BROWSER_RESPONSE_SIZE;
            if (prog > 90) prog = 90;
            load_progress = prog;
        }
        else if (n == 0) {
            s_printf("[Browser] recv EOF (total_read=%d)\n", total_read);
            break;
        }
        else {
            // Yield CPU properly by processing network/GUI events
            http_process_events();
        }
        // Pump GUI events every iteration. Previously this was throttled to
        // every 8th iteration (~160ms+ between pumps when k_recvfrom blocks),
        // which made the system feel unresponsive and starved the timer IRQ's
        // TCP retransmit path. http_process_events() already throttles via
        // timer_sleep(1) (~20ms), so calling it every iteration is cheap.
        http_process_events();

        // Repaint the browser window every iteration so the progress bar
        // animates smoothly and the user sees continuous feedback. The paint
        // callback is fast (clipped to dirty regions by gfx_hal) and the
        // cost is dominated by gfx_swap_buffers, which we already pay.
        if (browser_window) {
            window_t* bw = (window_t*)browser_window;
            if (bw->paint_callback) {
                typedef void (*pcb)(window_t*,int,int,int,int);
                extern uint32_t* gfx_get_active_buffer(void);
                uint32_t* fb = gfx_get_active_buffer();
                if (fb) {
                    ((pcb)bw->paint_callback)(bw, bw->x, bw->y + 38, bw->width, bw->height - 38);
                }
            }
            extern void gfx_swap_buffers(void);
            gfx_swap_buffers();
        }
    }
    response[total_read] = 0;

    if (tls_session) { extern void tls_client_session_close(tls_session_t*); tls_client_session_close(tls_session); }
    k_close(sockfd);

    load_progress = 92;

    // Parse HTTP status
    int http_status = 0;
    if (strncmp(response, "HTTP/", 5) == 0) {
        char* sp = strchr(response, ' ');
        if (sp) { sp++; while (*sp >= '0' && *sp <= '9') { http_status = http_status * 10 + (*sp - '0'); sp++; } }
    }

    // Handle redirects
    if (http_status == 301 || http_status == 302 || http_status == 303 || http_status == 307 || http_status == 308) {
        char* location = NULL;
        char* h = response;
        while (*h) {
            if (h == response || *(h-1) == '\n') {
                if ((h[0]=='L'||h[0]=='l')&&(h[1]=='o'||h[1]=='O')&&(h[2]=='c'||h[2]=='C')&&
                    (h[3]=='a'||h[3]=='A')&&(h[4]=='t'||h[4]=='T')&&(h[5]=='i'||h[5]=='I')&&
                    (h[6]=='o'||h[6]=='O')&&(h[7]=='n'||h[7]=='N')&&h[8]==':') {
                    location = h + 9;
                    while (*location == ' ' || *location == '\t') location++;
                    break;
                }
            }
            h++;
        }
        if (location) {
            char redirect_url[256]; int ri = 0;
            while (location[ri] && location[ri] != '\r' && location[ri] != '\n' && ri < 255) { redirect_url[ri] = location[ri]; ri++; }
            redirect_url[ri] = 0;

            if (redirect_url[0] == '/' && redirect_url[1] == '/') {
                char resolved[256]; strcpy(resolved, use_tls ? "https:" : "http:"); strcat(resolved, redirect_url);
                strncpy(redirect_url, resolved, 255); redirect_url[255] = 0;
            } else if (redirect_url[0] == '/') {
                char resolved[256]; strcpy(resolved, use_tls ? "https://" : "http://"); strcat(resolved, host); strcat(resolved, redirect_url);
                strncpy(redirect_url, resolved, 255); redirect_url[255] = 0;
            } else if (redirect_url[0] != 'h' || strncmp(redirect_url, "http", 4) != 0) {
                char resolved[256]; strcpy(resolved, use_tls ? "https://" : "http://"); strcat(resolved, host);
                char last_path[128]; strncpy(last_path, path, 127); last_path[127] = 0;
                char* last_slash = strrchr(last_path, '/');
                if (last_slash) { *(last_slash + 1) = 0; strcat(resolved, last_path); } else strcat(resolved, "/");
                strcat(resolved, redirect_url);
                strncpy(redirect_url, resolved, 255); redirect_url[255] = 0;
            }

            redirect_depth++;
            if (redirect_depth > 5) {
                error_type = ERR_REDIRECT;
                strcpy(error_detail, "Too many redirects");
                page_line_count = 0;
                strcpy(status_text, "Redirect Loop");
                is_loading = 0; load_progress = 0; redirect_depth = 0;
                kfree(response);
                return;
            }

            strncpy(url_buf, redirect_url, 255); url_buf[255] = 0;
            url_cursor = strlen(url_buf);
            strcpy(status_text, "Redirecting...");
            browser_load_page(redirect_url);
            kfree(response);
            return;
        }
    }

    redirect_depth = 0;

    char* body = strstr(response, "\r\n\r\n");
    if (body) body += 4;
    else { body = strstr(response, "\n\n"); if (body) body += 2; else body = response; }

    if (body == response && strncmp(body, "HTTP/", 5) == 0) {
        char* scan = body;
        while (*scan) {
            if ((*scan == '\r' && *(scan+1) == '\n' && *(scan+2) == '\r' && *(scan+3) == '\n')) { body = scan + 4; break; }
            if ((*scan == '\n' && *(scan+1) == '\n')) { body = scan + 2; break; }
            scan++;
        }
    }

    // Per HTTP spec, Transfer-Encoding (chunked) is about the transfer framing,
    // while Content-Encoding (gzip) is about the content itself. The correct
    // processing order is:
    //   1. Decode chunked transfer encoding (removes the chunk framing)
    //   2. Decompress gzip content encoding (decodes the actual content)
    //
    // BUG FIX: Previously gzip decompression ran BEFORE chunked decoding.
    // When a response was BOTH chunked AND gzipped (very common — most
    // real servers do both), the gzip check failed because body[0] was
    // the first hex digit of the chunk size, not the gzip magic 0x1F.
    // The chunked decoder then ran on the raw gzip stream, corrupting it
    // and producing binary garbage that the DOM parser couldn't handle.
    // Now we decode chunked first, then decompress gzip.
    int header_len = body - response;

    // Step 1: Check for chunked Transfer-Encoding and decode if needed.
    int is_chunked = 0;
    {
        // Scan headers for Transfer-Encoding: chunked
        char* h = response;
        while (h < body) {
            if ((h[0]=='T'||h[0]=='t') && (h[1]=='r'||h[1]=='R') && (h[2]=='a'||h[2]=='A') &&
                (h[3]=='n'||h[3]=='N') && (h[4]=='s'||h[4]=='S') && (h[5]=='f'||h[5]=='F') &&
                (h[6]=='e'||h[6]=='E') && (h[7]=='r'||h[7]=='R') && (h[8]=='-'||h[8]=='-') &&
                (h[9]=='E'||h[9]=='e') && (h[10]=='n'||h[10]=='N') && (h[11]=='c'||h[11]=='C') &&
                (h[12]=='o'||h[12]=='O') && (h[13]=='d'||h[13]=='D') && (h[14]=='i'||h[14]=='I') &&
                (h[15]=='n'||h[15]=='N') && (h[16]=='g'||h[16]=='G') && h[17]==':') {
                // Found Transfer-Encoding header — check value
                char* val = h + 18;
                while (*val == ' ') val++;
                if ((val[0]=='c'||val[0]=='C') && (val[1]=='h'||val[1]=='H') &&
                    (val[2]=='u'||val[2]=='U') && (val[3]=='n'||val[3]=='N') &&
                    (val[4]=='k'||val[4]=='K') && (val[5]=='e'||val[5]=='E') &&
                    (val[6]=='d'||val[6]=='D')) {
                    is_chunked = 1;
                }
                break;
            }
            // Advance to next header line
            while (h < body && *h != '\n') h++;
            if (h < body) h++;
        }
    }

    if (is_chunked) {
        int body_len = total_read - (body - response);
        int new_len = decode_chunked(body, body_len);
        total_read = (body - response) + new_len;
        s_printf("[Browser] Chunked encoding decoded: %d -> %d bytes\n", body_len, new_len);
    }

    // Step 2: Check for gzip Content-Encoding and decompress if needed.
    // This must run AFTER chunked decoding so the body starts with the
    // gzip magic bytes (0x1F 0x8B) rather than a chunk size header.
    int is_gzip = 0;
    {
        // Scan headers for Content-Encoding: gzip
        char* h = response;
        while (h < body) {
            if ((h[0]=='C'||h[0]=='c') && (h[1]=='o'||h[1]=='O') && (h[2]=='n'||h[2]=='N') &&
                (h[3]=='t'||h[3]=='T') && (h[4]=='e'||h[4]=='E') && (h[5]=='n'||h[5]=='N') &&
                (h[6]=='t'||h[6]=='T') && (h[7]=='-'||h[7]=='-') && (h[8]=='E'||h[8]=='e') &&
                (h[9]=='n'||h[9]=='N') && (h[10]=='c'||h[10]=='C') && (h[11]=='o'||h[11]=='O') &&
                (h[12]=='d'||h[12]=='D') && (h[13]=='i'||h[13]=='I') && (h[14]=='n'||h[14]=='N') &&
                (h[15]=='g'||h[15]=='G') && h[16]==':') {
                // Found Content-Encoding header — check value
                char* val = h + 17;
                while (*val == ' ') val++;
                if ((val[0]=='g'||val[0]=='G') && (val[1]=='z'||val[1]=='Z') &&
                    (val[2]=='i'||val[2]=='I') && (val[3]=='p'||val[3]=='P')) {
                    is_gzip = 1;
                }
                break;
            }
            // Advance to next header line
            while (h < body && *h != '\n') h++;
            if (h < body) h++;
        }
    }

    if (is_gzip && body[0] == 0x1F && body[1] == 0x8B) {
        // Decompress gzip body
        int body_len = total_read - (body - response);
        uint32_t decompressed_len = 0;
        // Allocate a new buffer for decompressed data
        char* decompressed = (char*)kmalloc(BROWSER_RESPONSE_SIZE);
        if (decompressed) {
            extern int gzip_inflate(const uint8_t* src, uint32_t src_len,
                                    uint8_t* dst, uint32_t dst_cap,
                                    uint32_t* dst_len);
            int result = gzip_inflate((const uint8_t*)body, body_len,
                                     (uint8_t*)decompressed, BROWSER_RESPONSE_SIZE - 1,
                                     &decompressed_len);
            if (result == 0 && decompressed_len > 0) {
                // Copy decompressed data back over the body area of the response buffer
                // We need the headers too for redirect handling, so copy decompressed
                // body to a fresh buffer and replace the response
                decompressed[decompressed_len] = 0;
                // Rebuild response: headers + decompressed body
                int new_total = header_len + decompressed_len;
                if (new_total < BROWSER_RESPONSE_SIZE) {
                    memcpy(body, decompressed, decompressed_len + 1);
                    total_read = new_total;
                } else {
                    // Decompressed body exceeds the buffer. Previously this path
                    // silently dropped the decompressed data, leaving the still-
                    // gzipped binary in `body` for the DOM parser to choke on.
                    // Now we copy as much as fits and log a warning so the user
                    // can see why a large page may have rendered partially.
                    int fit = BROWSER_RESPONSE_SIZE - header_len - 1;
                    if (fit < 0) fit = 0;
                    memcpy(body, decompressed, fit);
                    body[fit] = 0;
                    total_read = header_len + fit;
                    s_printf("[Browser] WARNING: decompressed body %u > buffer %d, truncated to %d\n",
                             decompressed_len, BROWSER_RESPONSE_SIZE - header_len - 1, fit);
                }
                s_printf("[Browser] Gzip decompressed: %d -> %d bytes\n", body_len, decompressed_len);
            } else {
                s_printf("[Browser] Gzip decompress FAILED (result=%d, decompressed_len=%u)\n",
                         result, decompressed_len);
            }
            kfree(decompressed);
            // Re-find body pointer (it hasn't moved since we copied in place)
        }
    } else if (is_gzip) {
        s_printf("[Browser] Gzip header detected but body doesn't start with magic bytes "
                 "(0x%02X 0x%02X, expected 0x1F 0x8B)\n",
                 (uint8_t)body[0], (uint8_t)body[1]);
    }

    // Save source HTML for view source (limit 32KB — was 8KB, too small for modern pages)
    {
        int src_len = total_read - (body - response);
        if (src_len > 32768) src_len = 32768;
        if (src_len > 0) {
            source_html = (char*)kmalloc(src_len + 1);
            if (source_html) { memcpy(source_html, body, src_len); source_html[src_len] = 0; source_len = src_len; }
        }
    }

    // Extract <title>
    char* title_start = strstr(body, "<title>");
    if (!title_start) title_start = strstr(body, "<TITLE>");
    if (!title_start) title_start = strstr(body, "<Title>");
    if (title_start) {
        title_start += 7;
        char* title_end = strstr(title_start, "</title>");
        if (!title_end) title_end = strstr(title_start, "</TITLE>");
        if (title_end) {
            int tlen = title_end - title_start;
            if (tlen > 63) tlen = 63;
            strncpy(page_title, title_start, tlen); page_title[tlen] = 0;
        }
    }

    if (browser_window && page_title[0]) {
        char wt[80]; strcpy(wt, page_title); strcat(wt, " - Browser");
        ws_set_title((window_t*)browser_window, wt);
    } else if (browser_window) {
        ws_set_title((window_t*)browser_window, "Browser");
    }

    // HTML to text conversion
    memset(line_types, 0, sizeof(line_types));
    int line = 0, col = 0;
    int in_tag = 0, in_script = 0, in_style = 0, in_head = 0, in_title_tag = 0;
    int in_link = 0, in_pre = 0, in_quote = 0, current_heading = 0;
    char link_url[256];
    int link_start_line = 0, link_start_col = 0;

    #define FLUSH_LINE() do { \
        if (col > 0 || in_pre) { \
            page_lines[line][col] = 0; \
            if (current_heading > 0) line_types[line] = current_heading; \
            else if (in_quote) line_types[line] = LINE_QUOTE; \
            line++; col = 0; \
        } \
    } while(0)

    for (int i = 0; body[i] && line < PAGE_LINES; i++) {
        char c = body[i];
        if (c == '<') {
            in_tag = 1;
            if (strncmp(body+i,"<script",7)==0) in_script=1;
            if (strncmp(body+i,"<style",6)==0) in_style=1;
            if (strncmp(body+i,"<head",5)==0) in_head=1;
            if (strncmp(body+i,"<title",6)==0) in_title_tag=1;
            if (strncmp(body+i,"</script>",9)==0) in_script=0;
            if (strncmp(body+i,"</style>",8)==0) in_style=0;
            if (strncmp(body+i,"</head>",7)==0) { in_head=0; continue; }
            if (strncmp(body+i,"</title>",8)==0) { in_title_tag=0; continue; }
            if (strncmp(body+i,"<pre",4)==0) { in_pre=1; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"</pre>",6)==0) { in_pre=0; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"<blockquote",11)==0) { in_quote=1; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"</blockquote>",12)==0) { in_quote=0; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"<a ",3)==0 || strncmp(body+i,"<A ",3)==0) {
                char* href = strstr(body+i,"href=\"");
                if (!href) href = strstr(body+i,"HREF=\"");
                if (href && href-(body+i) < 200) {
                    href+=6; int li=0;
                    while (href[li] && href[li]!='"' && li<255) { link_url[li]=href[li]; li++; }
                    link_url[li]=0; in_link=1; link_start_line=line; link_start_col=col;
                }
            }
            if (strncmp(body+i,"</a>",4)==0 || strncmp(body+i,"</A>",4)==0) {
                if (in_link && link_count < MAX_LINKS) {
                    links[link_count].line=link_start_line; links[link_count].col=link_start_col;
                    links[link_count].len=col-link_start_col;
                    strncpy(links[link_count].url,link_url,255); links[link_count].url[255]=0;
                    link_count++;
                }
                in_link=0;
            }
            if (strncmp(body+i,"<h1",3)==0) { current_heading=LINE_H1; FLUSH_LINE(); if(line<PAGE_LINES){page_lines[line][0]=0;line_types[line]=0;line++;} continue; }
            if (strncmp(body+i,"<h2",3)==0) { current_heading=LINE_H2; FLUSH_LINE(); if(line<PAGE_LINES){page_lines[line][0]=0;line_types[line]=0;line++;} continue; }
            if (strncmp(body+i,"<h3",3)==0) { current_heading=LINE_H3; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"<h4",3)==0) { current_heading=LINE_H4; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"<h5",3)==0) { current_heading=LINE_H5; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"<h6",3)==0) { current_heading=LINE_H6; FLUSH_LINE(); continue; }
            if (strncmp(body+i,"</h1",4)==0||strncmp(body+i,"</h2",4)==0||
                strncmp(body+i,"</h3",4)==0||strncmp(body+i,"</h4",4)==0||
                strncmp(body+i,"</h5",4)==0||strncmp(body+i,"</h6",4)==0) {
                current_heading=0; FLUSH_LINE();
                if(line<PAGE_LINES){page_lines[line][0]=0;line_types[line]=0;line++;}
                continue;
            }
            if (strncmp(body+i,"<hr",3)==0||strncmp(body+i,"<HR",3)==0) {
                FLUSH_LINE(); if(line<PAGE_LINES){line_types[line]=LINE_HR;page_lines[line][0]=0;line++;} continue;
            }
            if (strncmp(body+i,"<li",3)==0||strncmp(body+i,"<LI",3)==0) {
                FLUSH_LINE();
                if(line<PAGE_LINES){ page_lines[line][0]=' '; page_lines[line][1]='\x1e'; page_lines[line][2]=' '; col=3; line_types[line]=LINE_LI; }
                continue;
            }
            if (strncmp(body+i,"<br",3)==0||strncmp(body+i,"<BR",3)==0||
                strncmp(body+i,"<p",2)==0||strncmp(body+i,"<P",2)==0||
                strncmp(body+i,"<div",4)==0||strncmp(body+i,"<DIV",4)==0||
                strncmp(body+i,"<section",8)==0||strncmp(body+i,"<article",8)==0||
                strncmp(body+i,"<header",7)==0||strncmp(body+i,"<footer",7)==0||
                strncmp(body+i,"<nav",4)==0||strncmp(body+i,"<main",5)==0||
                strncmp(body+i,"<ul",3)==0||strncmp(body+i,"<ol",3)==0||
                strncmp(body+i,"<table",6)==0||
                strncmp(body+i,"<tr",3)==0||strncmp(body+i,"<TR",3)==0) {
                FLUSH_LINE();
                if (strncmp(body+i,"<p",2)==0||strncmp(body+i,"<P",2)==0) {
                    if(line<PAGE_LINES){page_lines[line][0]=0;line_types[line]=0;line++;}
                }
                continue;
            }
            if (strncmp(body+i,"</p",3)==0||strncmp(body+i,"</div",5)==0||
                strncmp(body+i,"</section",9)==0||strncmp(body+i,"</article",9)==0||
                strncmp(body+i,"</ul",4)==0||strncmp(body+i,"</ol",4)==0||
                strncmp(body+i,"</table",7)==0) {
                FLUSH_LINE();
                if (strncmp(body+i,"</p",3)==0) { if(line<PAGE_LINES){page_lines[line][0]=0;line_types[line]=0;line++;} }
                continue;
            }
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style || in_head || in_title_tag) continue;
        if (c == '&') {
            if (strncmp(body+i,"&amp;",5)==0) { c='&'; i+=4; }
            else if (strncmp(body+i,"&lt;",4)==0) { c='<'; i+=3; }
            else if (strncmp(body+i,"&gt;",4)==0) { c='>'; i+=3; }
            else if (strncmp(body+i,"&nbsp;",6)==0) { c=' '; i+=5; }
            else if (strncmp(body+i,"&quot;",6)==0) { c='"'; i+=5; }
            else if (strncmp(body+i,"&#39;",5)==0) { c='\''; i+=4; }
        }
        if (c == '\n' || c == '\r') {
            if (in_pre) { page_lines[line][col]=0; line_types[line]=LINE_PRE; line++; col=0; }
            else if (col > 0) {
                page_lines[line][col]=0;
                if (current_heading>0) line_types[line]=current_heading;
                else if (in_quote) line_types[line]=LINE_QUOTE;
                line++; col=0;
            }
            continue;
        }
        if (!in_pre && c==' ' && col>0 && page_lines[line][col-1]==' ') continue;
        if (col < PAGE_LINE_LEN-1) page_lines[line][col++] = c;
        else { page_lines[line][col]=0; line++; col=0; }
    }
    if (col > 0 && line < PAGE_LINES) { page_lines[line][col] = 0; line++; }
    page_line_count = line;
    #undef FLUSH_LINE

    // DOM Engine
    dom_doc = dom_document_create();
    if (dom_doc) {
        // Sync the JS bridge document singleton with the browser's current DOM
        // so JS bridge queries (getElementById, querySelector) operate on the
        // correct document instead of a stale singleton from a previous page
        dom_set_bridge_document(dom_doc);
        // Log what we're about to parse. The 'body' pointer points to the
        // start of the HTTP response body (after headers). Log the first
        // 200 bytes so we can verify the HTML is actually there.
        {
            int body_len = total_read - (body - response);
            s_printf("[Browser] DOM parse: body_len=%d, first 200 bytes:\n", body_len);
            int dump_len = body_len > 200 ? 200 : body_len;
            for (int i = 0; i < dump_len; i++) {
                char c = body[i];
                if (c < 32 && c != '\n' && c != '\r' && c != '\t') c = '.';
                s_printf("%c", c);
            }
            s_printf("\n--- end dump ---\n");
        }
        int dom_ok = dom_parse_html(dom_doc, body);
        if (dom_ok == 0) {
            // Wire up the external resource loader (browser_enhanced.c).
            // Previously this code was missing, so <link rel="stylesheet" href="...">
            // and <script src="..."> were silently dropped — only inline <style>
            // and inline <script> were parsed. This is why external CSS/JS were
            // "not rendered even when present in the response".
            //
            // Set the base URL first so relative hrefs / srcs can be resolved,
            // then fetch & apply external stylesheets, then collect external
            // (fetched + cached) and inline scripts into one concatenated buffer
            // that the mujs execution block below will run.
            browser_set_current_url_for_resources(url_buf);
            browser_process_link_tags(body);

            dom_apply_all_stylesheets(dom_doc);
            int content_h_est = browser_win_h - TAB_BAR_H - URL_BAR_H - STATUS_BAR_H;
            if (show_bookmarks) content_h_est -= BOOKMARK_BAR_H;
            dom_compute_styles(dom_doc, browser_win_w - PAD*2 - 12, content_h_est);

            // Collect ALL scripts (inline + external, in source order) into a
            // single buffer for execution. `dom_get_scripts` only returns the
            // inline scripts already extracted by the DOM parser; calling
            // `browser_process_script_tags(body, ...)` re-walks the raw HTML
            // and additionally fetches each <script src=...> via http_get,
            // appending its body to the buffer.
            //
            // 128KB is enough for ~30 typical bundled scripts. If a page
            // exceeds this, the truncation warning fires on serial.
            static char all_scripts[131072];
            browser_process_script_tags(body, all_scripts, sizeof(all_scripts));
            const char* scripts = (all_scripts[0]) ? all_scripts : dom_get_scripts(dom_doc);
            if (scripts && scripts[0]) {
                js_state = js_newstate(NULL, NULL, JS_STRICT);
                if (js_state) {
                    // Register browser APIs with mujs so scripts can use document, window, console
                    // -- console.log --
                    js_newobject(js_state);
                    js_newcfunction(js_state, js_browser_console_log, "log", 0);
                    js_setproperty(js_state, -2, "log");
                    js_setglobal(js_state, "console");
                    // -- document object --
                    js_newobject(js_state);
                    js_newcfunction(js_state, js_browser_doc_getElementById, "getElementById", 1);
                    js_setproperty(js_state, -2, "getElementById");
                    js_newcfunction(js_state, js_browser_doc_querySelector, "querySelector", 1);
                    js_setproperty(js_state, -2, "querySelector");
                    js_newcfunction(js_state, js_browser_doc_createElement, "createElement", 1);
                    js_setproperty(js_state, -2, "createElement");
                    js_newcfunction(js_state, js_browser_doc_write, "write", 1);
                    js_setproperty(js_state, -2, "write");
                    // document.body
                    js_newobject(js_state);
                    js_pushstring(js_state, "BODY");
                    js_setproperty(js_state, -2, "tagName");
                    js_setproperty(js_state, -2, "body");
                    // document.location
                    js_newobject(js_state);
                    js_pushstring(js_state, url_buf);
                    js_setproperty(js_state, -2, "href");
                    js_setproperty(js_state, -2, "location");
                    js_setglobal(js_state, "document");
                    // -- window object --
                    js_newobject(js_state);
                    js_getglobal(js_state, "document");
                    js_setproperty(js_state, -2, "document");
                    js_newobject(js_state);
                    js_pushstring(js_state, url_buf);
                    js_setproperty(js_state, -2, "href");
                    js_setproperty(js_state, -2, "location");
                    js_newcfunction(js_state, js_browser_window_setTimeout, "setTimeout", 2);
                    js_setproperty(js_state, -2, "setTimeout");
                    js_newcfunction(js_state, js_browser_window_setInterval, "setInterval", 2);
                    js_setproperty(js_state, -2, "setInterval");
                    js_setglobal(js_state, "window");
                    // -- other globals --
                    js_newcfunction(js_state, js_browser_alert, "alert", 1);
                    js_setglobal(js_state, "alert");
                    js_newcfunction(js_state, js_browser_parseInt, "parseInt", 1);
                    js_setglobal(js_state, "parseInt");

                    // Set execution limit: 500K instructions max, 4MB memory
                    // This prevents infinite loops in webpage JS from freezing the browser
                    js_setlimit(js_state, 500000, 4 * 1024 * 1024);
                    if (js_dostring(js_state, scripts)) s_printf("[Browser] JS execution error\n");
                    // Process any setTimeout/setInterval callbacks that are due
                    // Also set a lower limit for timer callbacks to prevent runaway scripts
                    js_setlimit(js_state, 50000, 4 * 1024 * 1024);
                    uint32_t timer_loop_start = get_tick_count();
                    int any_active;
                    do {
                        any_active = 0;
                        js_browser_process_timers(js_state);
                        for (int ti = 0; ti < MAX_TIMER_SLOTS; ti++) {
                            if (browser_timer_slots[ti].active) { any_active = 1; break; }
                        }
                        if (any_active) {
                            http_process_events();
                            // Safety: don't spin for more than 2 seconds (100 ticks at 50Hz) processing timers
                            if (get_tick_count() - timer_loop_start > 100) break;
                        }
                    } while (any_active);
                    // Clear all timer slots before freeing state
                    for (int ti = 0; ti < MAX_TIMER_SLOTS; ti++) {
                        if (browser_timer_slots[ti].active) {
                            js_delregistry(js_state, browser_timer_slots[ti].registry_key);
                            browser_timer_slots[ti].active = 0;
                        }
                    }
                    js_freestate(js_state); js_state = 0;
                }
            }
            use_dom_rendering = 1;
        } else {
            dom_document_destroy(dom_doc); dom_doc = 0; use_dom_rendering = 0;
            dom_set_bridge_document(NULL); // Clear stale bridge document
        }
    } else { use_dom_rendering = 0; dom_set_bridge_document(NULL); }

    char count_str[16];
    if (use_dom_rendering) strcpy(status_text, "Loaded (DOM)");
    else { strcpy(status_text, "Loaded ("); int_to_str(page_line_count, count_str); strcat(status_text, count_str); strcat(status_text, " lines)"); }
    if (http_status >= 400) { char st[64]; snprintf(st, 64, "HTTP %d - ", http_status); strcat(status_text, st); }
    if (use_tls) strcat(status_text, " [TLS]");
    is_loading = 0; load_progress = 100;

    // Track page load time
    page_load_time = get_tick_count() - page_load_start;
    page_size_bytes = total_read;

    // Track recently visited for NTP
    if (url_buf[0] && error_type == ERR_NONE) {
        // Shift existing entries down
        if (recent_visit_count >= 6) recent_visit_count = 5;
        for (int ri = recent_visit_count; ri > 0; ri--) {
            memcpy(&recent_visits[ri], &recent_visits[ri-1], sizeof(recent_visits[0]));
        }
        strncpy(recent_visits[0].title, page_title[0] ? page_title : url_buf, 47);
        recent_visits[0].title[47] = 0;
        strncpy(recent_visits[0].url, url_buf, 127);
        recent_visits[0].url[127] = 0;
        recent_visits[0].visit_time = get_tick_count();
        if (recent_visit_count < 6) recent_visit_count++;
    }

    kfree(response);
}

// ============================================================
// SECTION 6: Navigation
// ============================================================

static void browser_navigate(const char* url) {
    redirect_depth = 0;
    find_active = 0; find_match_count = 0; find_current_match = -1;

    // Detect non-URL queries and redirect to search engine
    // A URL should have a scheme (http://, https://) or contain a dot (domain.tld)
    // If the input looks like a search query, construct a DuckDuckGo search URL
    char nav_url[512];
    strncpy(nav_url, url, 511); nav_url[511] = 0;

    int is_url = 0;
    // Check for scheme prefix
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0 ||
        strncmp(url, "file://", 7) == 0 || strncmp(url, "ftp://", 6) == 0) {
        is_url = 1;
    }
    // Check for common domain patterns (contains a dot and no spaces)
    if (!is_url) {
        int has_dot = 0, has_space = 0;
        for (int i = 0; url[i]; i++) {
            if (url[i] == '.') has_dot = 1;
            if (url[i] == ' ' || url[i] == '\t') has_space = 1;
        }
        if (has_dot && !has_space) is_url = 1;
        // Also treat "localhost" as a URL
        if (strncmp(url, "localhost", 9) == 0) is_url = 1;
    }

    if (!is_url) {
        // Build DuckDuckGo search URL with URL-encoded query
        // Reserve space for "http://duckduckgo.com/?q=" (28 chars) in the 512-byte nav_url
        // So encoded query can be at most ~480 bytes, but we use 420 for safety margin
        char encoded[424];
        int ei = 0;
        for (int i = 0; url[i] && ei < 418; i++) {
            if (ei + 3 >= 418) break; // prevent overflow on 3-char encodings
            if (url[i] == ' ') {
                encoded[ei++] = '%'; encoded[ei++] = '2'; encoded[ei++] = '0';
            } else if (url[i] == '&') {
                encoded[ei++] = '%'; encoded[ei++] = '2'; encoded[ei++] = '6';
            } else if (url[i] == '#') {
                encoded[ei++] = '%'; encoded[ei++] = '2'; encoded[ei++] = '3';
            } else if (url[i] == '%') {
                encoded[ei++] = '%'; encoded[ei++] = '2'; encoded[ei++] = '5';
            } else if (url[i] == '+') {
                encoded[ei++] = '%'; encoded[ei++] = '2'; encoded[ei++] = 'B';
            } else if (url[i] == '?') {
                encoded[ei++] = '%'; encoded[ei++] = '3'; encoded[ei++] = 'F';
            } else {
                encoded[ei++] = url[i];
            }
        }
        encoded[ei] = 0;
        snprintf(nav_url, sizeof(nav_url), "http://duckduckgo.com/?q=%s", encoded);
    }

    if (history_pos < HISTORY_MAX - 1) {
        history_pos++;
        strncpy(history[history_pos], nav_url, 255); history[history_pos][255] = 0;
        history_count = history_pos + 1;
    }
    strncpy(url_buf, nav_url, 255); url_buf[255] = 0;
    url_cursor = strlen(url_buf);
    browser_load_page(nav_url);
}

static void browser_go_back(void) {
    if (history_pos > 0) {
        history_pos--;
        strncpy(url_buf, history[history_pos], 255); url_buf[255] = 0;
        url_cursor = strlen(url_buf);
        browser_load_page(url_buf);
    }
}

static void browser_go_forward(void) {
    if (history_pos < history_count - 1) {
        history_pos++;
        strncpy(url_buf, history[history_pos], 255); url_buf[255] = 0;
        url_cursor = strlen(url_buf);
        browser_load_page(url_buf);
    }
}

// ============================================================
// SECTION 7: Download
// ============================================================

static void browser_download_file(const char* url) {
    int use_tls = 0; char host[128] = ""; char path[128] = "/"; int port = 80;
    const char* url_start = url;
    if (strncmp(url,"https://",8)==0) { use_tls=1; url_start=url+8; port=443; }
    else if (strncmp(url,"http://",7)==0) url_start=url+7;
    int hi=0;
    while (*url_start && *url_start!='/' && hi<127) host[hi++]=*url_start++;
    host[hi]=0;
    char* colon = strchr(host,':');
    if (colon) { *colon=0; port=0; colon++;
        while (*colon>='0'&&*colon<='9') { port=port*10+(*colon-'0'); colon++; }
        if (port==0) port=use_tls?443:80;
    }
    if (*url_start=='/') { strncpy(path,url_start,127); path[127]=0; }
    const char* last_slash = strrchr(path,'/');
    const char* fname = last_slash ? last_slash+1 : path;
    if (!fname[0]) fname = "download.dat";
    strncpy(download_filename, fname, 63); download_filename[63]=0;
    int flen = strlen(download_filename);
    download_is_app = (flen>4 && (strcmp(download_filename+flen-4,".app")==0 ||
        strcmp(download_filename+flen-4,".cdl")==0 || strcmp(download_filename+flen-4,".dmg")==0));

    char ip_str[16];
    extern int dns_resolve(const char* name, char* ip_buf, int ip_buf_len);
    if (dns_resolve(host,ip_str,sizeof(ip_str))!=0) { strcpy(status_text,"Download: DNS Error"); return; }

    extern uint32_t ip_parse(const char* str);
    uint32_t ip = ip_parse(ip_str);
    int sockfd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd<0) { strcpy(status_text,"Download: Socket Error"); return; }
    sockaddr_in_t server_addr; memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family=AF_INET; server_addr.sin_port=htons(port); server_addr.sin_addr=ip;
    strcpy(status_text,"Download: Connecting...");
    if (k_connect(sockfd,&server_addr)<0) { k_close(sockfd); strcpy(status_text,"Download: Connect Error"); return; }

    tls_session_t* tls_session = 0;
    if (use_tls) {
        strcpy(status_text,"Download: Establishing TLS...");
        extern tls_session_t* tls_client_handshake_fd(int sockfd, const char* hostname, uint16_t port);
        tls_session = tls_client_handshake_fd(sockfd, host, port);
        if (!tls_session) { k_close(sockfd); strcpy(status_text,"Download: TLS Error"); return; }
    }
    char request[768];
    int rlen = sprintf(request,"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (use_tls && tls_session) tls_write(tls_session, request, rlen);
    else k_sendto(sockfd, request, rlen, 0, NULL);
    download_active=1; download_progress=10; strcpy(status_text,"Downloading...");

    char* response = (char*)kmalloc(BROWSER_RESPONSE_SIZE);
    if (!response) { strcpy(status_text,"Download: OOM"); download_active=0;
        if (tls_session){extern void tls_client_session_close(tls_session_t*);tls_client_session_close(tls_session);}
        k_close(sockfd); return; }
    int total_read=0;
    for (int retry=0; retry<300 && total_read<BROWSER_RESPONSE_SIZE-1; retry++) {
        extern void rtl8139_poll(); rtl8139_poll();
        int n;
        if (use_tls&&tls_session) n=tls_read(tls_session,response+total_read,BROWSER_RESPONSE_SIZE-total_read-1);
        else n=k_recvfrom(sockfd,response+total_read,BROWSER_RESPONSE_SIZE-total_read-1,0,NULL);
        if (n>0) { total_read+=n; download_progress=10+(total_read*80)/BROWSER_RESPONSE_SIZE; }
        else if (n==0) break;
        else { http_process_events(); } // yield CPU instead of busy-wait
        http_process_events();
    }
    response[total_read]=0;
    if (tls_session){extern void tls_client_session_close(tls_session_t*);tls_client_session_close(tls_session);}
    k_close(sockfd);
    char* body=strstr(response,"\r\n\r\n"); int body_len=0;
    if (body){body+=4;body_len=total_read-(body-response);} else {body=response;body_len=total_read;}
    char save_path[128];
    if (download_is_app) { strcpy(save_path,"/tmp/"); strcat(save_path,download_filename); }
    else { extern char g_desktop_path[128]; strcpy(save_path,g_desktop_path); strcat(save_path,"/"); strcat(save_path,download_filename); }
    int res = sys_fs_write(save_path, body, body_len);
    download_progress=100;
    if (res>=0) { strcpy(status_text,"Downloaded: "); strcat(status_text,download_filename);
        if (download_is_app){extern void desktop_install_app(const char*);desktop_install_app(save_path);strcat(status_text," (installed)");}
    } else strcpy(status_text,"Download: Save Error");
    kfree(response); download_active=0;
}

// ============================================================
// SECTION 8: Find on Page
// ============================================================

// Debounce counter for find-on-page: only search after the user stops
// typing for a few frames, preventing UI lag on every keystroke.
static int find_debounce = 0;
#define FIND_DEBOUNCE_FRAMES 5

static void find_update_matches(void) {
    find_match_count = 0;
    find_current_match = -1;
    if (!find_query[0]) return;
    int qlen = strlen(find_query);
    // Skip expensive scan for single-character queries (too many false matches)
    if (qlen < 2) return;
    for (int i = 0; i < page_line_count && find_match_count < MAX_FIND_MATCHES; i++) {
        int llen = strlen(page_lines[i]);
        for (int j = 0; j <= llen - qlen; j++) {
            if (strncmp(page_lines[i] + j, find_query, qlen) == 0) {
                find_matches[find_match_count].line = i;
                find_matches[find_match_count].col = j;
                find_matches[find_match_count].len = qlen;
                find_match_count++;
                j += qlen - 1;
                if (find_match_count >= MAX_FIND_MATCHES) break;
            }
        }
    }
    if (find_match_count > 0) find_current_match = 0;
}

static void find_next(void) {
    if (find_match_count == 0) return;
    find_current_match++;
    if (find_current_match >= find_match_count) find_current_match = 0;
    if (find_current_match >= 0 && find_current_match < find_match_count) {
        int line = find_matches[find_current_match].line;
        int lh = zoom_line_h[zoom_level];
        int vis = (browser_win_h - TAB_BAR_H - URL_BAR_H - STATUS_BAR_H -
                   (show_bookmarks ? BOOKMARK_BAR_H : 0) - (find_active ? FIND_BAR_H : 0) -
                   (dev_tools_active ? DEV_TOOLS_H : 0)) / lh;
        if (vis < 1) vis = 1;
        if (line < scroll_offset || line >= scroll_offset + vis) {
            scroll_offset = line - 2;
            if (scroll_offset < 0) scroll_offset = 0;
        }
    }
}

static void find_prev(void) {
    if (find_match_count == 0) return;
    find_current_match--;
    if (find_current_match < 0) find_current_match = find_match_count - 1;
    if (find_current_match >= 0 && find_current_match < find_match_count) {
        int line = find_matches[find_current_match].line;
        int lh = zoom_line_h[zoom_level];
        int vis = (browser_win_h - TAB_BAR_H - URL_BAR_H - STATUS_BAR_H -
                   (show_bookmarks ? BOOKMARK_BAR_H : 0) - (find_active ? FIND_BAR_H : 0) -
                   (dev_tools_active ? DEV_TOOLS_H : 0)) / lh;
        if (vis < 1) vis = 1;
        if (line < scroll_offset || line >= scroll_offset + vis) {
            scroll_offset = line - 2;
            if (scroll_offset < 0) scroll_offset = 0;
        }
    }
}

// ============================================================
// SECTION 9: Helper Drawing Functions
// ============================================================

static void draw_arrow_left(int cx, int cy, uint32_t color) {
    gfx_draw_line(cx + 4, cy - 5, cx - 3, cy, color);
    gfx_draw_line(cx + 4, cy + 5, cx - 3, cy, color);
}

static void draw_arrow_right(int cx, int cy, uint32_t color) {
    gfx_draw_line(cx - 4, cy - 5, cx + 3, cy, color);
    gfx_draw_line(cx - 4, cy + 5, cx + 3, cy, color);
}

static void draw_refresh_icon(int cx, int cy, uint32_t color) {
    // Circular arrow: partial circle with arrowhead
    gfx_draw_line(cx - 4, cy - 4, cx + 4, cy - 4, color);
    gfx_draw_line(cx - 4, cy - 4, cx - 4, cy + 2, color);
    gfx_draw_line(cx - 4, cy + 2, cx + 4, cy + 2, color);
    gfx_draw_line(cx + 4, cy - 4, cx + 4, cy, color);
    gfx_draw_line(cx + 4, cy - 4, cx + 2, cy - 6, color);
    gfx_draw_line(cx + 4, cy - 4, cx + 6, cy - 2, color);
}

// Draw a home icon (small house)
static void draw_home_icon(int cx, int cy, uint32_t color) {
    // Roof triangle
    gfx_draw_line(cx - 6, cy, cx, cy - 6, color);
    gfx_draw_line(cx + 6, cy, cx, cy - 6, color);
    // House body
    gfx_draw_line(cx - 5, cy, cx - 5, cy + 5, color);
    gfx_draw_line(cx + 5, cy, cx + 5, cy + 5, color);
    gfx_draw_line(cx - 5, cy + 5, cx + 5, cy + 5, color);
    // Door
    gfx_draw_line(cx - 2, cy + 5, cx - 2, cy + 2, color);
    gfx_draw_line(cx + 2, cy + 5, cx + 2, cy + 2, color);
    gfx_draw_line(cx - 2, cy + 2, cx + 2, cy + 2, color);
}

// Draw a book/reading icon
static void draw_reading_icon(int cx, int cy, uint32_t color) {
    // Book shape
    gfx_draw_line(cx - 5, cy - 5, cx - 5, cy + 5, color);
    gfx_draw_line(cx + 5, cy - 5, cx + 5, cy + 5, color);
    gfx_draw_line(cx - 5, cy - 5, cx, cy - 3, color);
    gfx_draw_line(cx + 5, cy - 5, cx, cy - 3, color);
    gfx_draw_line(cx - 5, cy + 5, cx, cy + 3, color);
    gfx_draw_line(cx + 5, cy + 5, cx, cy + 3, color);
    // Lines on pages
    gfx_draw_line(cx - 3, cy - 2, cx - 1, cy - 1, color);
    gfx_draw_line(cx - 3, cy + 1, cx - 1, cy + 2, color);
    gfx_draw_line(cx + 1, cy - 1, cx + 3, cy - 2, color);
    gfx_draw_line(cx + 1, cy + 2, cx + 3, cy + 1, color);
}

// Draw a download arrow icon
static void draw_download_icon(int cx, int cy, uint32_t color) {
    gfx_draw_line(cx, cy - 5, cx, cy + 3, color);
    gfx_draw_line(cx - 4, cy, cx, cy + 4, color);
    gfx_draw_line(cx + 4, cy, cx, cy + 4, color);
    gfx_draw_line(cx - 4, cy + 5, cx + 4, cy + 5, color);
}

// Draw a shield/lock icon for HTTPS
static void draw_shield_icon(int cx, int cy, uint32_t color) {
    gfx_draw_line(cx - 4, cy - 4, cx, cy - 6, color);
    gfx_draw_line(cx + 4, cy - 4, cx, cy - 6, color);
    gfx_draw_line(cx - 4, cy - 4, cx - 4, cy + 2, color);
    gfx_draw_line(cx + 4, cy - 4, cx + 4, cy + 2, color);
    gfx_draw_line(cx - 4, cy + 2, cx, cy + 5, color);
    gfx_draw_line(cx + 4, cy + 2, cx, cy + 5, color);
    // Checkmark inside
    gfx_draw_line(cx - 2, cy, cx, cy + 2, color);
    gfx_draw_line(cx, cy + 2, cx + 3, cy - 3, color);
}

// Draw a magnifying glass icon
static void draw_search_icon(int cx, int cy, int sz, uint32_t color) {
    int r = sz / 2;
    // Circle
    for (int a = 0; a < 360; a += 10) {
        int a1 = a * 1000 / 360;
        int a2 = (a + 10) * 1000 / 360;
        // Use integer sin/cos approximation
        int x1 = cx + (r * (1000 - 4 * a1 * (1000 - a1) / 1000) / 1000);
        int y1 = cy + (r * (1000 - 4 * a1 * (1000 - a1) / 1000) / 1000);
    }
    // Simplified: just draw a small circle with lines
    gfx_draw_line(cx - r, cy - r + 1, cx + r, cy - r + 1, color);
    gfx_draw_line(cx - r, cy + r - 1, cx + r, cy + r - 1, color);
    gfx_draw_line(cx - r + 1, cy - r, cx - r + 1, cy + r, color);
    gfx_draw_line(cx + r - 1, cy - r, cx + r - 1, cy + r, color);
    // Handle
    gfx_draw_line(cx + r - 1, cy + r - 1, cx + r + 3, cy + r + 3, color);
    gfx_draw_line(cx + r, cy + r, cx + r + 3, cy + r + 3, color);
}

// ============================================================
// SECTION 10: New Tab Page Rendering
// ============================================================

static void render_new_tab_page(int x, int y, int w, int h) {
    // Gradient background (top to bottom, light to slightly darker)
    int r1 = (COL_NTP_BG_TOP >> 16) & 0xFF, g1 = (COL_NTP_BG_TOP >> 8) & 0xFF, b1 = COL_NTP_BG_TOP & 0xFF;
    int r2 = (COL_NTP_BG_BOTTOM >> 16) & 0xFF, g2 = (COL_NTP_BG_BOTTOM >> 8) & 0xFF, b2 = COL_NTP_BG_BOTTOM & 0xFF;
    for (int row = 0; row < h; row++) {
        int r = (r1 * (h - row) + r2 * row) / h;
        int g = (g1 * (h - row) + g2 * row) / h;
        int b = (b1 * (h - row) + b2 * row) / h;
        gfx_fill_rect(x, y + row, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }

    int cx = x + w / 2;
    int base_y = y + 20;

    // Greeting based on time of day (using tick count as rough hour)
    uint32_t ticks = get_tick_count();
    int rough_hour = (ticks / 3600000) % 24;  // Very rough approximation
    const char* greeting;
    if (rough_hour < 12) greeting = "Good Morning";
    else if (rough_hour < 18) greeting = "Good Afternoon";
    else greeting = "Good Evening";

    // Greeting text
    gfx_draw_string_centered(cx, base_y, greeting, COL_TEXT_MUTED, 1);
    base_y += 22;

    // Title with larger font
    gfx_draw_string_centered(cx, base_y, "CamelOS Browser", COL_TEXT_DARK, 2);
    base_y += 30;

    // Subtitle
    gfx_draw_string_centered(cx, base_y, "Explore the web", COL_TEXT_MUTED, 1);
    base_y += 28;

    // Search bar (wider, with shadow, rounded)
    int search_w = 440;
    if (search_w > w - 40) search_w = w - 40;
    int search_x = cx - search_w / 2;
    int search_y = base_y;
    // Shadow
    gfx_fill_rounded_rect(search_x + 1, search_y + 2, search_w, 34, 0x20000000, 8);
    // Background
    gfx_fill_rounded_rect(search_x, search_y, search_w, 34, 0xFFFFFFFF, 8);
    gfx_draw_rect(search_x, search_y, search_w, 34, COL_URL_BAR_BORDER);
    // Magnifying glass icon
    gfx_draw_string(search_x + 10, search_y + 10, "Q", COL_TEXT_MUTED);  // Q as magnifying glass substitute
    gfx_draw_string(search_x + 28, search_y + 10, "Search or enter URL", COL_TEXT_MUTED);

    base_y = search_y + 50;

    // Favorites label
    gfx_draw_string_centered(cx, base_y, "Favorites", COL_TEXT_MUTED, 1);
    base_y += 20;

    // Shortcut tiles (2 rows x 4 cols) with improved styling
    int tile_w = 90, tile_h = 76, tile_gap = 14;
    int grid_w = 4 * tile_w + 3 * tile_gap;
    int grid_x = cx - grid_w / 2;
    int grid_y = base_y;

    for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4;
        int tx = grid_x + col * (tile_w + tile_gap);
        int ty = grid_y + row * (tile_h + tile_gap);
        if (ty + tile_h > y + h - 10) break;

        // Tile shadow
        gfx_fill_rounded_rect(tx + 1, ty + 2, tile_w, tile_h, 0x15000000, 8);
        // Tile background with white
        gfx_fill_rounded_rect(tx, ty, tile_w, tile_h, 0xFFFFFFFF, 8);
        gfx_draw_rect(tx, ty, tile_w, tile_h, 0xFFE8E8E8);

        // Icon (colored rounded rect with first letter, slightly larger)
        int icon_sz = 36;
        int icon_x = tx + (tile_w - icon_sz) / 2;
        int icon_y = ty + 8;
        gfx_fill_rounded_rect(icon_x, icon_y, icon_sz, icon_sz, shortcuts[i].color, 8);
        char letter[2] = { shortcuts[i].name[0], 0 };
        uint32_t letter_col = 0xFFFFFFFF;
        // Use dark text for light icon backgrounds
        if ((shortcuts[i].color & 0xFF) > 0xCC &&
            ((shortcuts[i].color >> 8) & 0xFF) > 0xCC &&
            ((shortcuts[i].color >> 16) & 0xFF) > 0xCC) {
            letter_col = 0xFF333333;
        }
        gfx_draw_string_centered(icon_x + icon_sz / 2, icon_y + 11, letter, letter_col, 1);

        // Name (centered below icon)
        gfx_draw_string_centered(tx + tile_w / 2, icon_y + icon_sz + 5, shortcuts[i].name, COL_TEXT_MUTED, 1);
    }

    // Recently visited section (if any)
    if (recent_visit_count > 0) {
        int rv_y = grid_y + 2 * (tile_h + tile_gap) + 16;
        if (rv_y + 80 < y + h) {
            gfx_draw_string_centered(cx, rv_y, "Recently Visited", COL_TEXT_MUTED, 1);
            rv_y += 18;
            gfx_fill_rect(x + 30, rv_y, w - 60, 1, COL_SEPARATOR);
            rv_y += 8;
            for (int i = 0; i < recent_visit_count && rv_y + 18 < y + h - 10; i++) {
                // Small colored dot
                gfx_fill_rounded_rect(x + 40, rv_y + 3, 8, 8, COL_ACCENT_BLUE, 4);
                // Calculate URL position first so we can use it for title clipping
                int url_x = x + w - 200;
                // Title (clipped to available width)
                {
                    const char* rv_title = recent_visits[i].title[0] ? recent_visits[i].title : recent_visits[i].url;
                    int max_title_w = (url_x > x + 54 + 100) ? (url_x - x - 60) : (w - 68);
                    gfx_draw_string_clipped(x + 54, rv_y, rv_title, COL_TEXT_DARK, max_title_w);
                }
                // URL on the right (truncated)
                if (url_x > x + 54 + 100) {
                    char trunc_url[24];
                    int ulen = strlen(recent_visits[i].url);
                    if (ulen > 22) { strncpy(trunc_url, recent_visits[i].url, 20); trunc_url[20] = '.'; trunc_url[21] = '.'; trunc_url[22] = 0; }
                    else strcpy(trunc_url, recent_visits[i].url);
                    gfx_draw_string(url_x, rv_y, trunc_url, COL_TEXT_MUTED);
                }
                rv_y += 18;
            }
        }
    }
}

// ============================================================
// SECTION 11: Error Page Rendering
// ============================================================

static void render_error_page(int x, int y, int w, int h) {
    // Subtle gradient background
    for (int row = 0; row < h; row++) {
        gfx_fill_rect(x, y + row, w, 1, 0xFFF8F8FA);
    }
    int cx = x + w / 2;
    int cy = y + h / 4;

    // Error icon (larger, with shadow)
    int icon_sz = 64;
    gfx_fill_rounded_rect(cx - icon_sz/2 + 2, cy - icon_sz/2 + 2, icon_sz, icon_sz, 0x30000000, icon_sz/2);
    gfx_fill_rounded_rect(cx - icon_sz/2, cy - icon_sz/2, icon_sz, icon_sz, COL_ERROR_RED, icon_sz/2);
    gfx_draw_string_centered(cx, cy - 8, "!", 0xFFFFFFFF, 3);

    cy += icon_sz/2 + 24;

    const char* title = "Error";
    const char* desc = "An error occurred while loading the page.";
    const char* suggestion = "Check the URL and try again.";
    int err_code = 0;

    switch (error_type) {
        case ERR_DNS:
            title = "Server Not Found";
            desc = "Could not resolve the hostname.";
            suggestion = "Check the address for typing errors.";
            err_code = 1;
            break;
        case ERR_CONNECT:
            title = "Can't Connect";
            desc = "Could not connect to the server.";
            suggestion = "The server may be down or unreachable.";
            err_code = 2;
            break;
        case ERR_TLS:
            title = "Secure Connection Failed";
            desc = "Could not establish a secure connection.";
            suggestion = "Try again or use HTTP instead.";
            err_code = 3;
            break;
        case ERR_SOCKET:
            title = "Network Error";
            desc = "Could not create a network socket.";
            suggestion = "Check your network connection.";
            err_code = 4;
            break;
        case ERR_MEMORY:
            title = "Out of Memory";
            desc = "Not enough memory to load the page.";
            suggestion = "Try closing other tabs.";
            err_code = 5;
            break;
        case ERR_SEND:
            title = "Request Failed";
            desc = "Could not send the request to the server.";
            suggestion = "Check your connection and try again.";
            err_code = 6;
            break;
        case ERR_REDIRECT:
            title = "Too Many Redirects";
            desc = "The server is redirecting in a loop.";
            suggestion = "The page may be misconfigured.";
            err_code = 7;
            break;
    }

    // Title (larger)
    gfx_draw_string_centered(cx, cy, title, COL_TEXT_DARK, 1);
    cy += 26;

    // Error detail in a subtle box
    if (error_detail[0]) {
        int detail_w = strlen(error_detail) * 8 + 24;
        if (detail_w > w - 40) detail_w = w - 40;
        gfx_fill_rounded_rect(cx - detail_w/2, cy, detail_w, 22, 0xFFFFE8E8, 4);
        gfx_draw_rect(cx - detail_w/2, cy, detail_w, 22, 0xFFE8C0C0);
        gfx_draw_string_centered(cx, cy + 4, error_detail, COL_ERROR_RED, 1);
        cy += 28;
    }

    // Description
    gfx_draw_string_centered(cx, cy, desc, COL_TEXT_MUTED, 1);
    cy += 24;

    // Suggestion
    gfx_draw_string_centered(cx, cy, suggestion, COL_ACCENT_BLUE, 1);
    cy += 30;

    // Retry button
    int retry_w = 120, retry_h = 34;
    gfx_fill_rounded_rect(cx - retry_w/2 + 1, cy + 2, retry_w, retry_h, 0x20000000, 6);
    gfx_fill_rounded_rect(cx - retry_w/2, cy, retry_w, retry_h, COL_ACCENT_BLUE, 6);
    gfx_draw_string_centered(cx, cy + 10, "Try Again", 0xFFFFFFFF, 1);

    // Error code at bottom
    cy = y + h - 30;
    char ec_str[32];
    snprintf(ec_str, 32, "Error Code: ERR_%d", err_code);
    gfx_draw_string_centered(cx, cy, ec_str, 0xFFBBBBBB, 1);
}

// ============================================================
// SECTION 12: View Source Rendering
// ============================================================

static void render_source_view(int x, int y, int w, int h) {
    if (!source_html || !source_len) {
        gfx_draw_string(x + PAD, y + 8, "No source available", COL_TEXT_MUTED);
        return;
    }
    int lh = 14;
    int max_chars = (w - PAD * 2) / 8;
    if (max_chars < 10) max_chars = 10;
    int vis_lines = h / lh;
    int src_lines = source_len / max_chars + 1;

    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > src_lines - vis_lines && src_lines > vis_lines) scroll_offset = src_lines - vis_lines;

    int pos = 0;
    int cur_line = 0;
    int in_tag = 0;
    int draw_y = y;

    // Skip to scroll position
    int skip_chars = scroll_offset * max_chars;
    pos = skip_chars;

    for (int vl = 0; vl < vis_lines && pos < source_len; vl++) {
        int line_x = x + PAD;
        int chars_drawn = 0;

        while (chars_drawn < max_chars && pos < source_len) {
            char c = source_html[pos++];
            if (c == '\n' || c == '\r') break;

            uint32_t color = COL_TEXT_DARK;
            if (c == '<') in_tag = 1;
            if (in_tag) color = 0xFF007AFF;  // Blue for tags
            if (c == '>') { color = 0xFF007AFF; in_tag = 0; }

            if (line_x + 8 <= x + w - PAD) {
                char s[2] = { c, 0 };
                gfx_draw_string(line_x, draw_y, s, color);
            }
            line_x += 8;
            chars_drawn++;
        }
        draw_y += lh;
    }
}

// ============================================================
// SECTION 13: Context Menu
// ============================================================

static void ctx_menu_show(int mx, int my) {
    ctx_menu_active = 1;
    ctx_menu_x = mx; ctx_menu_y = my;
    ctx_menu_hovered = -1;
    ctx_menu_count = 0;

    int ci = 0;
    strcpy(ctx_menu_items[ci].label, "Back");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_BACK; ci++;
    strcpy(ctx_menu_items[ci].label, "Forward");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_FORWARD; ci++;
    strcpy(ctx_menu_items[ci].label, "Reload");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_RELOAD; ci++;
    strcpy(ctx_menu_items[ci].label, "");
    ctx_menu_items[ci].is_separator = 1; ctx_menu_items[ci].action_id = 0; ci++;
    strcpy(ctx_menu_items[ci].label, "Bookmark This Page");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_BOOKMARK; ci++;
    strcpy(ctx_menu_items[ci].label, "Copy URL");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_COPY_URL; ci++;
    strcpy(ctx_menu_items[ci].label, "");
    ctx_menu_items[ci].is_separator = 1; ctx_menu_items[ci].action_id = 0; ci++;
    strcpy(ctx_menu_items[ci].label, "View Source");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_VIEW_SOURCE; ci++;
    strcpy(ctx_menu_items[ci].label, "Inspect Element");
    ctx_menu_items[ci].is_separator = 0; ctx_menu_items[ci].action_id = CTX_ACTION_INSPECT; ci++;

    ctx_menu_count = ci;
}

static void ctx_menu_hide(void) {
    ctx_menu_active = 0;
}

static void ctx_menu_exec(int action_id) {
    ctx_menu_hide();
    switch (action_id) {
        case CTX_ACTION_BACK: browser_go_back(); break;
        case CTX_ACTION_FORWARD: browser_go_forward(); break;
        case CTX_ACTION_RELOAD: if (history_pos >= 0) browser_load_page(url_buf); break;
        case CTX_ACTION_BOOKMARK:
            if (bookmark_count < BOOKMARK_MAX && url_buf[0]) {
                strncpy(bookmarks[bookmark_count].name, page_title[0] ? page_title : url_buf, 31);
                bookmarks[bookmark_count].name[31] = 0;
                strncpy(bookmarks[bookmark_count].url, url_buf, 255);
                bookmarks[bookmark_count].url[255] = 0;
                bookmark_count++;
            }
            break;
        case CTX_ACTION_COPY_URL:
            // Copy URL to clipboard (if clipboard API exists)
            break;
        case CTX_ACTION_VIEW_SOURCE:
            view_source = !view_source;
            tab_save_state();
            break;
        case CTX_ACTION_INSPECT:
            dev_tools_active = !dev_tools_active;
            tab_save_state();
            break;
    }
}

static void render_context_menu(int ox, int oy) {
    if (!ctx_menu_active) return;
    int menu_w = 180;
    int item_h = 22;
    int menu_h = 0;
    for (int i = 0; i < ctx_menu_count; i++)
        menu_h += ctx_menu_items[i].is_separator ? 6 : item_h;

    int mx = ctx_menu_x + ox, my = ctx_menu_y + oy;

    // Shadow
    gfx_fill_rounded_rect(mx + 3, my + 3, menu_w, menu_h, 0xFFD0D0D0, 6);
    // Background
    gfx_fill_rounded_rect(mx, my, menu_w, menu_h, 0xFFFFFFFF, 6);
    gfx_stroke_rounded_rect(mx, my, menu_w, menu_h, 0xFFC0C0C0, 6, 1);

    int iy = my;
    for (int i = 0; i < ctx_menu_count; i++) {
        if (ctx_menu_items[i].is_separator) {
            gfx_fill_rect(mx + 8, iy + 2, menu_w - 16, 1, 0xFFE0E0E0);
            iy += 6;
        } else {
            if (i == ctx_menu_hovered) {
                gfx_fill_rounded_rect(mx + 2, iy, menu_w - 4, item_h, COL_ACCENT_BLUE, 3);
                gfx_draw_string(mx + 12, iy + 5, ctx_menu_items[i].label, 0xFFFFFFFF);
            } else {
                gfx_draw_string(mx + 12, iy + 5, ctx_menu_items[i].label, COL_TEXT_DARK);
            }
            iy += item_h;
        }
    }
}

// ============================================================
// SECTION 14: Developer Tools Panel
// ============================================================

static void render_dev_tools(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, 0xFF2D2D2D);
    gfx_draw_rect(x, y, w, 1, 0xFF555555);

    int tab_w = 70;
    const char* tab_names[] = { "Console", "DOM", "Stats" };
    for (int i = 0; i < 3; i++) {
        int tx = x + i * tab_w;
        uint32_t bg = (i == dev_tools_tab) ? 0xFF3D3D3D : 0xFF2D2D2D;
        gfx_fill_rect(tx, y, tab_w, 24, bg);
        gfx_draw_string(tx + 8, y + 6, tab_names[i], (i == dev_tools_tab) ? 0xFFFFFFFF : 0xFF999999);
    }

    int cy = y + 28;
    if (dev_tools_tab == 0) {
        for (int i = 0; i < dev_console_count && cy < y + h - 16; i++) {
            gfx_draw_string(x + 8, cy, dev_console[i], 0xFF00FF00);
            cy += 14;
        }
        if (dev_console_count == 0) gfx_draw_string(x + 8, cy, "No console output", 0xFF666666);
    } else if (dev_tools_tab == 1) {
        gfx_draw_string(x + 8, cy, "DOM Tree:", 0xFFCCCCCC); cy += 16;
        if (dom_doc) {
            gfx_draw_string(x + 16, cy, "Document loaded", 0xFF88FF88); cy += 14;
            if (page_title[0]) { gfx_draw_string(x + 16, cy, "Title: ", 0xFFAAAAAA); gfx_draw_string(x + 64, cy, page_title, 0xFFCCCCCC); cy += 14; }
        } else {
            gfx_draw_string(x + 16, cy, "No DOM document", 0xFF888888);
        }
    } else {
        gfx_draw_string(x + 8, cy, "Page Statistics:", 0xFFCCCCCC); cy += 16;
        char buf[64];
        snprintf(buf, 64, "Lines: %d", page_line_count);
        gfx_draw_string(x + 16, cy, buf, 0xFFAAAAAA); cy += 14;
        snprintf(buf, 64, "Links: %d", link_count);
        gfx_draw_string(x + 16, cy, buf, 0xFFAAAAAA); cy += 14;
        snprintf(buf, 64, "DOM: %s", use_dom_rendering ? "Yes" : "No");
        gfx_draw_string(x + 16, cy, buf, 0xFFAAAAAA); cy += 14;
        snprintf(buf, 64, "Zoom: %d%%", zoom_pct[zoom_level]);
        gfx_draw_string(x + 16, cy, buf, 0xFFAAAAAA); cy += 14;
        snprintf(buf, 64, "Source: %d bytes", source_len);
        gfx_draw_string(x + 16, cy, buf, 0xFFAAAAAA); cy += 14;
    }
}

// ============================================================
// SECTION 15: Main Paint Function (Massively Upgraded v2)
// ============================================================

static void browser_on_paint(window_t* win, int x, int y, int w, int h) {
    // Process find-on-page debounce timer
    if (find_debounce > 0) {
        find_debounce--;
        if (find_debounce == 0 && find_active && find_query[0]) {
            find_update_matches();
        }
    }

    // Background
    gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);

    // --- Tab Bar (Safari-style, improved) ---
    // Gradient background for tab bar
    for (int i = 0; i < TAB_BAR_H; i++) {
        uint32_t col = (i < TAB_BAR_H / 2) ? 0xFFF0F0F2 : 0xFFE6E6EA;
        gfx_fill_rect(x, y + i, w, 1, col);
    }
    gfx_draw_rect(x, y + TAB_BAR_H - 1, w, 1, COL_SEPARATOR);

    int tab_w = 160;
    if (tab_count > 0 && tab_w * tab_count > w - 40) tab_w = (w - 40) / tab_count;
    if (tab_w < 60) tab_w = 60;

    int tx = x;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        int is_act = (i == active_tab);
        uint32_t tab_bg = is_act ? COL_TAB_ACTIVE_BG : COL_TAB_INACTIVE_BG;
        uint32_t tab_text_col = is_act ? COL_TEXT_DARK : COL_TEXT_MUTED;

        if (is_act) {
            // Active tab: rounded top with white background, slight shadow
            gfx_fill_rounded_rect(tx + 1, y + 1, tab_w - 2, TAB_BAR_H - 2, tab_bg, 8);
            // Active indicator line at top
            gfx_fill_rect(tx + 4, y, tab_w - 8, 2, COL_ACCENT_BLUE);
        } else {
            gfx_fill_rounded_rect(tx + 1, y + 2, tab_w - 2, TAB_BAR_H - 4, tab_bg, 6);
        }

        // Tab title
        const char* ttitle = tabs[i].page_title[0] ? tabs[i].page_title :
                             (tabs[i].url[0] ? tabs[i].url : "New Tab");
        char display[24];
        int tlen = strlen(ttitle);
        if (tlen > 18) tlen = 18;
        strncpy(display, ttitle, tlen); display[tlen] = 0;
        gfx_draw_string(tx + 10, y + 10, display, tab_text_col);

        // Close button (styled circle, only visible when tab_count > 1)
        if (tab_count > 1) {
            int close_x = tx + tab_w - 20;
            gfx_fill_rounded_rect(close_x, y + 9, 14, 14, is_act ? 0xFFE0E0E0 : 0xFFD0D0D0, 7);
            gfx_draw_string(close_x + 3, y + 10, "x", is_act ? 0xFF888888 : 0xFFAAAAAA);
        }

        tx += tab_w;
    }

    // New tab button (+) styled
    if (tab_count < MAX_TABS) {
        gfx_fill_rounded_rect(tx + 6, y + 8, 22, 20, 0xFFE8E8ED, 4);
        gfx_draw_string(tx + 11, y + 10, "+", COL_TEXT_MUTED);
    }

    // --- URL/Toolbar (improved with more buttons) ---
    int tb_y = y + TAB_BAR_H;
    for (int i = 0; i < URL_BAR_H; i++) {
        uint32_t col = (i < URL_BAR_H / 2) ? 0xFFF5F5F8 : 0xFFF0F0F4;
        gfx_fill_rect(x, tb_y + i, w, 1, col);
    }
    gfx_draw_rect(x, tb_y + URL_BAR_H - 1, w, 1, COL_SEPARATOR);

    int bx = x + 8;
    int btn_sz = 28;
    int btn_h = 28;
    int btn_y = tb_y + (URL_BAR_H - btn_h) / 2;
    int btn_cy = tb_y + URL_BAR_H / 2;

    // Back button (with hover effect styling)
    int can_back = (history_pos > 0);
    gfx_fill_rounded_rect(bx, btn_y, btn_sz, btn_h, can_back ? 0xFFE8E8ED : 0xFFF0F0F0, 6);
    draw_arrow_left(bx + btn_sz/2, btn_cy, can_back ? 0xFF555555 : 0xFFCCCCCC);
    bx += btn_sz + 2;

    // Forward button
    int can_fwd = (history_pos < history_count - 1);
    gfx_fill_rounded_rect(bx, btn_y, btn_sz, btn_h, can_fwd ? 0xFFE8E8ED : 0xFFF0F0F0, 6);
    draw_arrow_right(bx + btn_sz/2, btn_cy, can_fwd ? 0xFF555555 : 0xFFCCCCCC);
    bx += btn_sz + 2;

    // Refresh/Stop button
    gfx_fill_rounded_rect(bx, btn_y, btn_sz, btn_h, 0xFFE8E8ED, 6);
    if (is_loading) {
        gfx_draw_line(bx + 9, btn_cy - 4, bx + 19, btn_cy + 4, 0xFF555555);
        gfx_draw_line(bx + 19, btn_cy - 4, bx + 9, btn_cy + 4, 0xFF555555);
    } else {
        draw_refresh_icon(bx + btn_sz/2, btn_cy, 0xFF555555);
    }
    bx += btn_sz + 2;

    // Home button
    gfx_fill_rounded_rect(bx, btn_y, btn_sz, btn_h, 0xFFE8E8ED, 6);
    draw_home_icon(bx + btn_sz/2, btn_cy, 0xFF555555);
    bx += btn_sz + 4;

    // URL input field (with subtle shadow)
    int url_right_margin = 130;
    int url_w = w - (bx - x) - url_right_margin;
    if (url_w < 60) url_w = 60;
    int url_field_y = btn_y;
    // Shadow
    gfx_fill_rounded_rect(bx + 1, url_field_y + 2, url_w, btn_h, 0x10000000, 6);
    // Field
    gfx_fill_rounded_rect(bx, url_field_y, url_w, btn_h, COL_URL_BAR_BG, 6);
    gfx_draw_rect(bx, url_field_y, url_w, btn_h, url_active ? COL_URL_BAR_FOCUS : COL_URL_BAR_BORDER);

    // Progress bar overlay in URL field
    if (load_progress > 0 && load_progress < 100) {
        int progress_w = ((url_w - 2) * load_progress) / 100;
        uint32_t pcol = (load_progress >= 90) ? COL_SUCCESS_GREEN : COL_ACCENT_BLUE;
        gfx_fill_rect(bx + 1, url_field_y + btn_h - 3, progress_w, 2, pcol);
    }

    // Shield icon for HTTPS
    int text_x = bx + 8;
    if (strncmp(url_buf, "https://", 8) == 0) {
        draw_shield_icon(text_x + 4, btn_cy, COL_SUCCESS_GREEN);
        text_x += 18;
    }

    // URL text
    int max_chars = (url_w - 20) / 8;
    int scroll_chars = 0;
    if (url_cursor > max_chars) scroll_chars = url_cursor - max_chars + 5;
    char display_url[256];
    strncpy(display_url, url_buf + scroll_chars, max_chars);
    display_url[max_chars] = 0;
    gfx_draw_string(text_x, url_field_y + (btn_h - 14) / 2, display_url, COL_TEXT_DARK);

    // Cursor blink
    if (url_active) {
        static int blink = 0; blink++;
        if (blink % 60 < 30) {
            int cur_x = text_x + (url_cursor - scroll_chars) * 8;
            if (cur_x < bx + url_w - 4)
                gfx_fill_rect(cur_x, url_field_y + (btn_h - 14) / 2, 1, 14, COL_URL_BAR_FOCUS);
        }
    }

    // Go button
    int go_x = bx + url_w + 4;
    gfx_fill_rounded_rect(go_x, btn_y, 28, btn_h, COL_ACCENT_BLUE, 6);
    gfx_draw_string(go_x + 7, url_field_y + (btn_h - 14) / 2, "Go", 0xFFFFFFFF);

    // Reading mode button
    int read_x = go_x + 32;
    gfx_fill_rounded_rect(read_x, btn_y, btn_sz, btn_h, reading_mode ? COL_WARNING_ORANGE : 0xFFE8E8ED, 6);
    draw_reading_icon(read_x + btn_sz/2, btn_cy, reading_mode ? 0xFFFFFFFF : 0xFF555555);

    // Download indicator button
    int dl_x = read_x + btn_sz + 2;
    gfx_fill_rounded_rect(dl_x, btn_y, btn_sz, btn_h, download_active ? COL_ACCENT_BLUE : 0xFFE8E8ED, 6);
    draw_download_icon(dl_x + btn_sz/2, btn_cy, download_active ? 0xFFFFFFFF : 0xFF555555);

    // Sidebar toggle (bookmarks/history)
    int side_x = dl_x + btn_sz + 2;
    gfx_fill_rounded_rect(side_x, btn_y, btn_sz, btn_h, (show_bookmarks || history_sidebar_active) ? COL_WARNING_ORANGE : 0xFFE8E8ED, 6);
    gfx_draw_string(side_x + 8, url_field_y + (btn_h - 14) / 2, "=", (show_bookmarks || history_sidebar_active) ? 0xFFFFFFFF : COL_TEXT_MUTED);

    // --- Calculate content layout ---
    int content_y_start = tb_y + URL_BAR_H;
    int sidebar_w = 0;

    // History sidebar
    if (history_sidebar_active) {
        sidebar_w = 180;
        // Draw sidebar background
        gfx_fill_rect(x, content_y_start, sidebar_w, h - (content_y_start - y) - STATUS_BAR_H, 0xFFFAFAFA);
        gfx_draw_rect(x + sidebar_w, content_y_start, 1, h - (content_y_start - y) - STATUS_BAR_H, COL_SEPARATOR);

        // Sidebar title
        gfx_fill_rect(x, content_y_start, sidebar_w, 26, 0xFFF0F0F2);
        gfx_draw_string(x + 10, content_y_start + 6, "History", COL_TEXT_DARK);
        content_y_start += 26;

        // History entries
        int hy = content_y_start;
        for (int hi = history_count - 1; hi >= 0 && hy < y + h - STATUS_BAR_H - 20; hi--) {
            int is_current = (hi == history_pos);
            uint32_t entry_bg = is_current ? 0xFFE8F0FF : 0x00000000;
            if (is_current) gfx_fill_rect(x + 2, hy, sidebar_w - 4, 20, entry_bg);

            // Small arrow for current
            if (is_current) {
                gfx_draw_string(x + 6, hy + 3, ">", COL_ACCENT_BLUE);
            }
            // Truncated URL
            char trunc[22];
            int hlen = strlen(history[hi]);
            if (hlen > 20) { strncpy(trunc, history[hi], 18); trunc[18] = '.'; trunc[19] = '.'; trunc[20] = 0; }
            else strcpy(trunc, history[hi]);
            gfx_draw_string(x + 18, hy + 3, trunc, is_current ? COL_ACCENT_BLUE : COL_TEXT_MUTED);
            hy += 22;
        }
        content_y_start = tb_y + URL_BAR_H + 26; // Reset for main content
    }

    // Bookmark bar
    if (show_bookmarks) {
        gfx_fill_rect(x + sidebar_w, content_y_start, w - sidebar_w, BOOKMARK_BAR_H, 0xFFFFF8E8);
        gfx_draw_rect(x + sidebar_w, content_y_start + BOOKMARK_BAR_H - 1, w - sidebar_w, 1, 0xFFE0D8C0);
        int bmx = x + sidebar_w + 8;
        for (int bi = 0; bi < bookmark_count && bmx < x + w - 60; bi++) {
            int bm_w = strlen(bookmarks[bi].name) * 8 + 16;
            if (bmx + bm_w > x + w - 10) break;
            gfx_fill_rounded_rect(bmx, content_y_start + 3, bm_w, 20, 0xFFFFF0D0, 4);
            gfx_draw_rect(bmx, content_y_start + 3, bm_w, 20, 0xFFE0D8C0);
            gfx_draw_string(bmx + 8, content_y_start + 7, bookmarks[bi].name, 0xFF8B6914);
            bmx += bm_w + 4;
        }
        content_y_start += BOOKMARK_BAR_H;
    }

    // Find bar
    if (find_active) {
        gfx_fill_rect(x + sidebar_w, content_y_start, w - sidebar_w, FIND_BAR_H, 0xFFFFF8E0);
        gfx_draw_rect(x + sidebar_w, content_y_start + FIND_BAR_H - 1, w - sidebar_w, 1, 0xFFE0D0A0);
        gfx_draw_string(x + sidebar_w + 8, content_y_start + 9, "Find:", COL_TEXT_DARK);

        int find_input_x = x + sidebar_w + 48;
        int find_input_w = w - sidebar_w - 200;
        if (find_input_w < 60) find_input_w = 60;
        gfx_fill_rounded_rect(find_input_x, content_y_start + 5, find_input_w, 22, 0xFFFFFFFF, 3);
        gfx_draw_rect(find_input_x, content_y_start + 5, find_input_w, 22, COL_URL_BAR_BORDER);
        gfx_draw_string(find_input_x + 4, content_y_start + 9, find_query, COL_TEXT_DARK);

        // Find cursor
        if (!url_active) {
            static int fblink = 0; fblink++;
            if (fblink % 60 < 30) {
                int fcx = find_input_x + 4 + find_cursor * 8;
                if (fcx < find_input_x + find_input_w - 4)
                    gfx_fill_rect(fcx, content_y_start + 9, 1, 14, COL_ACCENT_BLUE);
            }
        }

        // Up/Down/Close buttons
        int fb_x = find_input_x + find_input_w + 6;
        gfx_fill_rounded_rect(fb_x, content_y_start + 5, 22, 22, 0xFFE8E8ED, 3);
        draw_arrow_left(fb_x + 11, content_y_start + 16, COL_TEXT_DARK);
        fb_x += 26;
        gfx_fill_rounded_rect(fb_x, content_y_start + 5, 22, 22, 0xFFE8E8ED, 3);
        draw_arrow_right(fb_x + 11, content_y_start + 16, COL_TEXT_DARK);
        fb_x += 30;
        if (find_match_count > 0) {
            char mc[16];
            snprintf(mc, 16, "%d/%d", find_current_match + 1, find_match_count);
            gfx_draw_string(fb_x, content_y_start + 9, mc, COL_TEXT_MUTED);
        } else if (find_query[0]) {
            gfx_draw_string(fb_x, content_y_start + 9, "0/0", COL_TEXT_MUTED);
        }
        gfx_draw_string(x + w - 24, content_y_start + 9, "X", COL_TEXT_MUTED);

        content_y_start += FIND_BAR_H;
    }

    // Status bar (moved up for calculation)
    int status_y = y + h - STATUS_BAR_H;

    // Dev tools panel
    int dev_y = 0, dev_h = 0;
    if (dev_tools_active) {
        dev_h = DEV_TOOLS_H;
        dev_y = status_y - dev_h;
        render_dev_tools(x + sidebar_w, dev_y, w - sidebar_w, dev_h);
    }

    // Shortcut hints bar (thin bar between content and status)
    int hints_h = SHORTCUT_BAR_H;
    int hints_y = status_y - hints_h;
    if (dev_tools_active) hints_y = dev_y - hints_h;

    // Content area
    int content_x = x + sidebar_w;
    int content_w_area = w - sidebar_w;
    int content_h = (dev_tools_active ? dev_y : hints_y) - content_y_start;
    if (content_h < 0) content_h = 0;
    int lh = zoom_line_h[zoom_level];
    int visible_lines = content_h / lh;

    // Clamp scroll offset
    if (scroll_offset < 0) scroll_offset = 0;
    if (use_dom_rendering && dom_doc) {
        int max_scroll = dom_doc->total_height - content_h;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    } else {
        int max_scroll = page_line_count - visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    }

    // Determine what to render
    if (url_buf[0] == 0) {
        // New Tab page
        render_new_tab_page(content_x, content_y_start, content_w_area, content_h);
    } else if (error_type != ERR_NONE) {
        // Error page
        render_error_page(content_x, content_y_start, content_w_area, content_h);
    } else if (view_source && source_html) {
        // View Source
        render_source_view(content_x, content_y_start, content_w_area, content_h);
    } else if (reading_mode && !use_dom_rendering) {
        // Reading Mode - stripped down, comfortable reading
        gfx_fill_rect(content_x, content_y_start, content_w_area, content_h, 0xFFFFFBF0);
        int read_margin = 60;
        int read_w = content_w_area - read_margin * 2;
        if (read_w < 100) { read_margin = 10; read_w = content_w_area - 20; }
        int read_lh = 18;
        int read_vis = content_h / read_lh;

        for (int i = 0; i < read_vis && (i + scroll_offset) < page_line_count; i++) {
            int ly = content_y_start + i * read_lh;
            int line_idx = i + scroll_offset;
            int lt = line_types[line_idx];

            if (lt == LINE_HR) {
                int rule_y = ly + read_lh / 2;
                gfx_fill_rect(content_x + read_margin, rule_y, read_w, 1, 0xFFD0D0D0);
                continue;
            }

            int text_x_off = content_x + read_margin;
            uint32_t text_color = 0xFF333333;

            switch (lt) {
                case LINE_H1: text_color = 0xFF1A1A1A; break;
                case LINE_H2: text_color = 0xFF222222; break;
                case LINE_H3: text_x_off += 8; break;
                case LINE_H4: text_x_off += 16; text_color = 0xFF444444; break;
                case LINE_LI:
                    { char* bp = page_lines[line_idx]; while (*bp) { if (*bp=='\x1e') *bp='\xE2'; bp++; } }
                    break;
                case LINE_QUOTE:
                    text_x_off += 24; text_color = 0xFF555555;
                    gfx_fill_rect(content_x + read_margin + 10, ly, 3, read_lh, 0xFFDDDDCC);
                    break;
                case LINE_PRE:
                    gfx_fill_rect(content_x + read_margin, ly - 1, read_w, read_lh + 2, 0xFFF5F5EE);
                    text_color = 0xFF222222;
                    break;
            }

            // Links in reading mode - subtle underline
            int is_link_line = 0;
            for (int li = 0; li < link_count; li++) {
                if (links[li].line == line_idx) { is_link_line = 1; break; }
            }

            if (is_link_line) {
                gfx_draw_string(text_x_off, ly, page_lines[line_idx], 0xFF3366AA);
                int tw = strlen(page_lines[line_idx]) * 8;
                gfx_fill_rect(text_x_off, ly + read_lh - 2, tw, 1, 0xFF3366AA);
            } else {
                gfx_draw_string(text_x_off, ly, page_lines[line_idx], text_color);
            }
        }
    } else if (use_dom_rendering && dom_doc) {
        // DOM Engine rendering
        gfx_set_clip(content_x + PAD, content_y_start, content_w_area - PAD*2 - 12, content_h);
        uint32_t* buffer = gfx_get_active_buffer();
        dom_render(dom_doc, buffer, content_x + PAD, content_y_start, content_w_area - PAD*2 - 12, content_h, scroll_offset);
        gfx_reset_clip();
    } else {
        // Line-by-line rendering with improved formatting
        gfx_set_clip(content_x, content_y_start, content_w_area, content_h);
        for (int i = 0; i < visible_lines && (i + scroll_offset) < page_line_count; i++) {
            int ly = content_y_start + i * lh;
            int line_idx = i + scroll_offset;
            int lt = line_types[line_idx];

            if (lt == LINE_HR) {
                int rule_y = ly + lh / 2;
                gfx_fill_rect(content_x + PAD, rule_y, content_w_area - PAD * 2 - 12, 1, 0xFFC0C0C0);
                continue;
            }

            int text_x_off = content_x + PAD;
            uint32_t text_color = COL_TEXT_DARK;
            int indent = 0;

            switch (lt) {
                case LINE_H1:
                    text_color = 0xFF1A1A1A;
                    // Draw subtle background highlight for H1
                    gfx_fill_rect(content_x + 2, ly, content_w_area - 4, lh, 0xFFF5F5F8);
                    break;
                case LINE_H2:
                    text_color = 0xFF222222;
                    gfx_fill_rect(content_x + 2, ly, content_w_area - 4, lh, 0xFFFAFAFA);
                    break;
                case LINE_H3: text_x_off += 8; text_color = 0xFF333333; break;
                case LINE_H4: text_x_off += 16; text_color = 0xFF444444; break;
                case LINE_H5: case LINE_H6: text_x_off += 24; text_color = 0xFF555555; break;
                case LINE_LI:
                    { char* bp = page_lines[line_idx]; while (*bp) { if (*bp=='\x1e') *bp='\xE2'; bp++; } }
                    // Draw bullet point
                    gfx_fill_rounded_rect(text_x_off + 4, ly + lh/2 - 2, 4, 4, COL_ACCENT_BLUE, 2);
                    text_x_off += 14;
                    break;
                case LINE_QUOTE:
                    text_x_off += 20; text_color = 0xFF666666;
                    gfx_fill_rect(content_x + PAD + 8, ly, 3, lh, 0xFFCCCCCC);
                    break;
                case LINE_PRE:
                    gfx_fill_rect(content_x + PAD, ly - 1, content_w_area - PAD*2 - 12, lh + 2, 0xFFF5F5F5);
                    gfx_draw_rect(content_x + PAD, ly - 1, content_w_area - PAD*2 - 12, lh + 2, 0xFFE8E8E8);
                    text_color = 0xFF222222;
                    break;
            }

            // Check if line has a link
            int is_link_line = 0;
            for (int li = 0; li < link_count; li++) {
                if (links[li].line == line_idx) { is_link_line = 1; break; }
            }

            // Draw find highlights first (behind text)
            if (find_active && find_query[0]) {
                for (int fi = 0; fi < find_match_count; fi++) {
                    if (find_matches[fi].line == line_idx) {
                        int hx = text_x_off + find_matches[fi].col * 8;
                        int hw = find_matches[fi].len * 8;
                        uint32_t hl_col = (fi == find_current_match) ? COL_FIND_CURRENT : COL_FIND_HIGHLIGHT;
                        gfx_fill_rect(hx, ly, hw, lh, hl_col);
                    }
                }
            }

            if (is_link_line) {
                gfx_draw_string(text_x_off, ly, page_lines[line_idx], COL_LINK_BLUE);
                int tw = strlen(page_lines[line_idx]) * 8;
                gfx_fill_rect(text_x_off, ly + lh - 2, tw, 1, COL_LINK_BLUE);
            } else {
                gfx_draw_string(text_x_off, ly, page_lines[line_idx], text_color);
            }

            // Heading underlines
            if (lt == LINE_H1 || lt == LINE_H2) {
                int tw = strlen(page_lines[line_idx]) * 8;
                uint32_t ul_color = (lt == LINE_H1) ? 0xFF333333 : 0xFF888888;
                gfx_fill_rect(text_x_off, ly + lh - 1, tw, 1, ul_color);
            }
        }
        gfx_reset_clip();
    }

    // Scrollbar (improved: thinner, semi-transparent)
    if (use_dom_rendering && dom_doc) {
        int total_h = dom_doc->total_height;
        if (total_h > content_h) {
            int sb_x = content_x + content_w_area - 10;
            int thumb_h = (content_h * content_h) / total_h;
            if (thumb_h < 20) thumb_h = 20;
            int max_px = total_h - content_h;
            int thumb_y_pos = (max_px > 0) ? content_y_start + (scroll_offset * (content_h - thumb_h)) / max_px : content_y_start;
            gfx_fill_rect(sb_x, content_y_start, 8, content_h, 0x10C0C0C0);
            gfx_fill_rounded_rect(sb_x + 1, thumb_y_pos, 6, thumb_h, 0xFFC0C0C0, 3);
        }
    } else if (page_line_count > visible_lines && url_buf[0] != 0) {
        int sb_x = content_x + content_w_area - 10;
        int thumb_h = (visible_lines * content_h) / page_line_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y_pos = content_y_start + (scroll_offset * (content_h - thumb_h)) / (page_line_count - visible_lines);
        gfx_fill_rect(sb_x, content_y_start, 8, content_h, 0x10C0C0C0);
        gfx_fill_rounded_rect(sb_x + 1, thumb_y_pos, 6, thumb_h, 0xFFC0C0C0, 3);
    }

    // Shortcut hints bar
    gfx_fill_rect(content_x, hints_y, content_w_area, hints_h, 0xFFF8F8FA);
    gfx_draw_rect(content_x, hints_y, content_w_area, 1, COL_SEPARATOR);
    // Clip hint text to available width to prevent overflow on small windows
    {
        const char* hint_text = "L:URL  B:Back  F:Fwd  R:Reload  V:Source  D:Dev  +/-:Zoom  0:Reset";
        int hint_max_w = content_w_area - 16;  // 8px padding each side
        if (hint_max_w > 0) {
            int hint_chars = hint_max_w / 8;  // 8px per character
            if (hint_chars > 0) {
                char hint_buf[80];
                int hint_len = strlen(hint_text);
                if (hint_len > hint_chars) hint_len = hint_chars;
                strncpy(hint_buf, hint_text, hint_len);
                hint_buf[hint_len] = 0;
                gfx_draw_string(content_x + 8, hints_y + 4, hint_buf, 0xFFAAAAAA);
            }
        }
    }

    // Status bar (enhanced with more info)
    gfx_fill_rect(x, status_y, w, STATUS_BAR_H, COL_STATUS_BAR_BG);
    gfx_draw_rect(x, status_y, w, 1, COL_SEPARATOR);

    // Status text (clipped to prevent overflow on small windows)
    {
        int status_max_w = w - 16;  // Leave 8px padding each side
        int status_max_chars = status_max_w / 8;
        if (status_max_chars > 0) {
            char status_clipped[256];
            int slen = strlen(status_text);
            if (slen > status_max_chars) slen = status_max_chars;
            strncpy(status_clipped, status_text, slen);
            status_clipped[slen] = 0;
            gfx_draw_string(x + 8, status_y + 6, status_clipped, COL_TEXT_MUTED);
        }
    }

    // Loading indicator
    if (is_loading) {
        static int dots = 0; dots++;
        int nd = (dots / 20) % 4;
        char loading[16] = "Loading";
        for (int d = 0; d < nd; d++) strcat(loading, ".");
        gfx_draw_string(x + w - 260, status_y + 6, loading, COL_ACCENT_BLUE);
    }

    // Download progress bar
    if (download_active || download_progress == 100) {
        int bar_x = x + 8; int bar_y2 = status_y + STATUS_BAR_H - 3; int bar_w2 = w - 16;
        gfx_fill_rect(bar_x, bar_y2, bar_w2, 2, 0xFFE0E0E0);
        int fill_w = (bar_w2 * download_progress) / 100;
        uint32_t bar_col = download_progress >= 100 ? COL_SUCCESS_GREEN : COL_ACCENT_BLUE;
        if (fill_w > 0) gfx_fill_rect(bar_x, bar_y2, fill_w, 2, bar_col);
    }

    // Page size info
    if (page_size_bytes > 0 && !is_loading) {
        char sz_buf[32];
        if (page_size_bytes > 1024) snprintf(sz_buf, 32, "%d KB", page_size_bytes / 1024);
        else snprintf(sz_buf, 32, "%d B", page_size_bytes);
        gfx_draw_string(x + w - 200, status_y + 6, sz_buf, 0xFFBBBBBB);
    }

    // Load time info
    if (page_load_time > 0 && !is_loading) {
        char lt_buf[32];
        snprintf(lt_buf, 32, "%dms", page_load_time);
        gfx_draw_string(x + w - 150, status_y + 6, lt_buf, 0xFFBBBBBB);
    }

    // Zoom indicator
    if (zoom_level != 1) {
        char zbuf[16];
        snprintf(zbuf, 16, "%d%%", zoom_pct[zoom_level]);
        gfx_draw_string(x + w - 100, status_y + 6, zbuf, COL_TEXT_MUTED);
    }

    // HTTPS indicator
    if (strncmp(url_buf, "https://", 8) == 0) {
        gfx_draw_string(x + w - 70, status_y + 6, "[TLS]", COL_SUCCESS_GREEN);
    }

    // Tab count indicator
    {
        char tc[16];
        snprintf(tc, 16, "%d tabs", tab_count);
        gfx_draw_string(x + w - 50, status_y + 6, tc, COL_TEXT_MUTED);
    }

    // Context menu (overlay)
    if (ctx_menu_active) {
        render_context_menu(x, y);
    }
}

// ============================================================
// SECTION 16: Event Handlers
// ============================================================

static void browser_on_scroll(window_t* win, int delta) {
    if (ctx_menu_active) { ctx_menu_hide(); return; }
    if (view_source) {
        scroll_offset -= delta * 3;
    } else if (use_dom_rendering) {
        scroll_offset -= delta * 30;
    } else {
        scroll_offset -= delta * 3;
    }
    if (scroll_offset < 0) scroll_offset = 0;
}

static void browser_on_resize(window_t* win, int new_w, int new_h) {
    browser_win_w = new_w;
    browser_win_h = new_h;
}

static void browser_on_input(window_t* win, int key) {
    if (key == 0) return;

    // Global shortcuts that work regardless of focus
    if (key == 6) {  // Ctrl+F: Find
        find_active = !find_active;
        if (find_active) { url_active = 0; find_query[0] = 0; find_cursor = 0; find_match_count = 0; find_current_match = -1; }
        else { find_match_count = 0; find_current_match = -1; }
        return;
    }
    if (key == 21) {  // Ctrl+U: View Source
        view_source = !view_source;
        tab_save_state();
        return;
    }
    if (key == 4) {  // Ctrl+D: Dev Tools
        dev_tools_active = !dev_tools_active;
        tab_save_state();
        return;
    }

    // Close context menu on any key
    if (ctx_menu_active) { ctx_menu_hide(); return; }

    if (find_active && !url_active) {
        // Find bar input
        if (key == 27) {
            find_active = 0; find_match_count = 0; find_current_match = -1;
            return;
        }
        if (key == '\n') {
            if (find_match_count > 0) find_next();
            else { find_update_matches(); if (find_match_count > 0) find_next(); }
            return;
        }
        if (key == '\b') {
            if (find_cursor > 0) {
                find_cursor--;
                for (int i = find_cursor; find_query[i]; i++) find_query[i] = find_query[i+1];
                find_debounce = FIND_DEBOUNCE_FRAMES;  // Debounce: search after user stops typing
            }
            return;
        }
        if (key >= 32 && key != 127 && find_cursor < 62) {
            int len = strlen(find_query);
            for (int i = len; i > find_cursor; i--) find_query[i] = find_query[i-1];
            find_query[find_cursor++] = (char)key;
            find_query[len + 1] = 0;
            find_debounce = FIND_DEBOUNCE_FRAMES;  // Debounce: search after user stops typing
            return;
        }
        return;
    }

    if (url_active) {
        if (key == '\n') {
            browser_navigate(url_buf);
        } else if (key == '\b') {
            if (url_cursor > 0) {
                url_cursor--;
                for (int i = url_cursor; url_buf[i]; i++) url_buf[i] = url_buf[i+1];
            }
        } else if (key == 27) {
            url_active = 0;
        } else if (key >= 32 && key != 127 && key < 127 && url_cursor < 254) {
            int len = strlen(url_buf);
            for (int i = len; i > url_cursor; i--) url_buf[i] = url_buf[i-1];
            url_buf[url_cursor++] = (char)key;
            url_buf[len + 1] = 0;
        }
    } else {
        // Content area shortcuts
        if (key == 'l' || key == 'L') url_active = 1;
        else if (key == 'b' || key == 'B') browser_go_back();
        else if (key == 'f' || key == 'F') browser_go_forward();
        else if (key == 'r' || key == 'R') { if (history_pos >= 0) browser_load_page(url_buf); }
        else if (key == 'g' || key == 'G') { find_active = 1; find_query[0] = 0; find_cursor = 0; find_match_count = 0; find_current_match = -1; }
        else if (key == 'v' || key == 'V') { view_source = !view_source; tab_save_state(); }
        else if (key == 'd' || key == 'D') { dev_tools_active = !dev_tools_active; tab_save_state(); }
        else if (key == '+' || key == '=') { if (zoom_level < 2) { zoom_level++; tab_save_state(); } }
        else if (key == '-' || key == '_') { if (zoom_level > 0) { zoom_level--; tab_save_state(); } }
        else if (key == '0') { zoom_level = 1; tab_save_state(); }
        else if (key == 'h' || key == 'H') { url_buf[0] = 0; url_cursor = 0; page_line_count = 0; scroll_offset = 0; link_count = 0; page_title[0] = 0; error_type = ERR_NONE; reading_mode = 0; view_source = 0; strcpy(status_text, "New Tab"); if (browser_window) ws_set_title((window_t*)browser_window, "Browser"); }
        else if (key == 'm' || key == 'M') { reading_mode = !reading_mode; tab_save_state(); }
        else if (key == 'e' || key == 'E') { history_sidebar_active = !history_sidebar_active; }
    }
}

static void browser_on_mouse(window_t* win, int lx, int ly, int btn) {
    // Right-click: context menu
    if (btn == 2) {
        ctx_menu_show(lx, ly);
        return;
    }

    if (btn != 1) return;

    // Context menu handling
    if (ctx_menu_active) {
        int menu_w = 180;
        int item_h = 22;
        int mx = ctx_menu_x, my = ctx_menu_y;

        if (lx >= mx && lx < mx + menu_w) {
            int iy = my;
            for (int i = 0; i < ctx_menu_count; i++) {
                int ih = ctx_menu_items[i].is_separator ? 6 : item_h;
                if (ly >= iy && ly < iy + ih && !ctx_menu_items[i].is_separator) {
                    ctx_menu_exec(ctx_menu_items[i].action_id);
                    return;
                }
                iy += ih;
            }
        }
        ctx_menu_hide();
        return;
    }

    // Tab bar click
    if (ly >= 0 && ly < TAB_BAR_H) {
        int tab_w = 150;
        if (tab_count > 0 && tab_w * tab_count > browser_win_w - 40) tab_w = (browser_win_w - 40) / tab_count;
        if (tab_w < 60) tab_w = 60;

        int tx = 0;
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].active) continue;
            // Close button
            if (tab_count > 1 && lx >= tx + tab_w - 18 && lx < tx + tab_w && ly >= 2 && ly < TAB_BAR_H - 2) {
                tab_close(i);
                return;
            }
            // Tab click
            if (lx >= tx && lx < tx + tab_w) {
                tab_switch(i);
                return;
            }
            tx += tab_w;
        }

        // New tab button
        if (tab_count < MAX_TABS && lx >= tx + 4 && lx < tx + 24 && ly >= 7 && ly < 25) {
            tab_create(NULL);
            return;
        }
        return;
    }

    // URL bar area click
    if (ly >= TAB_BAR_H && ly < TAB_BAR_H + URL_BAR_H) {
        int bx = 8;
        int btn_sz = 28;

        // Back button
        if (lx >= bx && lx < bx + btn_sz) { browser_go_back(); return; }
        bx += btn_sz + 2;
        // Forward button
        if (lx >= bx && lx < bx + btn_sz) { browser_go_forward(); return; }
        bx += btn_sz + 2;
        // Refresh/Stop
        if (lx >= bx && lx < bx + btn_sz) {
            if (is_loading) { is_loading = 0; load_progress = 0; strcpy(status_text, "Stopped"); }
            else if (history_pos >= 0) browser_load_page(url_buf);
            return;
        }
        bx += btn_sz + 2;
        // Home button
        if (lx >= bx && lx < bx + btn_sz) {
            url_buf[0] = 0; url_cursor = 0; page_line_count = 0; scroll_offset = 0;
            link_count = 0; page_title[0] = 0; error_type = ERR_NONE; reading_mode = 0; view_source = 0;
            strcpy(status_text, "New Tab");
            if (browser_window) ws_set_title((window_t*)browser_window, "Browser");
            return;
        }
        bx += btn_sz + 4;

        // URL field
        int url_right_margin = 130;
        int url_w = browser_win_w - bx - url_right_margin;
        if (url_w < 60) url_w = 60;
        if (lx >= bx && lx < bx + url_w) {
            url_active = 1;
            // Position cursor
            int field_x = bx + 8;
            if (strncmp(url_buf, "https://", 8) == 0) field_x += 18;
            int click_col = (lx - field_x) / 8;
            if (click_col < 0) click_col = 0;
            if (click_col > (int)strlen(url_buf)) click_col = strlen(url_buf);
            url_cursor = click_col;
            return;
        }

        int go_x = bx + url_w + 4;
        // Go button
        if (lx >= go_x && lx < go_x + 28) { browser_navigate(url_buf); return; }
        // Reading mode button
        int read_x = go_x + 32;
        if (lx >= read_x && lx < read_x + btn_sz) { reading_mode = !reading_mode; tab_save_state(); return; }
        // Download indicator
        int dl_x = read_x + btn_sz + 2;
        if (lx >= dl_x && lx < dl_x + btn_sz) { /* download placeholder */ return; }
        // Sidebar toggle
        int side_x = dl_x + btn_sz + 2;
        if (lx >= side_x && lx < side_x + btn_sz) {
            // Cycle: bookmarks -> history -> off
            if (show_bookmarks) { show_bookmarks = 0; history_sidebar_active = 1; }
            else if (history_sidebar_active) { history_sidebar_active = 0; }
            else { show_bookmarks = 1; }
            return;
        }
        return;
    }

    // Calculate content_y_start for click detection
    int content_y_start = TAB_BAR_H + URL_BAR_H;
    if (show_bookmarks) {
        // Bookmark bar click
        if (ly >= content_y_start && ly < content_y_start + BOOKMARK_BAR_H) {
            int bmx = 8;
            for (int bi = 0; bi < bookmark_count; bi++) {
                int bm_w = strlen(bookmarks[bi].name) * 8 + 16;
                if (lx >= bmx && lx <= bmx + bm_w) { browser_navigate(bookmarks[bi].url); return; }
                bmx += bm_w + 4;
            }
            return;
        }
        content_y_start += BOOKMARK_BAR_H;
    }

    // Find bar click
    if (find_active) {
        if (ly >= content_y_start && ly < content_y_start + FIND_BAR_H) {
            url_active = 0;  // Focus find bar
            int find_input_x = 48;
            int find_input_w = browser_win_w - 200;
            if (find_input_w < 60) find_input_w = 60;

            // Find input
            if (lx >= find_input_x && lx < find_input_x + find_input_w) {
                find_cursor = (lx - find_input_x - 4) / 8;
                if (find_cursor < 0) find_cursor = 0;
                if (find_cursor > (int)strlen(find_query)) find_cursor = strlen(find_query);
                return;
            }

            int fb_x = find_input_x + find_input_w + 6;
            // Up button
            if (lx >= fb_x && lx < fb_x + 22) { find_prev(); return; }
            fb_x += 26;
            // Down button
            if (lx >= fb_x && lx < fb_x + 22) { find_next(); return; }
            // Close button
            if (lx >= browser_win_w - 24 && lx < browser_win_w - 8) { find_active = 0; find_match_count = 0; find_current_match = -1; return; }
            return;
        }
        content_y_start += FIND_BAR_H;
    }

    // Dev tools click
    if (dev_tools_active) {
        int status_y = browser_win_h - STATUS_BAR_H;
        int dev_top = status_y - DEV_TOOLS_H;
        if (ly >= dev_top && ly < dev_top + 24) {
            int tab_w = 70;
            for (int i = 0; i < 3; i++) {
                if (lx >= i * tab_w && lx < (i + 1) * tab_w) { dev_tools_tab = i; return; }
            }
            return;
        }
    }

    // Content area click
    if (ly > content_y_start) {
        url_active = 0;

        // New tab page shortcut clicks
        if (url_buf[0] == 0) {
            int cx = browser_win_w / 2;
            int search_y = browser_win_h / 5 + 36;
            // Search bar click -> focus URL bar
            int search_w = 360;
            if (search_w > browser_win_w - 40) search_w = browser_win_w - 40;
            int search_x = cx - search_w / 2;
            if (ly >= search_y && ly < search_y + 28 && lx >= search_x && lx < search_x + search_w) {
                url_active = 1;
                return;
            }

            // Shortcut tiles
            int tile_w = 80, tile_h = 70, tile_gap = 16;
            int grid_w = 4 * tile_w + 3 * tile_gap;
            int grid_x = cx - grid_w / 2;
            int grid_y = search_y + 48;

            for (int i = 0; i < 8; i++) {
                int col = i % 4, row = i / 4;
                int tx = grid_x + col * (tile_w + tile_gap);
                int ty = grid_y + row * (tile_h + tile_gap);
                if (lx >= tx && lx < tx + tile_w && ly >= ty && ly < ty + tile_h) {
                    browser_navigate(shortcuts[i].url);
                    return;
                }
            }
            return;
        }

        // Link click in content
        int lh = zoom_line_h[zoom_level];
        int click_line = (ly - content_y_start) / lh + scroll_offset;
        if (click_line >= 0 && click_line < page_line_count) {
            for (int li = 0; li < link_count; li++) {
                if (links[li].line == click_line) {
                    int link_x_start = PAD + links[li].col * 8;
                    int link_x_end = link_x_start + links[li].len * 8;
                    if (lx >= link_x_start && lx <= link_x_end) {
                        browser_navigate(links[li].url);
                        return;
                    }
                }
            }
        }
    }
}

// ============================================================
// SECTION 17: Init
// ============================================================

static char browser_launch_url[256] = "";

void init_browser_app() {
    // Initialize globals
    page_line_count = 0;
    url_buf[0] = 0;
    url_cursor = 0;
    history_pos = -1;
    history_count = 0;
    scroll_offset = 0;
    is_loading = 0;
    load_progress = 0;
    link_count = 0;
    page_title[0] = 0;
    dom_doc = 0;
    use_dom_rendering = 0;
    source_html = 0;
    source_len = 0;
    view_source = 0;
    error_type = ERR_NONE;
    error_detail[0] = 0;
    find_active = 0;
    find_match_count = 0;
    find_current_match = -1;
    dev_tools_active = 0;
    dev_console_count = 0;
    zoom_level = 1;
    ctx_menu_active = 0;
    reading_mode = 0;
    history_sidebar_active = 0;
    page_load_start = 0;
    page_load_time = 0;
    page_size_bytes = 0;
    recent_visit_count = 0;
    strcpy(status_text, "New Tab");

    // Initialize first tab
    memset(tabs, 0, sizeof(tabs));
    tabs[0].active = 1;
    strcpy(tabs[0].status_text, "New Tab");
    tabs[0].history_pos = -1;
    tab_count = 1;
    active_tab = 0;

    browser_window = fw_create_window("Browser", 700, 500, browser_on_paint, browser_on_input, browser_on_mouse);
    browser_window->min_w = 400;

    ((window_t*)browser_window)->scroll_callback = (void*)browser_on_scroll;
    ((window_t*)browser_window)->resize_callback = (void*)browser_on_resize;

    ((window_t*)browser_window)->menu_count = 5;

    strcpy(((window_t*)browser_window)->menus[0].name, "File");
    strcpy(((window_t*)browser_window)->menus[0].items[0].label, "New Tab");
    strcpy(((window_t*)browser_window)->menus[0].items[1].label, "Close Tab");
    strcpy(((window_t*)browser_window)->menus[0].items[2].label, "Close Window");
    ((window_t*)browser_window)->menus[0].item_count = 3;

    strcpy(((window_t*)browser_window)->menus[1].name, "Edit");
    strcpy(((window_t*)browser_window)->menus[1].items[0].label, "Find");
    strcpy(((window_t*)browser_window)->menus[1].items[1].label, "Copy URL");
    strcpy(((window_t*)browser_window)->menus[1].items[2].label, "Paste");
    ((window_t*)browser_window)->menus[1].item_count = 3;

    strcpy(((window_t*)browser_window)->menus[2].name, "View");
    strcpy(((window_t*)browser_window)->menus[2].items[0].label, "Refresh");
    strcpy(((window_t*)browser_window)->menus[2].items[1].label, "Source");
    strcpy(((window_t*)browser_window)->menus[2].items[2].label, "Reading Mode");
    strcpy(((window_t*)browser_window)->menus[2].items[3].label, "Dev Tools");
    strcpy(((window_t*)browser_window)->menus[2].items[4].label, "Zoom In");
    strcpy(((window_t*)browser_window)->menus[2].items[5].label, "Zoom Out");
    ((window_t*)browser_window)->menus[2].item_count = 6;

    strcpy(((window_t*)browser_window)->menus[3].name, "Bookmarks");
    strcpy(((window_t*)browser_window)->menus[3].items[0].label, "Show Bookmarks");
    strcpy(((window_t*)browser_window)->menus[3].items[1].label, "Add Current");
    ((window_t*)browser_window)->menus[3].item_count = 2;

    strcpy(((window_t*)browser_window)->menus[4].name, "History");
    strcpy(((window_t*)browser_window)->menus[4].items[0].label, "Back");
    strcpy(((window_t*)browser_window)->menus[4].items[1].label, "Forward");
    strcpy(((window_t*)browser_window)->menus[4].items[2].label, "Show History");
    ((window_t*)browser_window)->menus[4].item_count = 3;

    fw_register_dock("Browser", 5, browser_window);

    if (browser_launch_url[0] != 0) {
        browser_navigate(browser_launch_url);
        browser_launch_url[0] = 0;
    }
}

void init_browser_app_with_url(const char* url) {
    strncpy(browser_launch_url, url, 255);
    browser_launch_url[255] = 0;
    init_browser_app();
}
