// usr/apps/browser_cdl.c - Enhanced browser with tabs, CSS, cache, and better HTML handling
// Version 2.1 - FIXED: cache restore, entity decoding, rgb() colors, link ancestor walk,
//               centering reset, mouse coords, noscript support, content buffer size
#include "../../sys/cdl_defs.h"
#include "../../include/types.h"

static kernel_api_t* sys = 0;

// ============================================================================
// CONFIGURATION
// ============================================================================
#define MAX_URL         256
#define MAX_CONTENT     196608     // FIX: was 48000 (48KB) — now 192KB to handle real pages
#define MAX_TITLE       128
#define MAX_LINKS       96
#define MAX_IMAGES      24
#define MAX_DOM_NODES   384
#define MAX_TEXT_RUNS   512
#define MAX_BOX_RUNS    256
#define HISTORY_SIZE    16
#define MAX_TABS        6
#define MAX_CSS_RULES   64
#define CACHE_SIZE      4          // FIX: reduced from 8 — each entry now holds 192KB content
#define MAX_STYLESHEETS 4

// Special margin value for auto margins
#define MARGIN_AUTO -1

// ============================================================================
// DEFAULT SEARCH ENGINE
// NOTE: Google.com requires working TLS 1.3 + HTTP/2 + JavaScript engine.
//       These are documented in the Priority 2/3 backlog below.
//       For now, using HTTP so at least DNS + TCP works; you will get a redirect
//       to HTTPS which will fail gracefully with an error message.
// ============================================================================
#define DEFAULT_HOME "http://www.google.com"
#define SEARCH_URL   "http://www.google.com/search?q="

// ============================================================================
// CSS STRUCTURES
// ============================================================================

typedef struct {
    char selector[64];
    uint32_t fg_color;
    uint32_t bg_color;
    int font_size;
    int font_weight;
    int font_style;
    int text_decoration;
    int display;
    int margin_top, margin_bottom, margin_left, margin_right;
    int padding_top, padding_bottom, padding_left, padding_right;
    int border_radius;
    int width;
    int height;
} css_rule_t;

typedef struct {
    uint32_t fg_color;
    uint32_t bg_color;
    int font_size;
    int font_weight;    // 400 normal, 700 bold
    int font_style;     // 0 normal, 1 italic
    int text_decoration; // 0 none, 1 underline
    int text_align;     // 0 left, 1 center, 2 right, 3 justify
    int display;        // 0 inline, 1 block, 2 none, 3 flex, 4 inline-block
    int margin_top, margin_bottom, margin_left, margin_right;
    int padding_top, padding_bottom, padding_left, padding_right;
    int border_radius;
    int is_link;
    int target_blank;
    int flex_direction, justify_content, align_items, align_self;
    int flex_wrap, flex_grow, flex_shrink, flex_basis, gap;
    int width, height, min_width, max_width;
    uint32_t border_color;
    int border_width, border_style;
    int border_top, border_right, border_bottom, border_left;
    int line_height;
    int overflow, visibility, position, z_index;
    int box_shadow_x, box_shadow_y, box_shadow_blur;
    uint32_t box_shadow_color;
} css_style_t;

// ============================================================================
// DOM STRUCTURES
// ============================================================================

typedef enum {
    DOM_DOCUMENT, DOM_ELEMENT, DOM_TEXT, DOM_COMMENT
} dom_node_type_t;

typedef enum {
    ELEM_UNKNOWN = 0,
    ELEM_HTML, ELEM_HEAD, ELEM_BODY, ELEM_TITLE,
    ELEM_DIV, ELEM_SPAN, ELEM_P, ELEM_BR, ELEM_HR,
    ELEM_H1, ELEM_H2, ELEM_H3, ELEM_H4, ELEM_H5, ELEM_H6,
    ELEM_A, ELEM_IMG, ELEM_UL, ELEM_OL, ELEM_LI,
    ELEM_TABLE, ELEM_TR, ELEM_TD, ELEM_TH, ELEM_THEAD, ELEM_TBODY,
    ELEM_FORM, ELEM_INPUT, ELEM_BUTTON, ELEM_TEXTAREA, ELEM_LABEL,
    ELEM_SELECT, ELEM_OPTION,
    ELEM_STRONG, ELEM_B, ELEM_EM, ELEM_I, ELEM_U,
    ELEM_CODE, ELEM_PRE, ELEM_BLOCKQUOTE,
    ELEM_SCRIPT, ELEM_STYLE, ELEM_META, ELEM_LINK,
    ELEM_HEADER, ELEM_FOOTER, ELEM_NAV, ELEM_MAIN, ELEM_SECTION,
    ELEM_ARTICLE, ELEM_ASIDE, ELEM_FIGURE, ELEM_FIGCAPTION,
    ELEM_DETAILS, ELEM_SUMMARY,
    ELEM_VIDEO, ELEM_AUDIO, ELEM_CANVAS, ELEM_IFRAME, ELEM_PICTURE,
    ELEM_SOURCE, ELEM_EMBED, ELEM_OBJECT, ELEM_PARAM, ELEM_TRACK,
    ELEM_SVG, ELEM_MATH, ELEM_DIALOG, ELEM_MENU, ELEM_MENUITEM,
    ELEM_PROGRESS, ELEM_METER, ELEM_TIME, ELEM_MARK, ELEM_RUBY,
    ELEM_RT, ELEM_RP, ELEM_BDI, ELEM_BDO, ELEM_WBR,
    ELEM_DATA, ELEM_OUTPUT, ELEM_DATALIST, ELEM_KEYGEN,
    ELEM_NOSCRIPT   // FIX: added — we show noscript content (no JS engine)
} element_type_t;

typedef struct dom_node {
    dom_node_type_t type;
    element_type_t  elem_type;
    char  tag_name[32];
    char* text_content;
    int   text_len;

    char href[MAX_URL];
    char src[MAX_URL];
    char alt[128];
    char id[64];
    char class_name[64];
    char target[16];
    char style_attr[256];
    char type_attr[32];

    css_style_t style;

    struct dom_node* parent;
    struct dom_node* first_child;
    struct dom_node* last_child;
    struct dom_node* next_sibling;

    int x, y, width, height, layout_computed;
} dom_node_t;

typedef struct {
    int x, y, width, height;
    uint32_t bg_color, border_color;
    int border_width, border_style, border_radius;
    int has_background, has_border, z_index;
    dom_node_t* node;
} box_run_t;

typedef struct {
    char text[256];
    int x, y, width, height;
    css_style_t style;
    int is_link;
    char link_url[MAX_URL];
    int target_blank;
    int line_height;
} text_run_t;

typedef struct {
    int x, y, width, height;
    char url[MAX_URL];
    int target_blank;
} link_region_t;

// ============================================================================
// PAGE CACHE  — FIX: now stores actual HTML so cache_restore actually works
// ============================================================================

typedef struct {
    char     url[MAX_URL];
    char     title[MAX_TITLE];
    int      content_len;
    uint32_t timestamp;
    int      valid;
    char     content[MAX_CONTENT];  // FIX: was missing — cache was storing nothing
} page_cache_t;

// NOTE: with MAX_CONTENT=192KB and CACHE_SIZE=4, cache uses ~768KB of BSS.
// Reduce CACHE_SIZE if your OS has tight memory constraints.
static page_cache_t page_cache[CACHE_SIZE];
static int cache_count = 0;

// ============================================================================
// TAB STRUCTURE
// ============================================================================

typedef struct {
    char url[MAX_URL];
    char title[MAX_TITLE];
    int  active;
    int  content_len;
    int  page_offset;
} browser_tab_t;

static browser_tab_t tabs[MAX_TABS];
static int current_tab = 0;
static int tab_count   = 1;

// ============================================================================
// GLOBAL STATE
// ============================================================================
char     current_url[MAX_URL];
char     page_content[MAX_CONTENT];
int      content_len = 0;
char     page_title[MAX_TITLE];
char     status[64];
int      page_offset = 0;

static dom_node_t  dom_nodes[MAX_DOM_NODES];
static int         dom_node_count = 0;
static dom_node_t* document = 0;

static text_run_t   text_runs[MAX_TEXT_RUNS];
static int          text_run_count = 0;
static box_run_t    box_runs[MAX_BOX_RUNS];
static int          box_run_count = 0;
static link_region_t link_regions[MAX_LINKS];
static int           link_region_count = 0;

typedef struct {
    char     url[MAX_URL];
    char     title[64];
    uint32_t timestamp;
} history_entry_t;
static history_entry_t history[HISTORY_SIZE];
static int history_pos   = -1;
static int history_count = 0;

static int search_mode     = 0;
static int url_cursor_pos  = 0;
static int url_cursor_blink = 0;
static int is_loading      = 0;
static int loading_dots    = 0;

// ============================================================================
// CSS DEFAULT STYLES
// ============================================================================

static css_style_t default_style = {
    .fg_color = 0xFF000000, .bg_color = 0xFFFFFFFF,
    .font_size = 14, .font_weight = 400, .font_style = 0,
    .text_decoration = 0, .text_align = 0, .display = 1,
    .margin_top = 8, .margin_bottom = 8, .margin_left = 0, .margin_right = 0,
    .padding_top = 0, .padding_bottom = 0, .padding_left = 0, .padding_right = 0,
    .border_radius = 0, .is_link = 0, .target_blank = 0,
    .flex_direction = 0, .justify_content = 0, .align_items = 0,
    .align_self = 0, .flex_wrap = 0, .flex_grow = 0, .flex_shrink = 1,
    .flex_basis = -1, .gap = 0, .width = 0, .height = 0,
    .min_width = 0, .max_width = 0,
    .border_color = 0xFF000000, .border_width = 0, .border_style = 0,
    .border_top = 0, .border_right = 0, .border_bottom = 0, .border_left = 0,
    .line_height = 18, .overflow = 0, .visibility = 0,
    .position = 0, .z_index = 0,
    .box_shadow_x = 0, .box_shadow_y = 0, .box_shadow_blur = 0,
    .box_shadow_color = 0x00000000
};

static css_style_t inline_style = {
    .fg_color = 0xFF000000, .bg_color = 0xFFFFFFFF,
    .font_size = 14, .font_weight = 400, .font_style = 0,
    .text_decoration = 0, .text_align = 0, .display = 0,
    .margin_top = 0, .margin_bottom = 0, .margin_left = 0, .margin_right = 0,
    .padding_top = 0, .padding_bottom = 0, .padding_left = 0, .padding_right = 0,
    .border_radius = 0, .is_link = 0, .target_blank = 0,
    .flex_direction = 0, .justify_content = 0, .align_items = 0,
    .align_self = 0, .flex_wrap = 0, .flex_grow = 0, .flex_shrink = 1,
    .flex_basis = -1, .gap = 0, .width = 0, .height = 0,
    .min_width = 0, .max_width = 0,
    .border_color = 0xFF000000, .border_width = 0, .border_style = 0,
    .border_top = 0, .border_right = 0, .border_bottom = 0, .border_left = 0,
    .line_height = 18, .overflow = 0, .visibility = 0,
    .position = 0, .z_index = 0,
    .box_shadow_x = 0, .box_shadow_y = 0, .box_shadow_blur = 0,
    .box_shadow_color = 0x00000000
};

// ============================================================================
// CACHE FUNCTIONS  — FIX: cache now actually saves and restores HTML content
// ============================================================================

static page_cache_t* cache_find(const char* url) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (page_cache[i].valid && sys->strcmp(page_cache[i].url, url) == 0)
            return &page_cache[i];
    }
    return 0;
}

static page_cache_t* cache_add(const char* url, const char* title,
                                const char* content, int len) {
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!page_cache[i].valid) { slot = i; break; }
        if (page_cache[i].timestamp < oldest) {
            oldest = page_cache[i].timestamp;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;

    page_cache_t* entry = &page_cache[slot];
    sys->strcpy(entry->url, url);
    sys->strcpy(entry->title, title);
    entry->content_len = len;
    entry->timestamp   = sys->get_ticks();
    entry->valid       = 1;

    // FIX: actually save the HTML so we can restore it
    int copy_len = len < MAX_CONTENT - 1 ? len : MAX_CONTENT - 1;
    sys->memcpy(entry->content, content, copy_len);
    entry->content[copy_len] = 0;

    if (cache_count < CACHE_SIZE) cache_count++;
    return entry;
}

static void cache_restore(page_cache_t* entry) {
    sys->strcpy(page_title, entry->title);

    // FIX: restore actual content and re-parse — previously this left a blank page
    content_len = entry->content_len;
    sys->memcpy(page_content, entry->content, content_len);
    page_content[content_len] = 0;

    dom_node_count = 0;
    document = 0;
    // document will be recreated by the caller before parse_html
    text_run_count = 0;
    box_run_count  = 0;
    link_region_count = 0;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static int str_casecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *s1 - *s2;
}

// FIX: parse_color rgb() was broken — str_casecmp("rgb(255,0,0)", "rgb") is never 0
// because it compared the FULL string. Now uses a prefix check.
static int parse_color(const char* color_str) {
    if (!color_str || !*color_str) return 0xFF000000;
    while (*color_str == ' ') color_str++;

    if (*color_str == '#') {
        color_str++;
        int len = sys->strlen((char*)color_str);
        uint32_t color = 0;
        for (int i = 0; i < len && i < 8; i++) {
            color <<= 4;
            char c = color_str[i];
            if (c >= '0' && c <= '9') color |= (c - '0');
            else if (c >= 'a' && c <= 'f') color |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') color |= (c - 'A' + 10);
            else break;
        }
        if (len <= 6) color |= 0xFF000000;
        return color;
    }

    // FIX: was str_casecmp(color_str, "rgb") == 0  which compares full strings
    // and can never match "rgb(255,0,0)".  Use strncmp on first 3 chars instead.
    if (color_str[0] == 'r' && color_str[1] == 'g' && color_str[2] == 'b') {
        const char* p = color_str + 3;
        // skip optional 'a' for rgba()
        if (*p == 'a') p++;
        while (*p && *p != '(') p++;
        if (*p == '(') p++;
        int r = 0, g = 0, b = 0;
        while (*p >= '0' && *p <= '9') r = r * 10 + (*p++ - '0');
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') g = g * 10 + (*p++ - '0');
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    // Named colors
    if (str_casecmp(color_str, "black")       == 0) return 0xFF000000;
    if (str_casecmp(color_str, "white")       == 0) return 0xFFFFFFFF;
    if (str_casecmp(color_str, "red")         == 0) return 0xFFFF0000;
    if (str_casecmp(color_str, "green")       == 0) return 0xFF008000;
    if (str_casecmp(color_str, "blue")        == 0) return 0xFF0000FF;
    if (str_casecmp(color_str, "yellow")      == 0) return 0xFFFFFF00;
    if (str_casecmp(color_str, "cyan")        == 0) return 0xFF00FFFF;
    if (str_casecmp(color_str, "magenta")     == 0) return 0xFFFF00FF;
    if (str_casecmp(color_str, "gray")        == 0) return 0xFF808080;
    if (str_casecmp(color_str, "grey")        == 0) return 0xFF808080;
    if (str_casecmp(color_str, "silver")      == 0) return 0xFFC0C0C0;
    if (str_casecmp(color_str, "maroon")      == 0) return 0xFF800000;
    if (str_casecmp(color_str, "olive")       == 0) return 0xFF808000;
    if (str_casecmp(color_str, "lime")        == 0) return 0xFF00FF00;
    if (str_casecmp(color_str, "aqua")        == 0) return 0xFF00FFFF;
    if (str_casecmp(color_str, "teal")        == 0) return 0xFF008080;
    if (str_casecmp(color_str, "navy")        == 0) return 0xFF000080;
    if (str_casecmp(color_str, "fuchsia")     == 0) return 0xFFFF00FF;
    if (str_casecmp(color_str, "purple")      == 0) return 0xFF800080;
    if (str_casecmp(color_str, "orange")      == 0) return 0xFFFFA500;
    if (str_casecmp(color_str, "pink")        == 0) return 0xFFFFC0CB;
    if (str_casecmp(color_str, "brown")       == 0) return 0xFFA52A2A;
    if (str_casecmp(color_str, "coral")       == 0) return 0xFFFF7F50;
    if (str_casecmp(color_str, "crimson")     == 0) return 0xFFDC143C;
    if (str_casecmp(color_str, "gold")        == 0) return 0xFFFFD700;
    if (str_casecmp(color_str, "indigo")      == 0) return 0xFF4B0082;
    if (str_casecmp(color_str, "khaki")       == 0) return 0xFFF0E68C;
    if (str_casecmp(color_str, "lavender")    == 0) return 0xFFE6E6FA;
    if (str_casecmp(color_str, "lightblue")   == 0) return 0xFFADD8E6;
    if (str_casecmp(color_str, "lightgray")   == 0) return 0xFFD3D3D3;
    if (str_casecmp(color_str, "lightgrey")   == 0) return 0xFFD3D3D3;
    if (str_casecmp(color_str, "lightgreen")  == 0) return 0xFF90EE90;
    if (str_casecmp(color_str, "lightyellow") == 0) return 0xFFFFFFE0;
    if (str_casecmp(color_str, "salmon")      == 0) return 0xFFFA8072;
    if (str_casecmp(color_str, "skyblue")     == 0) return 0xFF87CEEB;
    if (str_casecmp(color_str, "tomato")      == 0) return 0xFFFF6347;
    if (str_casecmp(color_str, "violet")      == 0) return 0xFFEE82EE;
    if (str_casecmp(color_str, "transparent") == 0) return 0x00000000;
    return 0xFF000000;
}

static int parse_size(const char* size_str) {
    if (!size_str || !*size_str) return 0;
    int value = 0;
    while (*size_str >= '0' && *size_str <= '9') {
        value = value * 10 + (*size_str - '0');
        size_str++;
    }
    if (*size_str == 'e' && *(size_str+1) == 'm') value = value * 14;
    else if (*size_str == 'p' && *(size_str+1) == 't') value = value * 96 / 72;
    return value;
}

static int parse_border_style(const char* v) {
    if (str_casecmp(v, "none")   == 0) return 0;
    if (str_casecmp(v, "solid")  == 0) return 1;
    if (str_casecmp(v, "dashed") == 0) return 2;
    if (str_casecmp(v, "dotted") == 0) return 3;
    return 0;
}

// ============================================================================
// HTML ENTITY DECODER  — FIX: entirely new — was completely absent before
// Decodes &amp; &lt; &gt; &nbsp; &quot; &apos; &#NNN; &#xNN;
// Operates in-place: decoded string is always <= original length.
// ============================================================================

static void decode_html_entities(char* text) {
    if (!text) return;
    char* src = text;
    char* dst = text;

    while (*src) {
        if (*src != '&') {
            *dst++ = *src++;
            continue;
        }

        // Find the semicolon terminator (max 10 chars ahead)
        const char* amp = src;
        const char* semi = src + 1;
        int found_semi = 0;
        for (int i = 0; i < 10 && *semi; i++, semi++) {
            if (*semi == ';') { found_semi = 1; break; }
        }

        if (!found_semi) {
            // Not a valid entity — emit literally
            *dst++ = *src++;
            continue;
        }

        // Length of name between & and ;
        int name_len = (int)(semi - src) - 1;  // excludes & and ;
        const char* name = src + 1;

        // Named entities
        int matched = 1;
        if      (name_len == 3 && sys->strncmp(name, "amp",  3) == 0) *dst++ = '&';
        else if (name_len == 2 && sys->strncmp(name, "lt",   2) == 0) *dst++ = '<';
        else if (name_len == 2 && sys->strncmp(name, "gt",   2) == 0) *dst++ = '>';
        else if (name_len == 4 && sys->strncmp(name, "nbsp", 4) == 0) *dst++ = ' ';
        else if (name_len == 4 && sys->strncmp(name, "quot", 4) == 0) *dst++ = '"';
        else if (name_len == 4 && sys->strncmp(name, "apos", 4) == 0) *dst++ = '\'';
        else if (name_len == 4 && sys->strncmp(name, "copy", 4) == 0) { *dst++ = '('; *dst++ = 'c'; *dst++ = ')'; }
        else if (name_len == 3 && sys->strncmp(name, "reg",  3) == 0) { *dst++ = '('; *dst++ = 'R'; *dst++ = ')'; }
        else if (name_len == 4 && sys->strncmp(name, "euro", 4) == 0) { *dst++ = 'E'; *dst++ = 'U'; *dst++ = 'R'; }
        else if (name_len == 5 && sys->strncmp(name, "mdash",5) == 0) { *dst++ = '-'; *dst++ = '-'; }
        else if (name_len == 5 && sys->strncmp(name, "ndash",5) == 0) *dst++ = '-';
        else if (name_len == 5 && sys->strncmp(name, "laquo",5) == 0) { *dst++ = '<'; *dst++ = '<'; }
        else if (name_len == 5 && sys->strncmp(name, "raquo",5) == 0) { *dst++ = '>'; *dst++ = '>'; }
        else if (name_len == 5 && sys->strncmp(name, "ldquo",5) == 0) *dst++ = '"';
        else if (name_len == 5 && sys->strncmp(name, "rdquo",5) == 0) *dst++ = '"';
        else if (name_len == 5 && sys->strncmp(name, "lsquo",5) == 0) *dst++ = '\'';
        else if (name_len == 5 && sys->strncmp(name, "rsquo",5) == 0) *dst++ = '\'';
        else if (name_len == 6 && sys->strncmp(name, "hellip",6)== 0) { *dst++ = '.'; *dst++ = '.'; *dst++ = '.'; }
        else if (name_len == 4 && sys->strncmp(name, "bull", 4) == 0) *dst++ = '*';
        else if (name_len == 5 && sys->strncmp(name, "times",5) == 0) *dst++ = 'x';
        else if (name_len == 6 && sys->strncmp(name, "divide",6)==0)  *dst++ = '/';
        else if (*name == '#') {
            // Numeric: &#NNN; or &#xNN;
            int codepoint = 0;
            const char* np = name + 1;
            if (*np == 'x' || *np == 'X') {
                np++;
                while (np < semi) {
                    char c = *np++;
                    if (c >= '0' && c <= '9') codepoint = codepoint * 16 + (c - '0');
                    else if (c >= 'a' && c <= 'f') codepoint = codepoint * 16 + (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') codepoint = codepoint * 16 + (c - 'A' + 10);
                }
            } else {
                while (np < semi && *np >= '0' && *np <= '9')
                    codepoint = codepoint * 10 + (*np++ - '0');
            }
            // Only emit printable ASCII range; replace others with '?'
            if (codepoint >= 32 && codepoint <= 126)
                *dst++ = (char)codepoint;
            else if (codepoint == 160)  // &nbsp; numeric form
                *dst++ = ' ';
            else
                *dst++ = '?';
        }
        else {
            // Unknown entity — emit literally
            *dst++ = *src++;
            matched = 0;
        }

        if (matched)
            src = (char*)semi + 1;  // skip past the semicolon
    }
    *dst = 0;
}

// ============================================================================
// URL RESOLUTION
// ============================================================================

static void extract_origin(const char* url, char* origin, int max_len) {
    if (!url || !*url) { origin[0] = 0; return; }
    const char* scheme_end = url;
    while (*scheme_end && *scheme_end != ':') scheme_end++;
    if (*scheme_end == ':') scheme_end += 3;
    else { sys->strncpy(origin, url, max_len - 1); origin[max_len-1] = 0; return; }
    const char* host_end = scheme_end;
    while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?' && *host_end != '#') host_end++;
    int origin_len = host_end - url;
    if (origin_len >= max_len) origin_len = max_len - 1;
    sys->strncpy(origin, url, origin_len);
    origin[origin_len] = 0;
}

static void str_cat(char* dest, const char* src);

static void resolve_url(const char* base_url, const char* relative_url, char* resolved, int max_len) {
    if (!relative_url || !*relative_url) {
        sys->strncpy(resolved, base_url, max_len - 1);
        resolved[max_len - 1] = 0;
        return;
    }
    // Absolute URL
    if (relative_url[0] == 'h' && relative_url[1] == 't') {
        sys->strncpy(resolved, relative_url, max_len - 1);
        resolved[max_len - 1] = 0;
        return;
    }
    // Protocol-relative
    if (relative_url[0] == '/' && relative_url[1] == '/') {
        const char* se = base_url;
        while (*se && *se != ':') se++;
        int slen = se - base_url + 1;
        if (slen >= max_len) slen = max_len - 1;
        sys->strncpy(resolved, base_url, slen);
        resolved[slen] = 0;
        str_cat(resolved, relative_url);
        return;
    }
    char origin[MAX_URL];
    extract_origin(base_url, origin, MAX_URL);
    if (relative_url[0] == '/') {
        sys->strncpy(resolved, origin, max_len - 1);
        resolved[max_len - 1] = 0;
        int rlen = sys->strlen(resolved);
        int rellen = sys->strlen(relative_url);
        if (rlen + rellen < max_len) sys->strcpy(resolved + rlen, relative_url);
    } else {
        sys->strncpy(resolved, origin, max_len - 1);
        resolved[max_len - 1] = 0;
        int rlen = sys->strlen(resolved);
        if (rlen + 1 < max_len) { resolved[rlen] = '/'; resolved[rlen+1] = 0; }
        rlen = sys->strlen(resolved);
        if (rlen + (int)sys->strlen(relative_url) < max_len)
            sys->strcpy(resolved + rlen, relative_url);
    }
    resolved[max_len - 1] = 0;
}

static int extract_google_redirect(const char* url, char* extracted, int max_len) {
    if (url[0]=='/'&&url[1]=='u'&&url[2]=='r'&&url[3]=='l'&&url[4]=='?') {
        const char* q = url + 5;
        while (*q) {
            if (q[0]=='q'&&q[1]=='=') {
                q += 2;
                int i = 0;
                while (*q && *q != '&' && i < max_len - 1) {
                    if (*q == '%' && q[1] && q[2]) {
                        char hex[3] = {q[1], q[2], 0};
                        int val = 0;
                        for (int j = 0; j < 2; j++) {
                            val <<= 4;
                            if (hex[j]>='0'&&hex[j]<='9') val |= hex[j]-'0';
                            else if (hex[j]>='a'&&hex[j]<='f') val |= hex[j]-'a'+10;
                            else if (hex[j]>='A'&&hex[j]<='F') val |= hex[j]-'A'+10;
                        }
                        extracted[i++] = (char)val;
                        q += 3;
                    } else { extracted[i++] = *q++; }
                }
                extracted[i] = 0;
                return 1;
            }
            q++;
        }
    }
    sys->strncpy(extracted, url, max_len - 1);
    extracted[max_len - 1] = 0;
    return 0;
}

// ============================================================================
// INLINE STYLE PARSER
// ============================================================================

static void parse_inline_style(const char* style_str, css_style_t* style) {
    if (!style_str || !*style_str) return;
    const char* p = style_str;
    char prop[64], value[128];
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        int pl = 0;
        while (*p && *p != ':' && pl < 63) prop[pl++] = *p++;
        prop[pl] = 0;
        if (*p == ':') p++;
        while (*p == ' ' || *p == '\t') p++;
        int vl = 0;
        while (*p && *p != ';' && vl < 127) value[vl++] = *p++;
        value[vl] = 0;
        if (*p == ';') p++;

        if (str_casecmp(prop,"color")==0) style->fg_color=parse_color(value);
        else if (str_casecmp(prop,"background-color")==0) style->bg_color=parse_color(value);
        else if (str_casecmp(prop,"background")==0) style->bg_color=parse_color(value);
        else if (str_casecmp(prop,"font-size")==0) {
            style->font_size=parse_size(value);
            if(style->font_size<8)style->font_size=8;
            if(style->font_size>72)style->font_size=72;
        }
        else if (str_casecmp(prop,"font-weight")==0) {
            if(str_casecmp(value,"bold")==0||str_casecmp(value,"700")==0)
                style->font_weight=700; else style->font_weight=400;
        }
        else if (str_casecmp(prop,"font-style")==0)
            style->font_style=(str_casecmp(value,"italic")==0)?1:0;
        else if (str_casecmp(prop,"text-decoration")==0)
            style->text_decoration=(sys->strstr(value,"underline")!=0)?1:0;
        else if (str_casecmp(prop,"display")==0) {
            if(str_casecmp(value,"none")==0) style->display=2;
            else if(str_casecmp(value,"block")==0) style->display=1;
            else if(str_casecmp(value,"flex")==0) style->display=3;
            else if(str_casecmp(value,"inline-block")==0) style->display=4;
            else style->display=0;
        }
        else if (str_casecmp(prop,"margin-top")==0)
            style->margin_top=(str_casecmp(value,"auto")==0)?MARGIN_AUTO:parse_size(value);
        else if (str_casecmp(prop,"margin-bottom")==0)
            style->margin_bottom=(str_casecmp(value,"auto")==0)?MARGIN_AUTO:parse_size(value);
        else if (str_casecmp(prop,"margin-left")==0)
            style->margin_left=(str_casecmp(value,"auto")==0)?MARGIN_AUTO:parse_size(value);
        else if (str_casecmp(prop,"margin-right")==0)
            style->margin_right=(str_casecmp(value,"auto")==0)?MARGIN_AUTO:parse_size(value);
        else if (str_casecmp(prop,"padding-top")==0)    style->padding_top=parse_size(value);
        else if (str_casecmp(prop,"padding-bottom")==0) style->padding_bottom=parse_size(value);
        else if (str_casecmp(prop,"padding-left")==0)   style->padding_left=parse_size(value);
        else if (str_casecmp(prop,"padding-right")==0)  style->padding_right=parse_size(value);
        else if (str_casecmp(prop,"border-radius")==0)  style->border_radius=parse_size(value);
        else if (str_casecmp(prop,"border-width")==0) {
            style->border_width=parse_size(value);
            style->border_top=style->border_right=style->border_bottom=style->border_left=style->border_width;
        }
        else if (str_casecmp(prop,"border-style")==0) style->border_style=parse_border_style(value);
        else if (str_casecmp(prop,"border-color")==0) style->border_color=parse_color(value);
        else if (str_casecmp(prop,"border")==0) {
            const char* vp=value;
            while(*vp==' ')vp++;
            style->border_width=parse_size(vp);
            style->border_top=style->border_right=style->border_bottom=style->border_left=style->border_width;
            while(*vp&&*vp!=' ')vp++; while(*vp==' ')vp++;
            style->border_style=parse_border_style(vp);
            while(*vp&&*vp!=' ')vp++; while(*vp==' ')vp++;
            if(*vp) style->border_color=parse_color(vp);
        }
        else if (str_casecmp(prop,"line-height")==0) {
            style->line_height=parse_size(value);
            if(style->line_height<12)style->line_height=12;
            if(style->line_height>48)style->line_height=48;
        }
        else if (str_casecmp(prop,"text-align")==0) {
            if(str_casecmp(value,"center")==0) style->text_align=1;
            else if(str_casecmp(value,"right")==0) style->text_align=2;
            else if(str_casecmp(value,"justify")==0) style->text_align=3;
            else style->text_align=0;
        }
        else if (str_casecmp(prop,"width")==0)  style->width=parse_size(value);
        else if (str_casecmp(prop,"height")==0) style->height=parse_size(value);
        else if (str_casecmp(prop,"visibility")==0) {
            if(str_casecmp(value,"hidden")==0)style->visibility=1;
            else style->visibility=0;
        }
    }
}

// ============================================================================
// ELEMENT TYPE LOOKUP
// ============================================================================

static element_type_t get_element_type(const char* tag) {
    if (str_casecmp(tag,"html")==0)    return ELEM_HTML;
    if (str_casecmp(tag,"head")==0)    return ELEM_HEAD;
    if (str_casecmp(tag,"body")==0)    return ELEM_BODY;
    if (str_casecmp(tag,"title")==0)   return ELEM_TITLE;
    if (str_casecmp(tag,"div")==0)     return ELEM_DIV;
    if (str_casecmp(tag,"span")==0)    return ELEM_SPAN;
    if (str_casecmp(tag,"p")==0)       return ELEM_P;
    if (str_casecmp(tag,"br")==0)      return ELEM_BR;
    if (str_casecmp(tag,"hr")==0)      return ELEM_HR;
    if (str_casecmp(tag,"h1")==0)      return ELEM_H1;
    if (str_casecmp(tag,"h2")==0)      return ELEM_H2;
    if (str_casecmp(tag,"h3")==0)      return ELEM_H3;
    if (str_casecmp(tag,"h4")==0)      return ELEM_H4;
    if (str_casecmp(tag,"h5")==0)      return ELEM_H5;
    if (str_casecmp(tag,"h6")==0)      return ELEM_H6;
    if (str_casecmp(tag,"a")==0)       return ELEM_A;
    if (str_casecmp(tag,"img")==0)     return ELEM_IMG;
    if (str_casecmp(tag,"ul")==0)      return ELEM_UL;
    if (str_casecmp(tag,"ol")==0)      return ELEM_OL;
    if (str_casecmp(tag,"li")==0)      return ELEM_LI;
    if (str_casecmp(tag,"table")==0)   return ELEM_TABLE;
    if (str_casecmp(tag,"tr")==0)      return ELEM_TR;
    if (str_casecmp(tag,"td")==0)      return ELEM_TD;
    if (str_casecmp(tag,"th")==0)      return ELEM_TH;
    if (str_casecmp(tag,"thead")==0)   return ELEM_THEAD;
    if (str_casecmp(tag,"tbody")==0)   return ELEM_TBODY;
    if (str_casecmp(tag,"form")==0)    return ELEM_FORM;
    if (str_casecmp(tag,"input")==0)   return ELEM_INPUT;
    if (str_casecmp(tag,"button")==0)  return ELEM_BUTTON;
    if (str_casecmp(tag,"textarea")==0)return ELEM_TEXTAREA;
    if (str_casecmp(tag,"strong")==0)  return ELEM_B;
    if (str_casecmp(tag,"b")==0)       return ELEM_B;
    if (str_casecmp(tag,"em")==0)      return ELEM_I;
    if (str_casecmp(tag,"i")==0)       return ELEM_I;
    if (str_casecmp(tag,"u")==0)       return ELEM_U;
    if (str_casecmp(tag,"code")==0)    return ELEM_CODE;
    if (str_casecmp(tag,"pre")==0)     return ELEM_PRE;
    if (str_casecmp(tag,"blockquote")==0) return ELEM_BLOCKQUOTE;
    if (str_casecmp(tag,"script")==0)  return ELEM_SCRIPT;
    if (str_casecmp(tag,"style")==0)   return ELEM_STYLE;
    if (str_casecmp(tag,"meta")==0)    return ELEM_META;
    if (str_casecmp(tag,"link")==0)    return ELEM_LINK;
    if (str_casecmp(tag,"header")==0)  return ELEM_HEADER;
    if (str_casecmp(tag,"footer")==0)  return ELEM_FOOTER;
    if (str_casecmp(tag,"nav")==0)     return ELEM_NAV;
    if (str_casecmp(tag,"main")==0)    return ELEM_MAIN;
    if (str_casecmp(tag,"section")==0) return ELEM_SECTION;
    if (str_casecmp(tag,"article")==0) return ELEM_ARTICLE;
    if (str_casecmp(tag,"aside")==0)   return ELEM_ASIDE;
    if (str_casecmp(tag,"figure")==0)  return ELEM_FIGURE;
    if (str_casecmp(tag,"figcaption")==0) return ELEM_FIGCAPTION;
    if (str_casecmp(tag,"details")==0) return ELEM_DETAILS;
    if (str_casecmp(tag,"summary")==0) return ELEM_SUMMARY;
    if (str_casecmp(tag,"label")==0)   return ELEM_LABEL;
    if (str_casecmp(tag,"select")==0)  return ELEM_SELECT;
    if (str_casecmp(tag,"option")==0)  return ELEM_OPTION;
    if (str_casecmp(tag,"video")==0)   return ELEM_VIDEO;
    if (str_casecmp(tag,"audio")==0)   return ELEM_AUDIO;
    if (str_casecmp(tag,"canvas")==0)  return ELEM_CANVAS;
    if (str_casecmp(tag,"iframe")==0)  return ELEM_IFRAME;
    if (str_casecmp(tag,"picture")==0) return ELEM_PICTURE;
    if (str_casecmp(tag,"source")==0)  return ELEM_SOURCE;
    if (str_casecmp(tag,"svg")==0)     return ELEM_SVG;
    if (str_casecmp(tag,"math")==0)    return ELEM_MATH;
    if (str_casecmp(tag,"dialog")==0)  return ELEM_DIALOG;
    if (str_casecmp(tag,"progress")==0)return ELEM_PROGRESS;
    if (str_casecmp(tag,"mark")==0)    return ELEM_MARK;
    if (str_casecmp(tag,"time")==0)    return ELEM_TIME;
    if (str_casecmp(tag,"wbr")==0)     return ELEM_WBR;
    if (str_casecmp(tag,"output")==0)  return ELEM_OUTPUT;
    if (str_casecmp(tag,"datalist")==0)return ELEM_DATALIST;
    if (str_casecmp(tag,"noscript")==0)return ELEM_NOSCRIPT;  // FIX: added
    return ELEM_UNKNOWN;
}

static css_style_t get_element_style(element_type_t elem_type, dom_node_t* parent) {
    css_style_t style = (parent && parent->style.display == 0) ? inline_style : default_style;
    switch (elem_type) {
        case ELEM_H1: style.font_size=32;style.font_weight=700;style.margin_top=24;style.margin_bottom=16;style.line_height=40;style.display=1;break;
        case ELEM_H2: style.font_size=26;style.font_weight=700;style.margin_top=20;style.margin_bottom=14;style.line_height=34;style.display=1;break;
        case ELEM_H3: style.font_size=22;style.font_weight=700;style.margin_top=18;style.margin_bottom=12;style.line_height=28;style.display=1;break;
        case ELEM_H4: style.font_size=18;style.font_weight=700;style.margin_top=16;style.margin_bottom=10;style.line_height=24;style.display=1;break;
        case ELEM_H5: style.font_size=16;style.font_weight=700;style.margin_top=14;style.margin_bottom=8;style.line_height=22;style.display=1;break;
        case ELEM_H6: style.font_size=14;style.font_weight=700;style.margin_top=12;style.margin_bottom=8;style.line_height=20;style.display=1;break;
        case ELEM_P:  style.margin_top=12;style.margin_bottom=12;style.line_height=20;style.display=1;break;
        case ELEM_DIV:style.display=1;style.margin_top=4;style.margin_bottom=4;break;
        case ELEM_BODY:case ELEM_HTML:
            style.display=1;style.margin_top=0;style.margin_bottom=0;
            style.padding_top=8;style.padding_bottom=8;style.padding_left=8;style.padding_right=8;break;
        case ELEM_MAIN:case ELEM_SECTION:case ELEM_ARTICLE:
            style.display=1;style.margin_top=8;style.margin_bottom=8;break;
        case ELEM_HEADER:style.display=1;style.margin_top=0;style.margin_bottom=12;style.padding_bottom=8;break;
        case ELEM_FOOTER:style.display=1;style.margin_top=12;style.margin_bottom=0;style.padding_top=8;break;
        case ELEM_NAV:style.display=1;style.margin_top=8;style.margin_bottom=8;break;
        case ELEM_ASIDE:
            style.display=1;style.margin_top=8;style.margin_bottom=8;
            style.padding_left=12;style.padding_right=12;style.bg_color=0xFFF5F5F5;break;
        case ELEM_SPAN: style.display=0;break;
        case ELEM_B:    style.font_weight=700;style.display=0;break;
        case ELEM_I:    style.font_style=1;style.display=0;break;
        case ELEM_U:    style.text_decoration=1;style.display=0;break;
        case ELEM_A:    style.fg_color=0xFF0066CC;style.text_decoration=1;style.is_link=1;style.display=0;break;
        case ELEM_CODE:
            style.font_size=13;style.bg_color=0xFFF5F5F5;
            style.padding_left=4;style.padding_right=4;style.border_radius=3;style.display=0;break;
        case ELEM_PRE:
            style.font_size=13;style.bg_color=0xFFF5F5F5;
            style.padding_top=12;style.padding_bottom=12;style.padding_left=16;style.padding_right=16;
            style.margin_top=12;style.margin_bottom=12;style.border_radius=4;style.line_height=16;style.display=1;break;
        case ELEM_BLOCKQUOTE:
            style.margin_top=16;style.margin_bottom=16;style.margin_left=24;
            style.padding_top=8;style.padding_bottom=8;style.padding_left=16;
            style.border_left=4;style.border_color=0xFFCCCCCC;
            style.fg_color=0xFF555555;style.bg_color=0xFFFAFAFA;style.display=1;break;
        case ELEM_UL:case ELEM_OL:
            style.margin_top=12;style.margin_bottom=12;style.padding_left=28;style.display=1;break;
        case ELEM_LI:style.margin_top=6;style.margin_bottom=6;style.display=1;break;
        case ELEM_TABLE:
            style.display=1;style.margin_top=12;style.margin_bottom=12;
            style.border_width=1;style.border_style=1;style.border_color=0xFFDDDDDD;break;
        case ELEM_TR:style.display=1;break;
        case ELEM_TD:style.display=0;style.padding_left=12;style.padding_right=12;style.padding_top=8;style.padding_bottom=8;break;
        case ELEM_TH:style.display=0;style.padding_left=12;style.padding_right=12;style.padding_top=8;style.padding_bottom=8;style.font_weight=700;style.bg_color=0xFFF0F0F0;break;
        case ELEM_INPUT:case ELEM_TEXTAREA:case ELEM_SELECT:
            style.display=0;style.bg_color=0xFFFFFFFF;style.border_radius=4;
            style.border_width=1;style.border_style=1;style.border_color=0xFFCCCCCC;
            style.padding_top=6;style.padding_bottom=6;style.padding_left=8;style.padding_right=8;break;
        case ELEM_BUTTON:
            style.display=0;style.bg_color=0xFF0066CC;style.fg_color=0xFFFFFFFF;
            style.border_radius=4;style.padding_top=6;style.padding_bottom=6;
            style.padding_left=12;style.padding_right=12;break;
        case ELEM_SCRIPT:case ELEM_META:case ELEM_LINK:case ELEM_DATALIST:
            style.display=2;break;
        case ELEM_STYLE:
            style.display=2;break;
        case ELEM_IMG:style.display=1;style.margin_top=8;style.margin_bottom=8;break;
        case ELEM_HR:
            style.display=1;style.margin_top=16;style.margin_bottom=16;
            style.border_width=1;style.border_style=1;style.border_color=0xFFCCCCCC;break;
        case ELEM_MARK:style.display=0;style.bg_color=0xFFFFFF00;break;
        case ELEM_PROGRESS:case ELEM_METER:
            style.display=1;style.margin_top=8;style.margin_bottom=8;
            style.height=20;style.border_radius=4;style.bg_color=0xFFE0E0E0;break;
        case ELEM_FIGURE:style.display=1;style.margin_top=12;style.margin_bottom=12;break;
        case ELEM_FIGCAPTION:style.font_size=12;style.fg_color=0xFF666666;style.text_align=1;style.display=1;break;
        // FIX: NOSCRIPT content should be shown (we have no JS engine)
        case ELEM_NOSCRIPT:
            style.display=1;style.margin_top=8;style.margin_bottom=8;
            style.padding_top=8;style.padding_bottom=8;style.padding_left=12;style.padding_right=12;
            style.bg_color=0xFFFFF8E1;style.border_left=3;style.border_color=0xFFFFB300;break;
        default: break;
    }
    return style;
}

// ============================================================================
// DOM FUNCTIONS
// ============================================================================

static dom_node_t* dom_create_node(dom_node_type_t type) {
    if (dom_node_count >= MAX_DOM_NODES) return 0;
    dom_node_t* node = &dom_nodes[dom_node_count++];
    sys->memset(node, 0, sizeof(dom_node_t));
    node->type = type;
    return node;
}

static void dom_append_child(dom_node_t* parent, dom_node_t* child) {
    if (!parent || !child) return;
    child->parent = parent;
    if (!parent->first_child) { parent->first_child = parent->last_child = child; }
    else { parent->last_child->next_sibling = child; parent->last_child = child; }
}

// ============================================================================
// HTML PARSER
// ============================================================================

typedef enum {
    PARSE_DATA, PARSE_TAG_OPEN, PARSE_TAG_NAME, PARSE_TAG_CLOSE,
    PARSE_ATTR_NAME, PARSE_ATTR_VALUE, PARSE_COMMENT, PARSE_DOCTYPE
} parse_state_t;

static parse_state_t  parse_state;
static dom_node_t*    current_element;
static int            skip_depth;

#define MAX_SCRIPT_SIZE 8192
static char script_buffer[MAX_SCRIPT_SIZE];
static int  script_buffer_len = 0;
static int  in_script = 0;

// Forward declarations
static void execute_document_write(const char* html);

static void execute_script_content(const char* script) {
    if (!script || !script[0]) return;
    const char* p = script;
    while ((p = sys->strstr(p, "document.write")) != 0) {
        p += 14;
        while (*p==' '||*p=='\t'||*p=='\n') p++;
        if (*p == '(') {
            p++;
            while (*p==' '||*p=='\t'||*p=='\n') p++;
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                char buf[4096]; int bl = 0;
                while (*p && *p != quote && bl < 4095) {
                    if (*p == '\\' && *(p+1)) {
                        p++;
                        switch(*p) {
                            case 'n': buf[bl++]='\n';break; case 't': buf[bl++]='\t';break;
                            case '\\': buf[bl++]='\\';break; case '"': buf[bl++]='"';break;
                            case '\'': buf[bl++]='\'';break; default: buf[bl++]=*p;break;
                        }
                        p++;
                    } else { buf[bl++] = *p++; }
                }
                buf[bl] = 0;
                execute_document_write(buf);
            }
        }
    }
}

static void execute_document_write(const char* html) {
    if (!html || !html[0]) return;
    int html_len = sys->strlen(html);
    if (content_len + html_len >= MAX_CONTENT - 1) return;
    sys->strcpy(page_content + content_len, html);
    content_len += html_len;
}

static void handle_start_tag(const char* tag_name, char* attrs, int attrs_len) {
    element_type_t elem_type = get_element_type(tag_name);

    if (elem_type == ELEM_SCRIPT) {
        in_script = 1; script_buffer_len = 0; script_buffer[0] = 0;
        return;
    }
    if (elem_type == ELEM_STYLE) { skip_depth = 1; return; }
    if (elem_type == ELEM_META || elem_type == ELEM_LINK) return;

    dom_node_t* node = dom_create_node(DOM_ELEMENT);
    if (!node) return;

    node->elem_type = elem_type;
    sys->strncpy(node->tag_name, tag_name, 31);
    node->style = get_element_style(elem_type, current_element);

    char* attr_ptr = attrs;
    while (attr_ptr < attrs + attrs_len) {
        while (*attr_ptr==' '||*attr_ptr=='\t'||*attr_ptr=='\n') attr_ptr++;
        if (!*attr_ptr) break;
        char* name_start = attr_ptr;
        while (*attr_ptr && *attr_ptr != '=' && *attr_ptr != ' ' && *attr_ptr != '>') attr_ptr++;
        int name_len = attr_ptr - name_start;
        if (name_len == 0) break;
        char value[256] = {0};
        if (*attr_ptr == '=') {
            attr_ptr++;
            if (*attr_ptr == '"' || *attr_ptr == '\'') {
                char quote = *attr_ptr++;
                char* vs = attr_ptr;
                while (*attr_ptr && *attr_ptr != quote) attr_ptr++;
                int vl = attr_ptr - vs;
                if (vl > 0 && vl < 256) sys->strncpy(value, vs, vl);
                if (*attr_ptr) attr_ptr++;
            } else {
                char* vs = attr_ptr;
                while (*attr_ptr && *attr_ptr != ' ' && *attr_ptr != '>') attr_ptr++;
                int vl = attr_ptr - vs;
                if (vl > 0 && vl < 256) sys->strncpy(value, vs, vl);
            }
        }
        if (name_len == 4 && sys->strncmp(name_start, "href", 4) == 0)
            sys->strncpy(node->href, value, MAX_URL - 1);
        else if (name_len == 3 && sys->strncmp(name_start, "src", 3) == 0)
            sys->strncpy(node->src, value, MAX_URL - 1);
        else if (name_len == 3 && sys->strncmp(name_start, "alt", 3) == 0)
            sys->strncpy(node->alt, value, 127);
        else if (name_len == 2 && sys->strncmp(name_start, "id", 2) == 0)
            sys->strncpy(node->id, value, 63);
        else if (name_len == 5 && sys->strncmp(name_start, "class", 5) == 0)
            sys->strncpy(node->class_name, value, 63);
        else if (name_len == 6 && sys->strncmp(name_start, "target", 6) == 0) {
            sys->strncpy(node->target, value, 15);
            if (str_casecmp(value, "_blank") == 0) node->style.target_blank = 1;
        }
        else if (name_len == 5 && sys->strncmp(name_start, "style", 5) == 0) {
            sys->strncpy(node->style_attr, value, 255);
            parse_inline_style(value, &node->style);
        }
        else if (name_len == 4 && sys->strncmp(name_start, "type", 4) == 0)
            sys->strncpy(node->type_attr, value, 31);
    }

    dom_append_child(current_element, node);

    if (elem_type != ELEM_BR && elem_type != ELEM_HR && elem_type != ELEM_IMG &&
        elem_type != ELEM_INPUT && elem_type != ELEM_META && elem_type != ELEM_LINK &&
        elem_type != ELEM_SOURCE && elem_type != ELEM_PARAM && elem_type != ELEM_WBR)
        current_element = node;
}

static void handle_end_tag(const char* tag_name) {
    element_type_t elem_type = get_element_type(tag_name);

    if (elem_type == ELEM_SCRIPT && in_script) {
        in_script = 0;
        script_buffer[script_buffer_len] = 0;
        execute_script_content(script_buffer);
        script_buffer_len = 0;
        return;
    }
    if (skip_depth > 0) {
        if (elem_type == ELEM_STYLE) skip_depth = 0;
        return;
    }
    dom_node_t* node = current_element;
    while (node && node->type == DOM_ELEMENT) {
        if (str_casecmp(node->tag_name, tag_name) == 0) {
            current_element = node->parent;
            return;
        }
        node = node->parent;
    }
}

static void handle_text(const char* text, int len) {
    if (in_script) {
        if (script_buffer_len + len < MAX_SCRIPT_SIZE - 1) {
            sys->memcpy(script_buffer + script_buffer_len, text, len);
            script_buffer_len += len;
        }
        return;
    }
    if (skip_depth > 0) return;
    if (len == 0) return;

    int has_content = 0;
    for (int i = 0; i < len; i++) { if ((unsigned char)text[i] > ' ') { has_content = 1; break; } }
    if (!has_content) return;

    dom_node_t* node = dom_create_node(DOM_TEXT);
    if (!node) return;

    // FIX: decode HTML entities in text content before storing
    node->text_content = (char*)sys->malloc(len + 1);
    if (node->text_content) {
        sys->memcpy(node->text_content, text, len);
        node->text_content[len] = 0;
        decode_html_entities(node->text_content);
        node->text_len = sys->strlen(node->text_content);
    }

    if (current_element) node->style = current_element->style;

    dom_append_child(current_element, node);

    if (current_element && current_element->elem_type == ELEM_TITLE) {
        int copy_len = len < MAX_TITLE - 1 ? len : MAX_TITLE - 1;
        sys->strncpy(page_title, text, copy_len);
        page_title[copy_len] = 0;
    }
}

static void parse_html(const char* html) {
    parse_state = PARSE_DATA;
    current_element = document;
    skip_depth = 0;

    char tag_name[64]; int tag_name_len = 0;
    char attrs[512];   int attrs_len = 0;
    char text_buffer[4096]; int text_len = 0;

    const char* p = html;
    while (*p) {
        char c = *p;
        switch (parse_state) {
            case PARSE_DATA:
                if (c == '<') {
                    if (text_len > 0) { handle_text(text_buffer, text_len); text_len = 0; }
                    parse_state = PARSE_TAG_OPEN;
                } else {
                    if (text_len < 4095) text_buffer[text_len++] = c;
                }
                break;
            case PARSE_TAG_OPEN:
                if (c == '!') {
                    if (p[1]=='-'&&p[2]=='-') { parse_state=PARSE_COMMENT; p+=2; }
                    else { parse_state=PARSE_DOCTYPE; }
                } else if (c == '/') {
                    parse_state = PARSE_TAG_CLOSE; tag_name_len = 0;
                } else if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')) {
                    parse_state = PARSE_TAG_NAME;
                    tag_name[0] = c; tag_name_len = 1; attrs_len = 0;
                } else { parse_state = PARSE_DATA; }
                break;
            case PARSE_TAG_NAME:
                if (c==' '||c=='\t'||c=='\n') {
                    tag_name[tag_name_len] = 0; parse_state = PARSE_ATTR_NAME;
                } else if (c == '>') {
                    tag_name[tag_name_len] = 0;
                    handle_start_tag(tag_name, attrs, attrs_len);
                    parse_state = PARSE_DATA;
                } else if (c=='/'&&p[1]=='>') {
                    tag_name[tag_name_len] = 0;
                    handle_start_tag(tag_name, attrs, attrs_len);
                    handle_end_tag(tag_name);
                    p++; parse_state = PARSE_DATA;
                } else if (tag_name_len < 63) { tag_name[tag_name_len++] = c; }
                break;
            case PARSE_ATTR_NAME:
                if (c == '>') {
                    handle_start_tag(tag_name, attrs, attrs_len); parse_state = PARSE_DATA;
                } else if (c=='/'&&p[1]=='>') {
                    handle_start_tag(tag_name, attrs, attrs_len);
                    handle_end_tag(tag_name); p++; parse_state = PARSE_DATA;
                } else if (attrs_len < 511) { attrs[attrs_len++] = c; }
                break;
            case PARSE_TAG_CLOSE:
                if (c == '>') {
                    tag_name[tag_name_len] = 0;
                    handle_end_tag(tag_name); parse_state = PARSE_DATA;
                } else if (tag_name_len < 63) { tag_name[tag_name_len++] = c; }
                break;
            case PARSE_COMMENT:
                if (c=='-'&&p[1]=='-'&&p[2]=='>') { p+=2; parse_state=PARSE_DATA; }
                break;
            case PARSE_DOCTYPE:
                if (c == '>') parse_state = PARSE_DATA;
                break;
            default: break;
        }
        p++;
    }
    if (text_len > 0) handle_text(text_buffer, text_len);
}

// ============================================================================
// LAYOUT AND RENDERING
// ============================================================================

static int text_width(const char* text, int font_size) {
    int len = sys->strlen(text);
    return len * (font_size / 2 + 4);
}

static void add_box_run(dom_node_t* node, int x, int y, int width, int height) {
    if (box_run_count >= MAX_BOX_RUNS) return;
    if (node->style.visibility == 1) return;
    box_run_t* box = &box_runs[box_run_count];
    box->x = x; box->y = y; box->width = width; box->height = height;
    box->bg_color     = node->style.bg_color;
    box->border_color = node->style.border_color;
    box->border_width = node->style.border_width;
    box->border_style = node->style.border_style;
    box->border_radius = node->style.border_radius;
    box->z_index = node->style.z_index;
    box->node = node;
    box->has_background = (node->style.bg_color != 0xFFFFFFFF &&
                           (node->style.bg_color & 0xFF000000) == 0xFF000000);
    box->has_border = (node->style.border_style > 0 && node->style.border_width > 0) ||
                      (node->style.border_top > 0 || node->style.border_right > 0 ||
                       node->style.border_bottom > 0 || node->style.border_left > 0);
    if (box->has_background || box->has_border) box_run_count++;
}

// FIX: Helper — walk ancestor chain to find nearest ELEM_A ancestor
static dom_node_t* find_link_ancestor(dom_node_t* node) {
    dom_node_t* n = node ? node->parent : 0;
    while (n) {
        if (n->elem_type == ELEM_A && n->href[0]) return n;
        n = n->parent;
    }
    return 0;
}

static void layout_dom(int content_width, int content_height) {
    text_run_count    = 0;
    box_run_count     = 0;
    link_region_count = 0;

    int y = 0, x = 0;
    int line_height = 16;
    int list_counter = 0, list_type = 0;
    int max_x = content_width - 32;
    int current_padding_left = 0;
    int current_padding_top  = 0;

    // FIX: centering_offset is now scoped per-element using a stack
    int centering_stack[32];
    int centering_top = 0;
    int centering_offset = 0;

    int block_stack[32];
    int block_stack_top = 0;
    int block_y_start[32];
    int block_x_start[32];

    dom_node_t* node = document->first_child;
    while (node) {
        if (node->type == DOM_TEXT) {
            char* text = node->text_content;
            if (!text) { node = node->next_sibling; continue; }

            int len = sys->strlen(text);
            int word_start = 0, word_len = 0;

            line_height = node->style.line_height;
            if (line_height < node->style.font_size + 4)
                line_height = node->style.font_size + 4;

            for (int i = 0; i <= len; i++) {
                if (text[i] <= ' ' || i == len) {
                    if (word_len > 0 && text_run_count < MAX_TEXT_RUNS) {
                        text_run_t* run = &text_runs[text_run_count++];
                        int copy_len = word_len < 255 ? word_len : 255;
                        sys->strncpy(run->text, text + word_start, copy_len);
                        run->text[copy_len] = 0;
                        run->width = word_len * (node->style.font_size / 2 + 4);

                        if (x + run->width > max_x && x > current_padding_left) {
                            y += line_height; x = current_padding_left;
                        }

                        run->x = x + current_padding_left + node->style.padding_left + centering_offset;
                        run->y = y + current_padding_top  + node->style.padding_top;
                        run->style = node->style;
                        run->height = line_height;
                        run->line_height = line_height;

                        if (node->style.text_align == 1)
                            run->x = (content_width - run->width) / 2;

                        // FIX: walk ancestor chain for ELEM_A
                        dom_node_t* link_anc = find_link_ancestor(node);
                        if (link_anc) {
                            run->is_link = 1;
                            run->target_blank = link_anc->style.target_blank;
                            sys->strcpy(run->link_url, link_anc->href);

                            if (link_region_count < MAX_LINKS) {
                                link_region_t* lr = &link_regions[link_region_count++];
                                lr->x = run->x;
                                lr->y = run->y;
                                lr->width = run->width;
                                lr->height = line_height;
                                lr->target_blank = link_anc->style.target_blank;
                                sys->strcpy(lr->url, link_anc->href);
                            }
                        } else {
                            run->is_link = 0;
                        }

                        x += run->width + 4;
                    }
                    word_start = i + 1;
                    word_len   = 0;
                    if (text[i] == '\n') { y += line_height; x = current_padding_left; }
                } else { word_len++; }
            }

        } else if (node->type == DOM_ELEMENT) {
            if (node->style.display == 2) {
                goto next_node;
            }

            int margin_top = node->style.margin_top;
            if (margin_top != MARGIN_AUTO && margin_top > 0) y += margin_top;

            line_height = node->style.line_height;
            if (line_height < node->style.font_size + 4)
                line_height = node->style.font_size + 4;

            if (node->style.display == 1 || node->style.display == 3) {
                if (x > current_padding_left) { y += line_height; x = current_padding_left; }

                // FIX: push the outgoing centering_offset before computing the new one
                if (centering_top < 32) {
                    centering_stack[centering_top++] = centering_offset;
                }

                centering_offset = 0;
                int ml = node->style.margin_left, mr = node->style.margin_right;
                if (ml == MARGIN_AUTO && mr == MARGIN_AUTO) {
                    int ew = node->style.width > 0 ? node->style.width : content_width - 32;
                    if (ew < content_width - 32) centering_offset = (content_width - 32 - ew) / 2;
                } else if (ml == MARGIN_AUTO) {
                    int ew = node->style.width > 0 ? node->style.width : content_width - 32;
                    int right = mr > 0 ? mr : 0;
                    if (ew + right < content_width - 32)
                        centering_offset = content_width - 32 - ew - right;
                }

                int block_x = (ml != MARGIN_AUTO && ml > 0) ? current_padding_left + ml : centering_offset;
                if (block_stack_top < 32) {
                    block_stack[block_stack_top]   = box_run_count;
                    block_y_start[block_stack_top] = y;
                    block_x_start[block_stack_top] = block_x;
                    block_stack_top++;
                }

                current_padding_left = node->style.padding_left;
                current_padding_top  = node->style.padding_top;
                y += node->style.padding_top;
            }

            if (node->elem_type == ELEM_UL)  { list_type = 1; list_counter = 0; }
            if (node->elem_type == ELEM_OL)  { list_type = 2; list_counter = 0; }
            if (node->elem_type == ELEM_LI)  {
                list_counter++;
                x = current_padding_left + node->style.padding_left;
                if (text_run_count < MAX_TEXT_RUNS) {
                    text_run_t* run = &text_runs[text_run_count++];
                    if (list_type == 1) { run->text[0]='\x95'; run->text[1]=' '; run->text[2]=0; }
                    else { run->text[0]='0'+(list_counter%10); run->text[1]='.'; run->text[2]=' '; run->text[3]=0; }
                    run->x = current_padding_left; run->y = y;
                    run->style = node->style; run->is_link = 0;
                    run->width = 20; run->line_height = line_height;
                }
                x = current_padding_left + 20;
            }
            if (node->elem_type == ELEM_IMG) {
                if (text_run_count < MAX_TEXT_RUNS) {
                    text_run_t* run = &text_runs[text_run_count++];
                    run->text[0] = '[';
                    int al = sys->strlen(node->alt);
                    int cl = al < 30 ? al : 30;
                    sys->strncpy(run->text + 1, node->alt, cl);
                    sys->strcpy(run->text + 1 + cl, "]");
                    run->x = x + current_padding_left; run->y = y;
                    run->style = node->style; run->style.bg_color = 0xFFEEEEEE;
                    run->is_link = 0; run->width = (cl + 2) * 8; run->line_height = line_height;
                }
                x += 100;
            }
            if (node->elem_type == ELEM_BR) { y += line_height; x = current_padding_left; }
            if (node->elem_type == ELEM_HR) {
                y += line_height;
                if (text_run_count < MAX_TEXT_RUNS) {
                    text_run_t* run = &text_runs[text_run_count++];
                    sys->strcpy(run->text, "________________________________________");
                    run->x=current_padding_left; run->y=y; run->style=node->style;
                    run->is_link=0; run->width=40*8; run->line_height=line_height;
                }
                y += line_height; x = current_padding_left;
            }
            if (node->elem_type == ELEM_INPUT) {
                if (text_run_count < MAX_TEXT_RUNS) {
                    text_run_t* run = &text_runs[text_run_count++];
                    sys->strcpy(run->text, "[ input ]");
                    run->x=x+current_padding_left; run->y=y; run->style=node->style;
                    run->is_link=0; run->width=80; run->line_height=line_height;
                    add_box_run(node, run->x-2, run->y-2, 84, line_height+4);
                }
                x += 90;
            }
            if (node->elem_type == ELEM_BUTTON) {
                if (text_run_count < MAX_TEXT_RUNS) {
                    text_run_t* run = &text_runs[text_run_count++];
                    sys->strcpy(run->text, "[Button]");
                    run->x=x+current_padding_left; run->y=y; run->style=node->style;
                    run->is_link=0; run->width=70; run->line_height=line_height;
                    add_box_run(node, run->x-4, run->y-2, 78, line_height+4);
                }
                x += 80;
            }
        }

        if (node->first_child && !(node->type == DOM_ELEMENT && node->style.display == 2)) {
            node = node->first_child;
            continue;
        }

        next_node:
        if (node->next_sibling) { node = node->next_sibling; continue; }

        while (node && !node->next_sibling) {
            node = node->parent;
            if (node && node->type == DOM_ELEMENT) {
                if (node->style.display == 1 || node->style.display == 3) {
                    y += node->style.padding_bottom;
                    if (block_stack_top > 0) {
                        block_stack_top--;
                        int block_h = y - block_y_start[block_stack_top] + node->style.padding_bottom;
                        add_box_run(node, block_x_start[block_stack_top],
                                   block_y_start[block_stack_top],
                                   content_width - 32, block_h);
                    }
                    if (node->parent && node->parent->type == DOM_ELEMENT) {
                        current_padding_left = node->parent->style.padding_left;
                        current_padding_top  = node->parent->style.padding_top;
                    } else { current_padding_left = 0; current_padding_top = 0; }

                    // FIX: restore the centering offset that was active before this element
                    if (centering_top > 0) {
                        centering_offset = centering_stack[--centering_top];
                    } else {
                        centering_offset = 0;
                    }
                }
                y += node->style.margin_bottom;
                if (node->style.display == 1 && x > current_padding_left) {
                    y += line_height; x = current_padding_left;
                }
                if (node->elem_type == ELEM_UL || node->elem_type == ELEM_OL) list_type = 0;
            }
        }
        if (node) node = node->next_sibling;
    }
}

// ============================================================================
// HTTP AND NAVIGATION
// ============================================================================

static void str_cat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = 0;
}

static void update_loading_status(void) {
    if (is_loading) {
        loading_dots = (loading_dots + 1) % 16;
        sys->strcpy(status, "Loading");
        for (int i = 0; i < (loading_dots / 4) + 1; i++) str_cat(status, ".");
    }
}

static void show_js_required_page(const char* url) {
    text_run_count = 0; box_run_count = 0;

    struct { const char* text; int x; int y; uint32_t color; int size; int bold; } msgs[] = {
        { "JavaScript Required", 20, 20, 0xFF222222, 22, 700 },
        { "This page needs JavaScript to render content.", 20, 60, 0xFF444444, 14, 400 },
        { "CamelOS Browser does not yet have a JS engine.", 20, 82, 0xFF444444, 14, 400 },
        { "What you can try:", 20, 116, 0xFF333333, 14, 700 },
        { "  - Append ?lite or ?hl=en to the URL", 20, 138, 0xFF555555, 13, 400 },
        { "  - Visit the site's /sitemap.xml for static links", 20, 158, 0xFF555555, 13, 400 },
        { "  - Try news.ycombinator.com or text.npr.org", 20, 178, 0xFF555555, 13, 400 },
        { "  - Wikipedia works well (static HTML)", 20, 198, 0xFF555555, 13, 400 },
        { "Technical status:", 20, 232, 0xFF333333, 14, 700 },
        { "  JS engine:    MISSING (Priority 3 backlog)", 20, 254, 0xFFBB2222, 13, 400 },
        { "  TLS 1.3:      INCOMPLETE (crypto stubs)", 20, 274, 0xFFBB2222, 13, 400 },
        { "  HTTP/2:       NOT WIRED (http2.c exists)", 20, 294, 0xFFBB5500, 13, 400 },
        { "  HTML parser:  WORKING (basic)", 20, 314, 0xFF228822, 13, 400 },
        { "  DOM layout:   WORKING (block/inline)", 20, 334, 0xFF228822, 13, 400 },
        { 0, 0, 0, 0, 0, 0 }
    };

    for (int i = 0; msgs[i].text && text_run_count < MAX_TEXT_RUNS; i++) {
        text_run_t* run = &text_runs[text_run_count++];
        sys->strcpy(run->text, msgs[i].text);
        run->x = msgs[i].x; run->y = msgs[i].y;
        run->style = default_style;
        run->style.fg_color   = msgs[i].color;
        run->style.font_size  = msgs[i].size;
        run->style.font_weight = msgs[i].bold;
        run->is_link = 0;
        run->width = sys->strlen(msgs[i].text) * (msgs[i].size / 2 + 4);
        run->line_height = msgs[i].size + 6;
    }

    sys->strcpy(page_title, "JavaScript Required");
}

int fetch_url(const char* url) {
    if (!sys) return -1;

    sys->strcpy(current_url, url);
    url_cursor_pos = sys->strlen(current_url);

    // Check cache — FIX: now actually restores content
    page_cache_t* cached = cache_find(url);
    if (cached) {
        cache_restore(cached);

        dom_node_count = 0;
        document = dom_create_node(DOM_DOCUMENT);
        parse_html(page_content);
        layout_dom(780, 500);

        sys->strcpy(current_url, url);
        sys->strcpy(tabs[current_tab].url, url);
        sys->strcpy(tabs[current_tab].title, page_title);
        status[0] = 0; is_loading = 0;
        return 0;
    }

    is_loading = 1; loading_dots = 0;
    sys->strcpy(status, "Loading...");

    sys->memset(page_content, 0, sizeof(page_content));
    content_len = 0; page_title[0] = 0;
    dom_node_count = 0;
    document = dom_create_node(DOM_DOCUMENT);
    text_run_count = 0; box_run_count = 0; link_region_count = 0;
    page_offset = 0;

    int result = sys->http_get(url, page_content, MAX_CONTENT - 1);

    if (result > 0) {
        content_len = result;
        page_title[0] = 0;

        parse_html(page_content);
        layout_dom(780, 500);

        if (!page_title[0]) sys->strcpy(page_title, "Untitled");

        int has_scripts = (sys->strstr(page_content, "<script") != 0 ||
                           sys->strstr(page_content, "<SCRIPT") != 0);
        int has_body_content = (sys->strstr(page_content, "<body") != 0 ||
                                sys->strstr(page_content, "<BODY")  != 0);

        if (has_scripts && text_run_count < 3 && has_body_content) {
            show_js_required_page(url);
        }

        cache_add(url, page_title, page_content, content_len);

        if (history_count < HISTORY_SIZE) history_count++;
        history_pos = (history_pos + 1) % HISTORY_SIZE;
        sys->strcpy(history[history_pos].url, url);
        sys->strcpy(history[history_pos].title, page_title);
        history[history_pos].timestamp = sys->get_ticks();

        sys->strcpy(tabs[current_tab].url,   url);
        sys->strcpy(tabs[current_tab].title, page_title);

        status[0] = 0; is_loading = 0;
        return 0;
    } else {
        text_run_count = 0; box_run_count = 0;
        struct { const char* text; int y; uint32_t col; int sz; } errs[] = {
            { "Failed to load page", 30, 0xFFCC0000, 20 },
            { url, 65, 0xFF666666, 12 },
            { "Possible causes:", 100, 0xFF333333, 14 },
            { "  - DNS resolution failed", 122, 0xFF555555, 13 },
            { "  - TCP connection refused", 142, 0xFF555555, 13 },
            { "  - TLS handshake failed (HTTPS not supported yet)", 162, 0xFF555555, 13 },
            { "  - Server returned an error", 182, 0xFF555555, 13 },
            { 0, 0, 0, 0 }
        };
        for (int i = 0; errs[i].text && text_run_count < MAX_TEXT_RUNS; i++) {
            text_run_t* run = &text_runs[text_run_count++];
            sys->strncpy(run->text, errs[i].text, 255);
            run->text[255] = 0;
            run->x = 20; run->y = errs[i].y;
            run->style = default_style;
            run->style.fg_color  = errs[i].col;
            run->style.font_size = errs[i].sz;
            run->is_link = 0;
            run->width = sys->strlen(run->text) * 8;
            run->line_height = errs[i].sz + 4;
        }
        sys->strcpy(page_title, "Error");
        sys->strcpy(status, "Error");
        is_loading = 0;
        return -1;
    }
}

// ============================================================================
// URL ENCODE / IS_URL / NAVIGATE
// ============================================================================

static void url_encode(const char* src, char* dst, int max_len) {
    int i = 0, j = 0;
    while (src[i] && j < max_len - 4) {
        char c = src[i];
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')
            dst[j++] = c;
        else if (c == ' ') dst[j++] = '+';
        else { dst[j++]='%'; const char* h="0123456789ABCDEF"; dst[j++]=h[(c>>4)&0xF]; dst[j++]=h[c&0xF]; }
        i++;
    }
    dst[j] = 0;
}

static int is_url(const char* input) {
    if (sys->strstr(input, "://"))  return 1;
    if (sys->strstr(input, "www.")) return 1;
    if (sys->strstr(input, ".com")) return 1;
    if (sys->strstr(input, ".org")) return 1;
    if (sys->strstr(input, ".net")) return 1;
    if (sys->strstr(input, ".edu")) return 1;
    if (sys->strstr(input, ".gov")) return 1;
    if (sys->strstr(input, ".io"))  return 1;
    return 0;
}

static void navigate(const char* input) {
    if (input[0] == 0) return;
    char url[MAX_URL];
    if (is_url(input)) {
        if (sys->strstr(input, "://")) sys->strcpy(url, input);
        else { sys->strcpy(url, "http://"); str_cat(url, input); }
    } else {
        char encoded[256];
        url_encode(input, encoded, sizeof(encoded));
        sys->strcpy(url, SEARCH_URL);
        str_cat(url, encoded);
    }
    url_cursor_pos = sys->strlen(url);
    fetch_url(url);
}

void nav_back() {
    if (history_pos > 0) { history_pos--; sys->strcpy(current_url, history[history_pos].url); fetch_url(current_url); }
}
void nav_forward() {
    if (history_pos < history_count - 1) { history_pos++; sys->strcpy(current_url, history[history_pos].url); fetch_url(current_url); }
}
void nav_home()  { sys->strcpy(current_url, DEFAULT_HOME); fetch_url(current_url); }

void new_tab() {
    if (tab_count < MAX_TABS) {
        tab_count++; current_tab = tab_count - 1;
        tabs[current_tab].url[0] = 0; tabs[current_tab].title[0] = 0;
        tabs[current_tab].active = 1; tabs[current_tab].page_offset = 0;
        current_url[0] = 0; page_title[0] = 0;
        text_run_count = 0; box_run_count = 0; link_region_count = 0;
        sys->strcpy(status, "Enter URL or search");
    }
}

void switch_tab(int index) {
    if (index < 0 || index >= tab_count) return;
    tabs[current_tab].page_offset = page_offset;
    current_tab = index;
    sys->strcpy(current_url, tabs[index].url);
    sys->strcpy(page_title,  tabs[index].title);
    page_offset = tabs[index].page_offset;
    text_run_count = 0; box_run_count = 0; link_region_count = 0;
    if (tabs[index].url[0]) {
        page_cache_t* cached = cache_find(tabs[index].url);
        if (cached) { cache_restore(cached); dom_node_count=0; document=dom_create_node(DOM_DOCUMENT); parse_html(page_content); layout_dom(780,500); }
        else fetch_url(tabs[index].url);
    }
}

void open_url_new_tab(const char* url) { new_tab(); sys->strcpy(current_url, url); fetch_url(url); }

// ============================================================================
// CURSOR MANAGEMENT
// ============================================================================

static void cursor_move_left()  { if (url_cursor_pos > 0) { url_cursor_pos--; url_cursor_blink=0; } }
static void cursor_move_right() { int l=sys->strlen(current_url); if(url_cursor_pos<l){url_cursor_pos++;url_cursor_blink=0;} }
static void cursor_move_home()  { url_cursor_pos=0; url_cursor_blink=0; }
static void cursor_move_end()   { url_cursor_pos=sys->strlen(current_url); url_cursor_blink=0; }

static void cursor_backspace() {
    int len = sys->strlen(current_url);
    if (url_cursor_pos > 0 && len > 0) {
        for (int i = url_cursor_pos - 1; i < len; i++) current_url[i] = current_url[i+1];
        url_cursor_pos--; url_cursor_blink = 0;
    }
}
static void cursor_delete() {
    int len = sys->strlen(current_url);
    if (url_cursor_pos < len) {
        for (int i = url_cursor_pos; i < len; i++) current_url[i] = current_url[i+1];
        url_cursor_blink = 0;
    }
}
static void cursor_insert_char(char c) {
    int len = sys->strlen(current_url);
    if (len < MAX_URL - 1) {
        for (int i = len; i > url_cursor_pos; i--) current_url[i] = current_url[i-1];
        current_url[url_cursor_pos] = c; url_cursor_pos++;
        current_url[len+1] = 0; url_cursor_blink = 0;
    }
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

void on_input(int key) {
    switch (key) {
        case '\n':  navigate(current_url); return;
        case '\b':  cursor_backspace(); return;
        case 0x25:  cursor_move_left(); return;
        case 0x27:  cursor_move_right(); return;
        case 0x24:  cursor_move_home(); return;
        case 0x23:  cursor_move_end(); return;
        case 0x26:  page_offset -= 5; if (page_offset < 0) page_offset = 0; return;
        case 0x28:  page_offset += 5; return;
        case 0x17:  // Ctrl+W close tab
            if (tab_count > 1) {
                for (int i = current_tab; i < tab_count - 1; i++) tabs[i] = tabs[i+1];
                tab_count--;
                if (current_tab >= tab_count) current_tab = tab_count - 1;
                switch_tab(current_tab);
            }
            return;
        case 0x14:  new_tab(); return;
        case 0x19:  search_mode = !search_mode; return;
        default:
            if (key >= 32 && key <= 126) cursor_insert_char((char)key);
            return;
    }
}

// ============================================================================
// PAINTING
// ============================================================================

static void draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    if (radius <= 0) { sys->draw_rect(x, y, w, h, color); return; }
    sys->draw_rect(x + radius, y, w - radius * 2, h, color);
    sys->draw_rect(x, y + radius, w, h - radius * 2, color);
    sys->draw_rect(x, y, radius, radius, color);
    sys->draw_rect(x + w - radius, y, radius, radius, color);
    sys->draw_rect(x, y + h - radius, radius, radius, color);
    sys->draw_rect(x + w - radius, y + h - radius, radius, radius, color);
}

static void draw_border(int x, int y, int w, int h, int bw, int bstyle, uint32_t bc, int br) {
    if (bstyle == 0 || bw <= 0) return;
    sys->draw_rect(x, y, w, bw, bc);
    sys->draw_rect(x, y + h - bw, w, bw, bc);
    sys->draw_rect(x, y + bw, bw, h - bw * 2, bc);
    sys->draw_rect(x + w - bw, y + bw, bw, h - bw * 2, bc);
}

void on_paint(int x, int y, int w, int h) {
    if (!sys) return;
    sys->draw_rect(x, y, w, h, 0xFFFFFFFF);

    // Tab bar
    int tab_bar_height = 28;
    sys->draw_rect(x, y, w, tab_bar_height, 0xFFE0E0E0);
    int tab_width = 120;
    int tab_x = x;
    for (int i = 0; i < tab_count; i++) {
        uint32_t tab_bg = (i == current_tab) ? 0xFFFFFFFF : 0xFFD0D0D0;
        sys->draw_rect(tab_x, y + 2, tab_width - 2, tab_bar_height - 4, tab_bg);
        char tab_title[20];
        int title_len = sys->strlen(tabs[i].title);
        int copy_len  = title_len < 16 ? title_len : 16;
        sys->strncpy(tab_title, tabs[i].title, copy_len);
        tab_title[copy_len] = 0;
        if (title_len > 16) { tab_title[14]='.'; tab_title[15]='.'; }
        sys->draw_text(tab_x + 8, y + 10, tab_title, 0xFF000000);
        if (tab_count > 1) sys->draw_text(tab_x + tab_width - 16, y + 10, "x", 0xFF666666);
        tab_x += tab_width;
    }
    sys->draw_text(tab_x + 4, y + 10, "+", 0xFF666666);

    // Toolbar
    int toolbar_y = y + tab_bar_height;
    sys->draw_rect(x, toolbar_y, w, 36, 0xFFF5F5F5);
    sys->draw_rect(x, toolbar_y + 36, w, 1, 0xFFCCCCCC);

    sys->draw_rect_rounded(x + 10, toolbar_y + 6, 24, 24, 0xFFFFFFFF, 4);
    sys->draw_text(x + 16, toolbar_y + 14, "<", 0xFF000000);
    sys->draw_rect_rounded(x + 40, toolbar_y + 6, 24, 24, 0xFFFFFFFF, 4);
    sys->draw_text(x + 46, toolbar_y + 14, ">", 0xFF000000);
    sys->draw_rect_rounded(x + 70, toolbar_y + 6, 24, 24, 0xFFFFFFFF, 4);
    sys->draw_text(x + 76, toolbar_y + 14, "H", 0xFF000000);

    sys->draw_rect_rounded(x + 100, toolbar_y + 6, w - 170, 24, 0xFFFFFFFF, 4);
    sys->draw_rect(x + 100, toolbar_y + 6, w - 170, 24, 0xFFCCCCCC);
    if (current_url[0] == 0) {
        sys->draw_text(x + 105, toolbar_y + 14, "Search Google or enter URL", 0xFF888888);
    } else {
        sys->draw_text(x + 105, toolbar_y + 14, current_url, 0xFF000000);
        url_cursor_blink = (url_cursor_blink + 1) % 30;
        if (url_cursor_blink < 15) {
            int cursor_x = x + 105 + url_cursor_pos * 8;
            sys->draw_rect(cursor_x, toolbar_y + 10, 2, 16, 0xFF000000);
        }
    }

    sys->draw_rect_rounded(x + w - 60, toolbar_y + 6, 24, 24, 0xFFFFFFFF, 4);
    sys->draw_text(x + w - 54, toolbar_y + 14, "R", 0xFF000000);

    if (status[0]) sys->draw_text(x + 10, y + h - 20, status, 0xFF888888);

    // Content area
    int content_y = toolbar_y + 40;
    int content_h = h - content_y - y - 20;
    int content_w = w - 20;

    sys->draw_rect(x, content_y, w, content_h, 0xFFFFFFFF);
    sys->draw_rect(x, content_y, w, 1, 0xFFE0E0E0);

    int line_height  = 16;
    int scroll_offset = page_offset * line_height;

    // Pass 1: box backgrounds and borders
    for (int i = 0; i < box_run_count; i++) {
        box_run_t* box = &box_runs[i];
        int draw_y = content_y + box->y - scroll_offset;
        int draw_x = x + 10 + box->x;
        if (draw_y + box->height < content_y || draw_y > content_y + content_h) continue;
        if (box->has_background) {
            if (box->border_radius > 0)
                draw_rounded_rect(draw_x, draw_y, box->width, box->height, box->border_radius, box->bg_color);
            else
                sys->draw_rect(draw_x, draw_y, box->width, box->height, box->bg_color);
        }
        if (box->has_border)
            draw_border(draw_x, draw_y, box->width, box->height, box->border_width, box->border_style, box->border_color, box->border_radius);
    }

    // Pass 2: text runs
    for (int i = 0; i < text_run_count; i++) {
        text_run_t* run = &text_runs[i];
        int draw_y = content_y + run->y - scroll_offset;
        int draw_x = x + 10 + run->x;

        if (draw_y < content_y - line_height || draw_y > content_y + content_h) continue;

        int aligned_x = draw_x;
        if (run->style.text_align == 1) aligned_x = x + (content_w - run->width) / 2;
        else if (run->style.text_align == 2) aligned_x = x + content_w - run->width - 20;
        if (aligned_x < x + 10) aligned_x = x + 10;

        int max_chars = (content_w - (aligned_x - x - 10)) / 8;
        if (max_chars < 0) max_chars = 0;
        if (max_chars > 255) max_chars = 255;

        char display_text[256];
        int text_len = sys->strlen(run->text);
        if (text_len > max_chars) {
            sys->strncpy(display_text, run->text, max_chars);
            display_text[max_chars] = 0;
        } else {
            sys->strcpy(display_text, run->text);
        }

        sys->draw_text(aligned_x, draw_y, display_text, run->style.fg_color);

        if (run->is_link) {
            int tw = sys->strlen(display_text) * 8;
            sys->draw_rect(aligned_x, draw_y + line_height - 2, tw, 1, run->style.fg_color);
        }
        if (run->style.font_weight == 700)
            sys->draw_text(aligned_x + 1, draw_y, display_text, run->style.fg_color);
    }

    // Scroll indicator
    if (text_run_count > 0) {
        int total_h = 0;
        for (int i = 0; i < text_run_count; i++)
            if (text_runs[i].y > total_h) total_h = text_runs[i].y;
        total_h += line_height;
        if (total_h > content_h) {
            int sp = page_offset * content_h / (total_h / line_height);
            int sh = content_h * content_h / total_h;
            if (sh < 20) sh = 20;
            sys->draw_rect(x + w - 10, content_y + sp, 8, sh, 0xFFAAAAAA);
        }
    }
}

// ============================================================================
// MOUSE HANDLING
// ============================================================================

void on_mouse(int mx, int my, int btn) {
    int tab_bar_height = 28;
    int toolbar_y = tab_bar_height;

    // Tab bar clicks
    if (my < tab_bar_height && btn == 1) {
        int tab_width = 120;
        int tab_index = mx / tab_width;
        if (mx >= tab_count * tab_width && mx < tab_count * tab_width + 24) { new_tab(); return; }
        if (tab_index < tab_count) {
            if (mx > (tab_index + 1) * tab_width - 16 && tab_count > 1) {
                for (int i = tab_index; i < tab_count - 1; i++) tabs[i] = tabs[i+1];
                tab_count--;
                if (current_tab >= tab_count) current_tab = tab_count - 1;
                switch_tab(current_tab);
            } else { switch_tab(tab_index); }
            return;
        }
    }

    // Toolbar clicks
    if (my >= toolbar_y && my < toolbar_y + 36 && btn == 1) {
        if (mx >= 10 && mx <= 34) nav_back();
        else if (mx >= 40 && mx <= 64) nav_forward();
        else if (mx >= 70 && mx <= 94) nav_home();
        else if (mx >= 500 && mx <= 524) fetch_url(current_url);
        return;
    }

    // Link clicks
    if (btn == 1) {
        int content_y    = toolbar_y + 40;
        int scroll_offset = page_offset * 16;
        int content_margin = 10;

        for (int i = 0; i < link_region_count; i++) {
            link_region_t* lr = &link_regions[i];
            int link_y = content_y + lr->y - scroll_offset;
            int link_x = content_margin + lr->x;

            if (my >= link_y && my <= link_y + lr->height &&
                mx >= link_x && mx <= link_x + lr->width) {

                char resolved[MAX_URL], final_url[MAX_URL];
                resolve_url(current_url, lr->url, resolved, MAX_URL);
                extract_google_redirect(resolved, final_url, MAX_URL);

                if (lr->target_blank) open_url_new_tab(final_url);
                else { sys->strcpy(current_url, final_url); fetch_url(current_url); }
                return;
            }
        }
    }
}

// ============================================================================
// MAIN ENTRY
// ============================================================================

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;

    sys->strcpy(current_url, "");
    status[0] = 0; page_title[0] = 0;

    for (int i = 0; i < CACHE_SIZE; i++) page_cache[i].valid = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        tabs[i].url[0] = 0; tabs[i].title[0] = 0;
        tabs[i].active = 0; tabs[i].page_offset = 0;
    }
    tabs[0].active = 1;
    sys->strcpy(tabs[0].title, "New Tab");

    document = dom_create_node(DOM_DOCUMENT);

    void* win = sys->create_window("Web Browser", 800, 600, on_paint, on_input, on_mouse);

    static menu_def_t menus[3];
    sys->strcpy(menus[0].name, "File");
    menus[0].item_count = 4;
    sys->strcpy(menus[0].items[0].label, "New Tab");
    sys->strcpy(menus[0].items[1].label, "New Window");
    sys->strcpy(menus[0].items[2].label, "Open URL...");
    sys->strcpy(menus[0].items[3].label, "Close Tab");

    sys->strcpy(menus[1].name, "Edit");
    menus[1].item_count = 3;
    sys->strcpy(menus[1].items[0].label, "Copy");
    sys->strcpy(menus[1].items[1].label, "Paste");
    sys->strcpy(menus[1].items[2].label, "Select All");

    sys->strcpy(menus[2].name, "View");
    menus[2].item_count = 3;
    sys->strcpy(menus[2].items[0].label, "Reload");
    sys->strcpy(menus[2].items[1].label, "View Source");
    sys->strcpy(menus[2].items[2].label, "Full Screen");

    sys->set_window_menu(win, menus, 3, 0);
    return 0;
}
