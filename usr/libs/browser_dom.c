// usr/libs/browser_dom.c - CamelOS Browser DOM Engine Implementation
// Version 1.0 - HTML parsing, CSS styling, layout, and rendering
// Kernel-mode code: no stdlib, no malloc, uses kmalloc/kfree and kernel string API

#include "browser_dom.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"

// Debug output via serial
extern void s_printf(const char *str);

// ============================================================================
// INTERNAL HELPERS - MINI STRING UTILITIES
// ============================================================================

// Case-insensitive character comparison
static int char_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

// Case-insensitive string comparison
static int str_casecmp(const char *a, const char *b) {
    if (!a || !b) return (a != b) ? -1 : 0;
    while (*a && *b) {
        int ca = char_tolower((unsigned char)*a);
        int cb = char_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

// Case-insensitive string prefix comparison
static int str_casencmp(const char *a, const char *b, size_t n) {
    if (!a || !b || n == 0) return 0;
    while (n > 0 && *a && *b) {
        int ca = char_tolower((unsigned char)*a);
        int cb = char_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

// Check if character is whitespace
static int is_ws(char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

// Case-insensitive substring search (like strcasestr)
static const char* str_casestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    int nlen = strlen(needle);
    if (nlen == 0) return haystack;
    const char *p = haystack;
    while (*p) {
        if (str_casencmp(p, needle, nlen) == 0) return p;
        p++;
    }
    return NULL;
}

// Parse a hex digit character to its integer value
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode HTML entities in-place in a text buffer
// Handles: &amp; &lt; &gt; &nbsp; &quot; &#39; &#NNN; &#xHHH;
static void decode_html_entities(char *text) {
    if (!text) return;
    char *dst = text;
    char *src = text;
    while (*src) {
        if (*src == '&') {
            // Try named entities
            if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&'; src += 5; continue;
            }
            if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<'; src += 4; continue;
            }
            if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>'; src += 4; continue;
            }
            if (strncmp(src, "&nbsp;", 6) == 0) {
                *dst++ = ' '; src += 6; continue;
            }
            if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '"'; src += 6; continue;
            }
            if (strncmp(src, "&#39;", 5) == 0) {
                *dst++ = '\''; src += 5; continue;
            }
            if (strncmp(src, "&apos;", 6) == 0) {
                *dst++ = '\''; src += 6; continue;
            }
            // Numeric character reference: &#NNN;
            if (src[1] == '#' && src[2] >= '0' && src[2] <= '9') {
                int val = 0;
                const char *np = src + 2;
                while (*np >= '0' && *np <= '9') {
                    val = val * 10 + (*np - '0');
                    np++;
                }
                if (*np == ';') np++;
                if (val > 0 && val < 0x10000) {
                    // Simple: only handle ASCII range for now
                    if (val < 128) {
                        *dst++ = (char)val;
                    } else {
                        *dst++ = '?'; // placeholder for non-ASCII
                    }
                    src = (char *)np;
                    continue;
                }
            }
            // Hex character reference: &#xHHH;
            if (src[1] == '#' && (src[2] == 'x' || src[2] == 'X')) {
                int val = 0;
                const char *np = src + 3;
                while (1) {
                    int d = hex_digit(*np);
                    if (d < 0) break;
                    val = val * 16 + d;
                    np++;
                }
                if (*np == ';') np++;
                if (val > 0 && val < 0x10000) {
                    if (val < 128) {
                        *dst++ = (char)val;
                    } else {
                        *dst++ = '?';
                    }
                    src = (char *)np;
                    continue;
                }
            }
            // Not a known entity, copy as-is
            *dst++ = *src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Skip whitespace, return pointer to next non-ws character
static const char* skip_ws(const char *p) {
    if (!p) return NULL;
    while (*p && is_ws(*p)) p++;
    return p;
}

// Safe string copy with truncation
static void safe_strcpy(char *dst, const char *src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = 0;
    while (i < max_len - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// Safe string cat with truncation
static void __attribute__((unused)) safe_strcat(char *dst, const char *src, int max_total) {
    if (!dst || !src || max_total <= 0) return;
    int dlen = strlen(dst);
    int remaining = max_total - dlen - 1;
    if (remaining <= 0) return;
    int i = 0;
    while (i < remaining && src[i]) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

// Parse an integer from a string, return 0 on success, -1 on failure
static int parse_int(const char *s, int *out) {
    if (!s || !*s) return -1;
    int neg = 0;
    int val = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    *out = neg ? -val : val;
    return 0;
}

// ============================================================================
// NAMED COLOR TABLE
// ============================================================================

typedef struct {
    const char *name;
    uint32_t color;  // ARGB
} named_color_t;

static const named_color_t named_colors[] = {
    {"white",       0xFFFFFFFF},
    {"black",       0xFF000000},
    {"red",         0xFFFF0000},
    {"green",       0xFF008000},
    {"blue",        0xFF0000FF},
    {"yellow",      0xFFFFFF00},
    {"cyan",        0xFF00FFFF},
    {"magenta",     0xFFFF00FF},
    {"orange",      0xFFFFA500},
    {"purple",      0xFF800080},
    {"pink",        0xFFFFC0CB},
    {"gray",        0xFF808080},
    {"grey",        0xFF808080},
    {"silver",      0xFFC0C0C0},
    {"maroon",      0xFF800000},
    {"olive",       0xFF808000},
    {"lime",        0xFF00FF00},
    {"aqua",        0xFF00FFFF},
    {"teal",        0xFF008080},
    {"navy",        0xFF000080},
    {"fuchsia",     0xFFFF00FF},
    {"coral",       0xFFFF7F50},
    {"salmon",      0xFFFA8072},
    {"lightgray",   0xFFD3D3D3},
    {"lightgrey",   0xFFD3D3D3},
    {"darkgray",    0xFFA9A9A9},
    {"darkgrey",    0xFFA9A9A9},
    {"transparent", 0x00000000},
    {NULL, 0}
};

// ============================================================================
// COLOR PARSING
// ============================================================================

// Parse a hex color component (2 hex digits) to 0-255
static int hex_byte(const char *s) {
    int hi = hex_digit(s[0]);
    int lo = hex_digit(s[1]);
    if (hi < 0 || lo < 0) return 0;
    return (hi << 4) | lo;
}

int dom_parse_color(const char *str, uint32_t *out_color) {
    if (!str || !out_color) return -1;

    // Trim leading whitespace
    while (is_ws(*str)) str++;
    if (!*str) return -1;

    // Hex format: #RGB or #RRGGBB
    if (*str == '#') {
        str++;
        int len = strlen(str);
        // Trim trailing whitespace
        while (len > 0 && is_ws(str[len - 1])) len--;

        if (len == 3) {
            // #RGB -> #RRGGBB
            int r = hex_digit(str[0]);
            int g = hex_digit(str[1]);
            int b = hex_digit(str[2]);
            if (r < 0 || g < 0 || b < 0) return -1;
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
            *out_color = DOM_COLOR(r, g, b);
            return 0;
        } else if (len == 6) {
            int r = hex_byte(str);
            int g = hex_byte(str + 2);
            int b = hex_byte(str + 4);
            *out_color = DOM_COLOR(r, g, b);
            return 0;
        }
        return -1;
    }

    // rgb(r, g, b) format
    if (str_casencmp(str, "rgb(", 4) == 0) {
        const char *p = str + 4;
        int r = 0, g = 0, b = 0;
        // Skip whitespace
        while (is_ws(*p)) p++;
        // Parse r
        if (parse_int(p, &r) != 0) return -1;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++; // skip comma
        while (is_ws(*p)) p++;
        // Parse g
        if (parse_int(p, &g) != 0) return -1;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++; // skip comma
        while (is_ws(*p)) p++;
        // Parse b
        if (parse_int(p, &b) != 0) return -1;
        // Clamp to 0-255
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *out_color = DOM_COLOR((uint8_t)r, (uint8_t)g, (uint8_t)b);
        return 0;
    }

    // rgba(r, g, b, a) format
    if (str_casencmp(str, "rgba(", 5) == 0) {
        const char *p = str + 5;
        int r = 0, g = 0, b = 0, a = 255;
        while (is_ws(*p)) p++;
        if (parse_int(p, &r) != 0) return -1;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++;
        while (is_ws(*p)) p++;
        if (parse_int(p, &g) != 0) return -1;
        while (*p && *p != ',') p++;
        if (!*p) return -1;
        p++;
        while (is_ws(*p)) p++;
        if (parse_int(p, &b) != 0) return -1;
        while (*p && *p != ',') p++;
        if (*p) {
            p++;
            while (is_ws(*p)) p++;
            if (parse_int(p, &a) != 0) a = 255;
        }
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        *out_color = DOM_COLOR_A((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        return 0;
    }

    // Named color lookup
    for (int i = 0; named_colors[i].name != NULL; i++) {
        if (str_casecmp(str, named_colors[i].name) == 0) {
            *out_color = named_colors[i].color;
            return 0;
        }
    }

    return -1;
}

// ============================================================================
// STYLE INITIALIZATION
// ============================================================================

void dom_style_init_defaults(dom_style_t *style) {
    if (!style) return;
    memset(style, 0, sizeof(dom_style_t));

    style->color              = DOM_COLOR_BLACK;
    style->background_color   = DOM_COLOR_TRANSPARENT;
    style->font_size          = 16;
    style->font_weight        = DOM_FONT_WEIGHT_NORMAL;
    style->font_style         = DOM_FONT_STYLE_NORMAL;
    style->text_align         = DOM_TEXT_ALIGN_LEFT;
    style->text_decoration    = DOM_TEXT_DECOR_NONE;
    style->line_height        = 0;  // 0 = auto (1.2x font_size)
    style->letter_spacing     = 0;
    style->word_spacing       = 0;
    style->text_transform     = DOM_TEXT_TRANSFORM_NONE;
    style->font_family_monospace = 0;
    style->display            = DOM_DISPLAY_BLOCK;
    style->position           = DOM_POSITION_STATIC;
    style->overflow           = DOM_OVERFLOW_VISIBLE;
    style->vertical_align     = DOM_VALIGN_BASELINE;
    style->white_space        = DOM_WHITESPACE_NORMAL;
    style->list_style_type    = DOM_LIST_STYLE_DISC;
    style->opacity            = 255;  // Fully opaque
    style->visible            = 1;    // Visible

    // Margins and padding default to 0
    style->margin[0]  = 0; style->margin[1]  = 0;
    style->margin[2]  = 0; style->margin[3]  = 0;
    style->padding[0] = 0; style->padding[1] = 0;
    style->padding[2] = 0; style->padding[3] = 0;

    // Auto sizing
    style->width      = -1;
    style->height     = -1;
    style->width_pct  = -1;
    style->height_pct = -1;

    // Min/max sizing (unset)
    style->min_width   = -1;
    style->max_width   = -1;
    style->min_height  = -1;
    style->max_height  = -1;

    // No borders
    for (int i = 0; i < 4; i++) {
        style->border[i].width = 0;
        style->border[i].style = DOM_BORDER_STYLE_NONE;
        style->border[i].color = DOM_COLOR_BLACK;
    }

    // Border radius
    style->border_radius = 0;

    // Position offsets
    style->top    = 0;
    style->left   = 0;
    style->right  = -1;
    style->bottom = -1;
    style->z_index = 0;

    // Layout positions (unset)
    style->layout_x  = 0;
    style->layout_y  = 0;
    style->layout_w  = 0;
    style->layout_h  = 0;
    style->content_x = 0;
    style->content_y = 0;
    style->content_w = 0;
    style->content_h = 0;
}

// ============================================================================
// DEFAULT DISPLAY TYPES FOR HTML ELEMENTS
// ============================================================================

static dom_display_t default_display_for_tag(const char *tag) {
    if (!tag || !*tag) return DOM_DISPLAY_INLINE;

    // Block-level elements
    if (str_casecmp(tag, "div") == 0)       return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "p") == 0)         return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h1") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h2") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h3") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h4") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h5") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "h6") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "ul") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "ol") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "li") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "table") == 0)     return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "tr") == 0)        return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "section") == 0)   return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "article") == 0)   return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "header") == 0)    return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "footer") == 0)    return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "nav") == 0)       return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "main") == 0)      return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "aside") == 0)     return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "form") == 0)      return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "blockquote") == 0) return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "pre") == 0)       return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "hr") == 0)        return DOM_DISPLAY_BLOCK;

    // Inline elements
    if (str_casecmp(tag, "span") == 0)      return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "a") == 0)         return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "strong") == 0)    return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "em") == 0)        return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "b") == 0)         return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "i") == 0)         return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "u") == 0)         return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "code") == 0)      return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "br") == 0)        return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "img") == 0)       return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "input") == 0)     return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "label") == 0)     return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "button") == 0)    return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "sub") == 0)       return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "sup") == 0)       return DOM_DISPLAY_INLINE;
    if (str_casecmp(tag, "small") == 0)     return DOM_DISPLAY_INLINE;

    // Default: block for html/body
    if (str_casecmp(tag, "html") == 0)      return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "body") == 0)      return DOM_DISPLAY_BLOCK;
    if (str_casecmp(tag, "head") == 0)      return DOM_DISPLAY_NONE;

    // Default: block for unknown structural elements
    return DOM_DISPLAY_BLOCK;
}

// Default font size for heading elements
static int default_font_size_for_tag(const char *tag) {
    if (!tag) return 16;
    if (str_casecmp(tag, "h1") == 0) return 32;
    if (str_casecmp(tag, "h2") == 0) return 24;
    if (str_casecmp(tag, "h3") == 0) return 20;
    if (str_casecmp(tag, "h4") == 0) return 18;
    if (str_casecmp(tag, "h5") == 0) return 16;
    if (str_casecmp(tag, "h6") == 0) return 14;
    return 16;
}

static dom_font_weight_t default_font_weight_for_tag(const char *tag) {
    if (!tag) return DOM_FONT_WEIGHT_NORMAL;
    if (str_casecmp(tag, "h1") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "h2") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "h3") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "h4") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "h5") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "h6") == 0) return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "b") == 0)  return DOM_FONT_WEIGHT_BOLD;
    if (str_casecmp(tag, "strong") == 0) return DOM_FONT_WEIGHT_BOLD;
    return DOM_FONT_WEIGHT_NORMAL;
}

// Default margins for specific elements
static void apply_default_element_styles(dom_node_t *node) {
    if (!node || node->type != DOM_NODE_ELEMENT) return;
    const char *tag = node->tag;

    // Headings get top/bottom margin
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
        node->computed_style.margin[0] = 12; // top
        node->computed_style.margin[2] = 6;  // bottom
    }
    // Paragraph gets margin
    else if (str_casecmp(tag, "p") == 0) {
        node->computed_style.margin[0] = 8;
        node->computed_style.margin[2] = 8;
    }
    // Lists get margin and padding
    else if (str_casecmp(tag, "ul") == 0) {
        node->computed_style.margin[0] = 8;
        node->computed_style.margin[2] = 8;
        node->computed_style.padding[3] = 24; // left padding for bullets
        node->computed_style.list_style_type = DOM_LIST_STYLE_DISC;
    }
    else if (str_casecmp(tag, "ol") == 0) {
        node->computed_style.margin[0] = 8;
        node->computed_style.margin[2] = 8;
        node->computed_style.padding[3] = 24; // left padding for bullets
        node->computed_style.list_style_type = DOM_LIST_STYLE_DECIMAL;
    }
    // Body has default margin
    else if (str_casecmp(tag, "body") == 0) {
        node->computed_style.margin[0] = 8;
        node->computed_style.margin[1] = 8;
        node->computed_style.margin[2] = 8;
        node->computed_style.margin[3] = 8;
    }
    // Links get blue color and underline by default
    else if (str_casecmp(tag, "a") == 0) {
        node->computed_style.color = 0xFF0000FF; // blue
        node->computed_style.text_decoration = DOM_TEXT_DECOR_UNDERLINE;
    }
    // HR gets top/bottom margin
    else if (str_casecmp(tag, "hr") == 0) {
        node->computed_style.margin[0] = 8;
        node->computed_style.margin[2] = 8;
        node->computed_style.border[0].width = 1;
        node->computed_style.border[0].style = DOM_BORDER_STYLE_SOLID;
        node->computed_style.border[0].color = DOM_COLOR_GRAY;
    }
    // Italic elements
    else if (str_casecmp(tag, "i") == 0 || str_casecmp(tag, "em") == 0) {
        node->computed_style.font_style = DOM_FONT_STYLE_ITALIC;
    }
    // Strikethrough elements
    else if (str_casecmp(tag, "s") == 0 || str_casecmp(tag, "del") == 0 ||
             str_casecmp(tag, "strike") == 0) {
        node->computed_style.text_decoration = DOM_TEXT_DECOR_LINE_THROUGH;
    }
    // Underline element
    else if (str_casecmp(tag, "u") == 0 || str_casecmp(tag, "ins") == 0) {
        node->computed_style.text_decoration = DOM_TEXT_DECOR_UNDERLINE;
    }
    // Monospace elements
    else if (str_casecmp(tag, "code") == 0 || str_casecmp(tag, "pre") == 0 ||
             str_casecmp(tag, "tt") == 0 || str_casecmp(tag, "kbd") == 0 ||
             str_casecmp(tag, "samp") == 0) {
        node->computed_style.font_family_monospace = 1;
    }
    // Pre preserves whitespace
    else if (str_casecmp(tag, "pre") == 0) {
        node->computed_style.white_space = DOM_WHITESPACE_PRE;
    }
    // Superscript/subscript
    else if (str_casecmp(tag, "sup") == 0) {
        node->computed_style.font_size = 12;
        node->computed_style.vertical_align = DOM_VALIGN_TOP;
    }
    else if (str_casecmp(tag, "sub") == 0) {
        node->computed_style.font_size = 12;
        node->computed_style.vertical_align = DOM_VALIGN_BOTTOM;
    }
    // Small text
    else if (str_casecmp(tag, "small") == 0) {
        node->computed_style.font_size = 13;
    }
}

// ============================================================================
// DOCUMENT LIFECYCLE
// ============================================================================

dom_document_t* dom_document_create(void) {
    dom_document_t *doc = (dom_document_t *)kmalloc(sizeof(dom_document_t));
    if (!doc) {
        s_printf("[DOM] Failed to allocate document\n");
        return NULL;
    }
    memset(doc, 0, sizeof(dom_document_t));
    doc->root = NULL;
    doc->head = NULL;
    doc->body = NULL;
    doc->css_rule_count = 0;
    doc->script_count = 0;
    doc->stylesheet_count = 0;
    doc->node_count = 0;
    doc->viewport_w = 800;
    doc->viewport_h = 600;
    doc->total_height = 0;
    doc->base_url[0] = '\0';
    return doc;
}

void dom_document_destroy(dom_document_t *doc) {
    if (!doc) return;
    // All nodes are in the static pool inside doc, no separate free needed.
    // Just free the document struct itself.
    kfree(doc);
}

// ============================================================================
// NODE OPERATIONS
// ============================================================================

dom_node_t* dom_node_alloc(dom_document_t *doc) {
    if (!doc) return NULL;
    if (doc->node_count >= DOM_MAX_NODES) {
        s_printf("[DOM] Node pool exhausted\n");
        return NULL;
    }
    // Find a free slot (in_use == 0)
    for (int i = 0; i < DOM_MAX_NODES; i++) {
        if (!doc->node_pool[i].in_use) {
            dom_node_t *node = &doc->node_pool[i];
            memset(node, 0, sizeof(dom_node_t));
            node->in_use = 1;
            doc->node_count++;
            // Initialize computed style with defaults
            dom_style_init_defaults(&node->computed_style);
            return node;
        }
    }
    s_printf("[DOM] No free node slots found\n");
    return NULL;
}

static void __attribute__((unused)) dom_node_free(dom_document_t *doc, dom_node_t *node) {
    if (!doc || !node) return;
    // Recursively free children first
    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *next = child->next_sibling;
        dom_node_free(doc, child);
        child = next;
    }
    // Unlink from parent
    if (node->parent) {
        if (node->parent->first_child == node) {
            node->parent->first_child = node->next_sibling;
        }
        if (node->parent->last_child == node) {
            node->parent->last_child = node->prev_sibling;
        }
        if (node->prev_sibling) {
            node->prev_sibling->next_sibling = node->next_sibling;
        }
        if (node->next_sibling) {
            node->next_sibling->prev_sibling = node->prev_sibling;
        }
        node->parent->child_count--;
    }
    node->in_use = 0;
    doc->node_count--;
}

void dom_node_append_child(dom_node_t *parent, dom_node_t *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = NULL;
    child->prev_sibling = parent->last_child;

    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->child_count++;
}

int dom_node_set_attr(dom_node_t *node, const char *name, const char *value) {
    if (!node || !name || node->type != DOM_NODE_ELEMENT) return -1;
    // Check if attribute already exists, update if so
    for (int i = 0; i < node->attr_count; i++) {
        if (str_casecmp(node->attrs[i].name, name) == 0) {
            safe_strcpy(node->attrs[i].value, value ? value : "", DOM_MAX_ATTR_VALUE);
            // Update convenience fields
            if (strcmp(name, "id") == 0) {
                safe_strcpy(node->id, value ? value : "", DOM_MAX_ID_LEN);
            } else if (strcmp(name, "class") == 0) {
                safe_strcpy(node->class_name, value ? value : "", DOM_MAX_CLASS_LEN);
            } else if (strcmp(name, "style") == 0) {
                safe_strcpy(node->style, value ? value : "", DOM_MAX_STYLE_LEN);
            } else if (strcmp(name, "href") == 0) {
                safe_strcpy(node->href, value ? value : "", DOM_MAX_URL_LEN);
            } else if (strcmp(name, "src") == 0) {
                safe_strcpy(node->src, value ? value : "", DOM_MAX_URL_LEN);
            }
            return 0;
        }
    }
    // Add new attribute
    if (node->attr_count >= DOM_MAX_ATTRS) return -1;
    dom_attr_t *attr = &node->attrs[node->attr_count];
    safe_strcpy(attr->name, name, DOM_MAX_ATTR_NAME);
    safe_strcpy(attr->value, value ? value : "", DOM_MAX_ATTR_VALUE);
    node->attr_count++;
    // Update convenience fields
    if (strcmp(name, "id") == 0) {
        safe_strcpy(node->id, value ? value : "", DOM_MAX_ID_LEN);
    } else if (strcmp(name, "class") == 0) {
        safe_strcpy(node->class_name, value ? value : "", DOM_MAX_CLASS_LEN);
    } else if (strcmp(name, "style") == 0) {
        safe_strcpy(node->style, value ? value : "", DOM_MAX_STYLE_LEN);
    } else if (strcmp(name, "href") == 0) {
        safe_strcpy(node->href, value ? value : "", DOM_MAX_URL_LEN);
    } else if (strcmp(name, "src") == 0) {
        safe_strcpy(node->src, value ? value : "", DOM_MAX_URL_LEN);
    }
    return 0;
}

const char* dom_node_get_attr(dom_node_t *node, const char *name) {
    if (!node || !name || node->type != DOM_NODE_ELEMENT) return NULL;
    for (int i = 0; i < node->attr_count; i++) {
        if (str_casecmp(node->attrs[i].name, name) == 0) {
            return node->attrs[i].value;
        }
    }
    return NULL;
}

int dom_node_get_text_content(dom_node_t *node, char *buf, int buf_size) {
    if (!node || !buf || buf_size <= 0) return 0;
    int written = 0;

    if (node->type == DOM_NODE_TEXT) {
        int tlen = strlen(node->text);
        int to_write = tlen;
        if (to_write > buf_size - 1) to_write = buf_size - 1;
        memcpy(buf, node->text, to_write);
        buf[to_write] = '\0';
        return to_write;
    }

    // Element: concatenate all child text
    buf[0] = '\0';
    dom_node_t *child = node->first_child;
    while (child && written < buf_size - 1) {
        char tmp[DOM_MAX_TEXT_LEN];
        int cw = dom_node_get_text_content(child, tmp, sizeof(tmp));
        if (cw > 0) {
            int space = buf_size - written - 1;
            if (cw > space) cw = space;
            memcpy(buf + written, tmp, cw);
            written += cw;
        }
        child = child->next_sibling;
    }
    buf[written] = '\0';
    return written;
}

// ============================================================================
// HTML PARSING
// ============================================================================

// Self-closing / void elements that don't need a closing tag
static int is_void_element(const char *tag) {
    if (!tag) return 0;
    if (str_casecmp(tag, "br") == 0)      return 1;
    if (str_casecmp(tag, "hr") == 0)      return 1;
    if (str_casecmp(tag, "img") == 0)     return 1;
    if (str_casecmp(tag, "input") == 0)   return 1;
    if (str_casecmp(tag, "meta") == 0)    return 1;
    if (str_casecmp(tag, "link") == 0)    return 1;
    if (str_casecmp(tag, "area") == 0)    return 1;
    if (str_casecmp(tag, "base") == 0)    return 1;
    if (str_casecmp(tag, "col") == 0)     return 1;
    if (str_casecmp(tag, "embed") == 0)   return 1;
    if (str_casecmp(tag, "source") == 0)  return 1;
    if (str_casecmp(tag, "track") == 0)   return 1;
    if (str_casecmp(tag, "wbr") == 0)     return 1;
    return 0;
}

// Raw text elements: their content is not parsed as HTML
static int is_raw_text_element(const char *tag) {
    if (!tag) return 0;
    if (str_casecmp(tag, "style") == 0)  return 1;
    if (str_casecmp(tag, "script") == 0) return 1;
    return 0;
}

// Parse an opening tag's attributes from a string like:
//   tagname attr1="val1" attr2='val2' attr3=noquotes
static void parse_tag_attrs(dom_node_t *node, const char *attr_str, int attr_len) {
    if (!node || !attr_str || attr_len <= 0) return;

    const char *p = attr_str;
    const char *end = attr_str + attr_len;

    // Skip the tag name
    while (p < end && !is_ws(*p)) p++;
    // Skip whitespace
    while (p < end && is_ws(*p)) p++;

    while (p < end) {
        // Skip whitespace
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        // Read attribute name
        char attr_name[DOM_MAX_ATTR_NAME];
        int ni = 0;
        while (p < end && !is_ws(*p) && *p != '=' && *p != '>' && *p != '/') {
            if (ni < DOM_MAX_ATTR_NAME - 1) {
                attr_name[ni++] = char_tolower((unsigned char)*p);
            }
            p++;
        }
        attr_name[ni] = '\0';
        if (ni == 0) { p++; continue; }

        // Skip whitespace
        while (p < end && is_ws(*p)) p++;

        // Check for '='
        char attr_value[DOM_MAX_ATTR_VALUE];
        attr_value[0] = '\0';

        if (p < end && *p == '=') {
            p++; // skip '='
            // Skip whitespace
            while (p < end && is_ws(*p)) p++;

            if (p < end && (*p == '"' || *p == '\'')) {
                // Quoted value
                char quote = *p;
                p++;
                int vi = 0;
                while (p < end && *p != quote) {
                    if (vi < DOM_MAX_ATTR_VALUE - 1) {
                        attr_value[vi++] = *p;
                    }
                    p++;
                }
                attr_value[vi] = '\0';
                if (p < end && *p == quote) p++; // skip closing quote
            } else {
                // Unquoted value
                int vi = 0;
                while (p < end && !is_ws(*p) && *p != '>' && *p != '/') {
                    if (vi < DOM_MAX_ATTR_VALUE - 1) {
                        attr_value[vi++] = *p;
                    }
                    p++;
                }
                attr_value[vi] = '\0';
            }
        }

        dom_node_set_attr(node, attr_name, attr_value);
    }
}

int dom_parse_html(dom_document_t *doc, const char *html_body) {
    if (!doc || !html_body) return -1;

    // Reset node pool
    doc->node_count = 0;
    doc->script_count = 0;
    doc->stylesheet_count = 0;
    memset(doc->node_pool, 0, sizeof(doc->node_pool));

    // Create root <html> node
    dom_node_t *root = dom_node_alloc(doc);
    if (!root) return -1;
    root->type = DOM_NODE_ELEMENT;
    safe_strcpy(root->tag, "html", DOM_MAX_TAG_LEN);
    root->computed_style.display = DOM_DISPLAY_BLOCK;
    doc->root = root;

    // Stack for current parent context (max nesting depth)
    dom_node_t *stack[64];
    int stack_top = 0;
    stack[stack_top] = root;

    const char *p = html_body;
    char text_buf[DOM_MAX_TEXT_LEN];
    int text_pos = 0;

    while (*p) {
        if (*p == '<') {
            // Flush any accumulated text
            if (text_pos > 0) {
                text_buf[text_pos] = '\0';
                // Trim trailing whitespace from text
                while (text_pos > 0 && is_ws(text_buf[text_pos - 1])) {
                    text_buf[--text_pos] = '\0';
                }
                // Skip pure-whitespace text nodes
                int has_content = 0;
                for (int i = 0; i < text_pos; i++) {
                    if (!is_ws(text_buf[i])) { has_content = 1; break; }
                }
                if (has_content && stack_top >= 0) {
                    dom_node_t *text_node = dom_node_alloc(doc);
                    if (text_node) {
                        text_node->type = DOM_NODE_TEXT;
                        safe_strcpy(text_node->text, text_buf, DOM_MAX_TEXT_LEN);
                        // Decode HTML entities in the text node
                        decode_html_entities(text_node->text);
                        text_node->computed_style.display = DOM_DISPLAY_INLINE;
                        dom_node_append_child(stack[stack_top], text_node);
                    }
                }
                text_pos = 0;
            }

            p++; // skip '<'

            // Check for comment <!-- ... -->
            if (*p == '!' && *(p + 1) == '-' && *(p + 2) == '-') {
                p += 3;
                while (*p && !(*p == '-' && *(p + 1) == '-' && *(p + 2) == '>')) p++;
                if (*p) p += 3;
                continue;
            }

            // Check for DOCTYPE
            if (*p == '!' || str_casencmp(p, "?xml", 4) == 0) {
                // Skip until '>'
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }

            // Closing tag?
            if (*p == '/') {
                p++; // skip '/'
                // Read tag name
                char close_tag[DOM_MAX_TAG_LEN];
                int ci = 0;
                while (*p && !is_ws(*p) && *p != '>') {
                    if (ci < DOM_MAX_TAG_LEN - 1) {
                        close_tag[ci++] = char_tolower((unsigned char)*p);
                    }
                    p++;
                }
                close_tag[ci] = '\0';
                // Skip to '>'
                while (*p && *p != '>') p++;
                if (*p) p++;

                // Pop the stack looking for matching tag
                while (stack_top > 0) {
                    dom_node_t *current = stack[stack_top];
                    if (current->type == DOM_NODE_ELEMENT &&
                        str_casecmp(current->tag, close_tag) == 0) {
                        stack_top--;
                        break;
                    }
                    stack_top--;
                }
                continue;
            }

            // Opening tag - read tag name and attributes
            char tag_name[DOM_MAX_TAG_LEN];
            int ti = 0;
            while (*p && !is_ws(*p) && *p != '>' && *p != '/') {
                if (ti < DOM_MAX_TAG_LEN - 1) {
                    tag_name[ti++] = char_tolower((unsigned char)*p);
                }
                p++;
            }
            tag_name[ti] = '\0';

            if (ti == 0) {
                // Malformed tag, skip
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }

            // Read the rest of the tag (attributes) until '>' or '/>'
            char attr_buf[512];
            int ai = 0;
            int self_closing = 0;

            while (*p && *p != '>') {
                if (*p == '/' && (*(p + 1) == '>' || is_ws(*(p + 1)))) {
                    self_closing = 1;
                    p++;
                    break;
                }
                if (ai < (int)sizeof(attr_buf) - 1) {
                    attr_buf[ai++] = *p;
                }
                p++;
            }
            attr_buf[ai] = '\0';
            if (*p == '>') p++;

            // Skip non-visible elements (head children except style/script)
            // We still create nodes but may mark them display:none

            // Create the element node
            dom_node_t *elem = dom_node_alloc(doc);
            if (!elem) {
                s_printf("[DOM] Out of nodes while parsing\n");
                return -1;
            }
            elem->type = DOM_NODE_ELEMENT;
            safe_strcpy(elem->tag, tag_name, DOM_MAX_TAG_LEN);

            // Set default display and font styles
            elem->computed_style.display = default_display_for_tag(tag_name);
            elem->computed_style.font_size = default_font_size_for_tag(tag_name);
            elem->computed_style.font_weight = default_font_weight_for_tag(tag_name);
            apply_default_element_styles(elem);

            // Parse attributes
            if (ai > 0) {
                parse_tag_attrs(elem, attr_buf, ai);
            }

            // Handle special elements
            if (str_casecmp(tag_name, "html") == 0) {
                // Use the pre-created root instead of creating a duplicate
                // Transfer attributes to root
                for (int i = 0; i < elem->attr_count; i++) {
                    dom_node_set_attr(root, elem->attrs[i].name, elem->attrs[i].value);
                }
                // Free the duplicate node
                elem->in_use = 0;
                doc->node_count--;
                // Don't add to parent or push to stack; root is already there
                continue;
            }
            // Also handle <head> specially - mark as display:none
            if (str_casecmp(tag_name, "head") == 0) {
                elem->computed_style.display = DOM_DISPLAY_NONE;
            }
            // Handle <meta>, <link>, <title> etc in head - also display:none
            if (str_casecmp(tag_name, "meta") == 0 ||
                str_casecmp(tag_name, "link") == 0 ||
                str_casecmp(tag_name, "title") == 0) {
                elem->computed_style.display = DOM_DISPLAY_NONE;
            }

            // Add to current parent
            dom_node_append_child(stack[stack_top], elem);

            // Track head and body
            if (str_casecmp(tag_name, "head") == 0 && !doc->head) {
                doc->head = elem;
            }
            if (str_casecmp(tag_name, "body") == 0 && !doc->body) {
                doc->body = elem;
            }

            // Handle raw text elements (style, script)
            if (is_raw_text_element(tag_name)) {
                // Find closing tag using case-insensitive search
                // Build pattern: </tagname>
                char close_pattern[64];
                close_pattern[0] = '<';
                close_pattern[1] = '/';
                int cpi = 2;
                int tlen = strlen(tag_name);
                for (int i = 0; i < tlen && cpi < 60; i++) {
                    close_pattern[cpi++] = tag_name[i];
                }
                close_pattern[cpi++] = '>';
                close_pattern[cpi] = '\0';

                // Case-insensitive search for closing tag (handles </STYLE>, </Style>, etc.)
                const char *close_pos = str_casestr(p, close_pattern);
                if (close_pos) {
                    int content_len = close_pos - p;
                    // Also skip any whitespace before the '>' in the closing tag
                    // Some HTML has </style > with space before >

                    if (str_casecmp(tag_name, "style") == 0) {
                        // Store in stylesheets
                        if (doc->stylesheet_count < DOM_MAX_STYLESHEETS) {
                            int copy_len = content_len;
                            if (copy_len >= DOM_MAX_STYLESHEET_LEN) {
                                copy_len = DOM_MAX_STYLESHEET_LEN - 1;
                            }
                            memcpy(doc->stylesheets[doc->stylesheet_count], p, copy_len);
                            doc->stylesheets[doc->stylesheet_count][copy_len] = '\0';
                            doc->stylesheet_count++;
                        }
                        // Mark style elements as display:none (don't show CSS text)
                        elem->computed_style.display = DOM_DISPLAY_NONE;
                    } else if (str_casecmp(tag_name, "script") == 0) {
                        // Store in scripts
                        if (doc->script_count < DOM_MAX_SCRIPTS) {
                            int copy_len = content_len;
                            if (copy_len >= DOM_MAX_SCRIPT_LEN) {
                                copy_len = DOM_MAX_SCRIPT_LEN - 1;
                            }
                            memcpy(doc->scripts[doc->script_count], p, copy_len);
                            doc->scripts[doc->script_count][copy_len] = '\0';
                            doc->script_count++;
                        }
                        // Mark script elements as display:none (don't show JS text)
                        elem->computed_style.display = DOM_DISPLAY_NONE;
                    }

                    p = close_pos + strlen(close_pattern);
                } else {
                    // No closing tag found - consume rest as content
                    // This is more robust than skipping to EOF
                    int content_len = strlen(p);
                    if (str_casecmp(tag_name, "style") == 0) {
                        if (doc->stylesheet_count < DOM_MAX_STYLESHEETS) {
                            int copy_len = content_len;
                            if (copy_len >= DOM_MAX_STYLESHEET_LEN) {
                                copy_len = DOM_MAX_STYLESHEET_LEN - 1;
                            }
                            memcpy(doc->stylesheets[doc->stylesheet_count], p, copy_len);
                            doc->stylesheets[doc->stylesheet_count][copy_len] = '\0';
                            doc->stylesheet_count++;
                        }
                        elem->computed_style.display = DOM_DISPLAY_NONE;
                    } else if (str_casecmp(tag_name, "script") == 0) {
                        if (doc->script_count < DOM_MAX_SCRIPTS) {
                            int copy_len = content_len;
                            if (copy_len >= DOM_MAX_SCRIPT_LEN) {
                                copy_len = DOM_MAX_SCRIPT_LEN - 1;
                            }
                            memcpy(doc->scripts[doc->script_count], p, copy_len);
                            doc->scripts[doc->script_count][copy_len] = '\0';
                            doc->script_count++;
                        }
                        elem->computed_style.display = DOM_DISPLAY_NONE;
                    }
                    while (*p) p++;
                }
                // Don't push raw text elements onto the stack
                continue;
            }

            // Void elements don't push onto the stack
            if (is_void_element(tag_name) || self_closing) {
                continue;
            }

            // Push onto stack
            if (stack_top < 63) {
                stack_top++;
                stack[stack_top] = elem;
            }
        } else {
            // Regular text character
            // Collapse consecutive whitespace
            if (is_ws(*p)) {
                if (text_pos > 0 && !is_ws(text_buf[text_pos - 1])) {
                    if (text_pos < DOM_MAX_TEXT_LEN - 1) {
                        text_buf[text_pos++] = ' ';
                    }
                }
            } else {
                if (text_pos < DOM_MAX_TEXT_LEN - 1) {
                    text_buf[text_pos++] = *p;
                }
            }
            p++;
        }
    }

    // Flush remaining text
    if (text_pos > 0) {
        text_buf[text_pos] = '\0';
        while (text_pos > 0 && is_ws(text_buf[text_pos - 1])) {
            text_buf[--text_pos] = '\0';
        }
        int has_content = 0;
        for (int i = 0; i < text_pos; i++) {
            if (!is_ws(text_buf[i])) { has_content = 1; break; }
        }
        if (has_content && stack_top >= 0) {
            dom_node_t *text_node = dom_node_alloc(doc);
            if (text_node) {
                text_node->type = DOM_NODE_TEXT;
                safe_strcpy(text_node->text, text_buf, DOM_MAX_TEXT_LEN);
                // Decode HTML entities in the text node
                decode_html_entities(text_node->text);
                text_node->computed_style.display = DOM_DISPLAY_INLINE;
                dom_node_append_child(stack[stack_top], text_node);
            }
        }
    }

    // If no <body> was found, look for it in the root's children
    // or treat the root as the body
    if (!doc->body) {
        // Search children of root for a body-like element
        dom_node_t *child = doc->root->first_child;
        while (child) {
            if (child->type == DOM_NODE_ELEMENT &&
                str_casecmp(child->tag, "body") == 0) {
                doc->body = child;
                break;
            }
            child = child->next_sibling;
        }
        // If still not found, use the root itself
        if (!doc->body) {
            doc->body = doc->root;
        }
    }

    s_printf("[DOM] Parse complete: nodes=");
    // Debug: log node count
    {
        char nc[16]; int nc_i = 0; int nc_v = doc->node_count;
        if (nc_v == 0) { nc[0] = '0'; nc_i = 1; }
        else { char tmp[16]; int ti = 0;
            while (nc_v > 0) { tmp[ti++] = '0' + (nc_v % 10); nc_v /= 10; }
            for (int j = 0; j < ti; j++) nc[nc_i++] = tmp[ti - 1 - j];
        }
        nc[nc_i] = 0;
        s_printf(nc);
        s_printf(" body=");
        s_printf(doc->body ? doc->body->tag : "NULL");
        s_printf(" children=");
        {
            int cc = doc->body ? doc->body->child_count : 0;
            char cc_s[16]; int cc_i = 0;
            if (cc == 0) { cc_s[0] = '0'; cc_i = 1; }
            else { char tmp[16]; int ti = 0;
                while (cc > 0) { tmp[ti++] = '0' + (cc % 10); cc /= 10; }
                for (int j = 0; j < ti; j++) cc_s[cc_i++] = tmp[ti - 1 - j];
            }
            cc_s[cc_i] = 0;
            s_printf(cc_s);
        }
        s_printf("\n");
    }

    return 0;
}

// ============================================================================
// CSS PARSING AND APPLICATION
// ============================================================================

// Parse a single CSS property value into a dom_style_t
static void apply_css_property(dom_style_t *style, const char *prop_name, const char *prop_value) {
    if (!style || !prop_name || !prop_value) return;

    // Color properties
    if (strcmp(prop_name, "color") == 0) {
        dom_parse_color(prop_value, &style->color);
    }
    else if (strcmp(prop_name, "background-color") == 0) {
        dom_parse_color(prop_value, &style->background_color);
    }
    else if (strcmp(prop_name, "background") == 0) {
        // Simplified: try to parse as color, ignore gradient/URL
        dom_parse_color(prop_value, &style->background_color);
    }
    // Font properties
    else if (strcmp(prop_name, "font-size") == 0) {
        int val = 0;
        const char *vp = prop_value;
        // Parse number
        while (*vp && *vp >= '0' && *vp <= '9') {
            val = val * 10 + (*vp - '0');
            vp++;
        }
        // Skip unit (px, em, rem, pt, %)
        if (val > 0) {
            // If percentage, scale relative to parent (16px default)
            if (*vp == '%') {
                val = (val * 16) / 100;
            }
            // em and rem: approximate as *16
            else if (*vp == 'e' || *vp == 'r') {
                val = val * 16;
            }
            style->font_size = val;
        }
    }
    else if (strcmp(prop_name, "font-weight") == 0) {
        if (strcmp(prop_value, "bold") == 0 || strcmp(prop_value, "700") == 0 ||
            strcmp(prop_value, "800") == 0 || strcmp(prop_value, "900") == 0) {
            style->font_weight = DOM_FONT_WEIGHT_BOLD;
        } else {
            style->font_weight = DOM_FONT_WEIGHT_NORMAL;
        }
    }
    // Text alignment
    else if (strcmp(prop_name, "text-align") == 0) {
        if (strcmp(prop_value, "center") == 0)       style->text_align = DOM_TEXT_ALIGN_CENTER;
        else if (strcmp(prop_value, "right") == 0)    style->text_align = DOM_TEXT_ALIGN_RIGHT;
        else                                           style->text_align = DOM_TEXT_ALIGN_LEFT;
    }
    // Width
    else if (strcmp(prop_name, "width") == 0) {
        const char *vp = prop_value;
        int val = 0;
        while (*vp && *vp >= '0' && *vp <= '9') {
            val = val * 10 + (*vp - '0');
            vp++;
        }
        if (*vp == '%') {
            style->width_pct = val;
            style->width = -1;
        } else {
            style->width = val;
            style->width_pct = -1;
        }
    }
    // Height
    else if (strcmp(prop_name, "height") == 0) {
        const char *vp = prop_value;
        int val = 0;
        while (*vp && *vp >= '0' && *vp <= '9') {
            val = val * 10 + (*vp - '0');
            vp++;
        }
        if (*vp == '%') {
            style->height_pct = val;
            style->height = -1;
        } else {
            style->height = val;
            style->height_pct = -1;
        }
    }
    // Margins (shorthand and individual)
    else if (strcmp(prop_name, "margin") == 0) {
        int vals[4] = {0, 0, 0, 0};
        int count = 0;
        const char *vp = prop_value;
        while (*vp && count < 4) {
            while (is_ws(*vp)) vp++;
            int val = 0;
            while (*vp && *vp >= '0' && *vp <= '9') {
                val = val * 10 + (*vp - '0');
                vp++;
            }
            // Skip unit
            while (*vp && !is_ws(*vp) && *vp != ',') vp++;
            vals[count++] = val;
        }
        if (count == 1) {
            style->margin[0] = style->margin[1] = style->margin[2] = style->margin[3] = vals[0];
        } else if (count == 2) {
            style->margin[0] = style->margin[2] = vals[0];
            style->margin[1] = style->margin[3] = vals[1];
        } else if (count == 3) {
            style->margin[0] = vals[0]; style->margin[1] = vals[1];
            style->margin[2] = vals[2]; style->margin[3] = vals[1];
        } else if (count >= 4) {
            style->margin[0] = vals[0]; style->margin[1] = vals[1];
            style->margin[2] = vals[2]; style->margin[3] = vals[3];
        }
    }
    else if (strcmp(prop_name, "margin-top") == 0)    { parse_int(prop_value, &style->margin[0]); }
    else if (strcmp(prop_name, "margin-right") == 0)  { parse_int(prop_value, &style->margin[1]); }
    else if (strcmp(prop_name, "margin-bottom") == 0) { parse_int(prop_value, &style->margin[2]); }
    else if (strcmp(prop_name, "margin-left") == 0)   { parse_int(prop_value, &style->margin[3]); }
    // Padding (shorthand and individual)
    else if (strcmp(prop_name, "padding") == 0) {
        int vals[4] = {0, 0, 0, 0};
        int count = 0;
        const char *vp = prop_value;
        while (*vp && count < 4) {
            while (is_ws(*vp)) vp++;
            int val = 0;
            while (*vp && *vp >= '0' && *vp <= '9') {
                val = val * 10 + (*vp - '0');
                vp++;
            }
            while (*vp && !is_ws(*vp) && *vp != ',') vp++;
            vals[count++] = val;
        }
        if (count == 1) {
            style->padding[0] = style->padding[1] = style->padding[2] = style->padding[3] = vals[0];
        } else if (count == 2) {
            style->padding[0] = style->padding[2] = vals[0];
            style->padding[1] = style->padding[3] = vals[1];
        } else if (count == 3) {
            style->padding[0] = vals[0]; style->padding[1] = vals[1];
            style->padding[2] = vals[2]; style->padding[3] = vals[1];
        } else if (count >= 4) {
            style->padding[0] = vals[0]; style->padding[1] = vals[1];
            style->padding[2] = vals[2]; style->padding[3] = vals[3];
        }
    }
    else if (strcmp(prop_name, "padding-top") == 0)    { parse_int(prop_value, &style->padding[0]); }
    else if (strcmp(prop_name, "padding-right") == 0)  { parse_int(prop_value, &style->padding[1]); }
    else if (strcmp(prop_name, "padding-bottom") == 0) { parse_int(prop_value, &style->padding[2]); }
    else if (strcmp(prop_name, "padding-left") == 0)   { parse_int(prop_value, &style->padding[3]); }
    // Border (simplified)
    else if (strcmp(prop_name, "border") == 0 ||
             strcmp(prop_name, "border-top") == 0 ||
             strcmp(prop_name, "border-right") == 0 ||
             strcmp(prop_name, "border-bottom") == 0 ||
             strcmp(prop_name, "border-left") == 0) {
        // Parse "1px solid #color" or "1px solid red"
        int bwidth = 0;
        dom_border_style_t bstyle = DOM_BORDER_STYLE_SOLID;
        uint32_t bcolor = DOM_COLOR_BLACK;

        const char *vp = skip_ws(prop_value);
        // Parse width
        int wval = 0;
        while (*vp && *vp >= '0' && *vp <= '9') {
            wval = wval * 10 + (*vp - '0');
            vp++;
        }
        bwidth = wval;
        // Skip unit (px, etc.)
        while (*vp && *vp != ' ' && *vp != '\t') vp++;
        vp = skip_ws(vp);
        // Parse style keyword
        if (str_casencmp(vp, "solid", 5) == 0) {
            bstyle = DOM_BORDER_STYLE_SOLID; vp += 5;
        } else if (str_casencmp(vp, "dashed", 6) == 0) {
            bstyle = DOM_BORDER_STYLE_DASHED; vp += 6;
        } else if (str_casencmp(vp, "dotted", 6) == 0) {
            bstyle = DOM_BORDER_STYLE_DOTTED; vp += 6;
        } else if (str_casencmp(vp, "none", 4) == 0) {
            bstyle = DOM_BORDER_STYLE_NONE; bwidth = 0; vp += 4;
        }
        vp = skip_ws(vp);
        // Parse color
        if (*vp) {
            dom_parse_color(vp, &bcolor);
        }

        dom_border_side_t bs;
        bs.width = bwidth;
        bs.style = bstyle;
        bs.color = bcolor;

        if (strcmp(prop_name, "border") == 0) {
            style->border[0] = style->border[1] = style->border[2] = style->border[3] = bs;
        } else if (strcmp(prop_name, "border-top") == 0)    { style->border[0] = bs; }
        else if (strcmp(prop_name, "border-right") == 0)   { style->border[1] = bs; }
        else if (strcmp(prop_name, "border-bottom") == 0)  { style->border[2] = bs; }
        else if (strcmp(prop_name, "border-left") == 0)    { style->border[3] = bs; }
    }
    else if (strcmp(prop_name, "border-width") == 0) {
        int val = 0;
        parse_int(prop_value, &val);
        style->border[0].width = style->border[1].width =
        style->border[2].width = style->border[3].width = val;
    }
    else if (strcmp(prop_name, "border-color") == 0) {
        uint32_t col = DOM_COLOR_BLACK;
        dom_parse_color(prop_value, &col);
        style->border[0].color = style->border[1].color =
        style->border[2].color = style->border[3].color = col;
    }
    else if (strcmp(prop_name, "border-style") == 0) {
        dom_border_style_t bs = DOM_BORDER_STYLE_SOLID;
        if (strcmp(prop_value, "none") == 0)        bs = DOM_BORDER_STYLE_NONE;
        else if (strcmp(prop_value, "dashed") == 0)  bs = DOM_BORDER_STYLE_DASHED;
        else if (strcmp(prop_value, "dotted") == 0)  bs = DOM_BORDER_STYLE_DOTTED;
        style->border[0].style = style->border[1].style =
        style->border[2].style = style->border[3].style = bs;
    }
    // Border radius
    else if (strcmp(prop_name, "border-radius") == 0) {
        int val = 0;
        const char *vp = prop_value;
        while (*vp && *vp >= '0' && *vp <= '9') {
            val = val * 10 + (*vp - '0');
            vp++;
        }
        style->border_radius = val;
    }
    // Font style
    else if (strcmp(prop_name, "font-style") == 0) {
        if (strcmp(prop_value, "italic") == 0 || strcmp(prop_value, "oblique") == 0) {
            style->font_style = DOM_FONT_STYLE_ITALIC;
        } else {
            style->font_style = DOM_FONT_STYLE_NORMAL;
        }
    }
    // Font family (simplified - detect monospace)
    else if (strcmp(prop_name, "font-family") == 0) {
        if (str_casestr(prop_value, "monospace") ||
            str_casestr(prop_value, "courier") ||
            str_casestr(prop_value, "consolas") ||
            str_casestr(prop_value, "menlo")) {
            style->font_family_monospace = 1;
        } else {
            style->font_family_monospace = 0;
        }
    }
    // Text decoration
    else if (strcmp(prop_name, "text-decoration") == 0) {
        if (str_casestr(prop_value, "underline")) {
            style->text_decoration = DOM_TEXT_DECOR_UNDERLINE;
        } else if (str_casestr(prop_value, "line-through") || str_casestr(prop_value, "strikethrough")) {
            style->text_decoration = DOM_TEXT_DECOR_LINE_THROUGH;
        } else if (str_casestr(prop_value, "overline")) {
            style->text_decoration = DOM_TEXT_DECOR_OVERLINE;
        } else if (strcmp(prop_value, "none") == 0) {
            style->text_decoration = DOM_TEXT_DECOR_NONE;
        }
    }
    // Line height
    else if (strcmp(prop_name, "line-height") == 0) {
        int val = 0;
        const char *vp = prop_value;
        while (*vp && *vp >= '0' && *vp <= '9') {
            val = val * 10 + (*vp - '0');
            vp++;
        }
        if (*vp == '.' || *vp == ',') {
            // Decimal value like "1.5" means multiplier
            vp++;
            int frac = 0;
            int divisor = 1;
            while (*vp && *vp >= '0' && *vp <= '9') {
                frac = frac * 10 + (*vp - '0');
                divisor *= 10;
                vp++;
            }
            // Convert to px: multiplier * font_size
            if (val == 0) val = 1;
            style->line_height = (val * style->font_size) +
                                  (frac * style->font_size) / divisor;
        } else if (val > 0) {
            // Absolute value in px (skip unit)
            style->line_height = val;
        } else {
            style->line_height = 0; // auto
        }
    }
    // Letter spacing
    else if (strcmp(prop_name, "letter-spacing") == 0) {
        if (strcmp(prop_value, "normal") == 0) {
            style->letter_spacing = 0;
        } else {
            int val = 0;
            const char *vp = prop_value;
            while (*vp && *vp >= '0' && *vp <= '9') {
                val = val * 10 + (*vp - '0');
                vp++;
            }
            style->letter_spacing = val;
        }
    }
    // Word spacing
    else if (strcmp(prop_name, "word-spacing") == 0) {
        if (strcmp(prop_value, "normal") == 0) {
            style->word_spacing = 0;
        } else {
            int val = 0;
            const char *vp = prop_value;
            while (*vp && *vp >= '0' && *vp <= '9') {
                val = val * 10 + (*vp - '0');
                vp++;
            }
            style->word_spacing = val;
        }
    }
    // Text transform
    else if (strcmp(prop_name, "text-transform") == 0) {
        if (strcmp(prop_value, "uppercase") == 0)        style->text_transform = DOM_TEXT_TRANSFORM_UPPERCASE;
        else if (strcmp(prop_value, "lowercase") == 0)    style->text_transform = DOM_TEXT_TRANSFORM_LOWERCASE;
        else if (strcmp(prop_value, "capitalize") == 0)   style->text_transform = DOM_TEXT_TRANSFORM_CAPITALIZE;
        else                                               style->text_transform = DOM_TEXT_TRANSFORM_NONE;
    }
    // Display - expanded with inline-block
    else if (strcmp(prop_name, "display") == 0) {
        if (strcmp(prop_value, "none") == 0)              style->display = DOM_DISPLAY_NONE;
        else if (strcmp(prop_value, "inline") == 0)       style->display = DOM_DISPLAY_INLINE;
        else if (strcmp(prop_value, "inline-block") == 0) style->display = DOM_DISPLAY_INLINE_BLOCK;
        else if (strcmp(prop_value, "block") == 0)        style->display = DOM_DISPLAY_BLOCK;
        // Treat flex, grid, table as block for our simple engine
        else                                               style->display = DOM_DISPLAY_BLOCK;
    }
    // Position
    else if (strcmp(prop_name, "position") == 0) {
        if (strcmp(prop_value, "static") == 0)            style->position = DOM_POSITION_STATIC;
        else if (strcmp(prop_value, "relative") == 0)     style->position = DOM_POSITION_RELATIVE;
        else if (strcmp(prop_value, "absolute") == 0)     style->position = DOM_POSITION_ABSOLUTE;
        else if (strcmp(prop_value, "fixed") == 0)        style->position = DOM_POSITION_FIXED;
    }
    // Position offsets
    else if (strcmp(prop_name, "top") == 0)              { parse_int(prop_value, &style->top); }
    else if (strcmp(prop_name, "left") == 0)             { parse_int(prop_value, &style->left); }
    else if (strcmp(prop_name, "right") == 0)            { parse_int(prop_value, &style->right); }
    else if (strcmp(prop_name, "bottom") == 0)           { parse_int(prop_value, &style->bottom); }
    else if (strcmp(prop_name, "z-index") == 0)          { parse_int(prop_value, &style->z_index); }
    // Overflow
    else if (strcmp(prop_name, "overflow") == 0) {
        if (strcmp(prop_value, "hidden") == 0)            style->overflow = DOM_OVERFLOW_HIDDEN;
        else if (strcmp(prop_value, "scroll") == 0)       style->overflow = DOM_OVERFLOW_SCROLL;
        else if (strcmp(prop_value, "auto") == 0)         style->overflow = DOM_OVERFLOW_AUTO;
        else                                               style->overflow = DOM_OVERFLOW_VISIBLE;
    }
    else if (strcmp(prop_name, "overflow-x") == 0 ||
             strcmp(prop_name, "overflow-y") == 0) {
        // Simplified: apply same overflow to both axes
        if (strcmp(prop_value, "hidden") == 0)            style->overflow = DOM_OVERFLOW_HIDDEN;
        else if (strcmp(prop_value, "scroll") == 0)       style->overflow = DOM_OVERFLOW_SCROLL;
        else if (strcmp(prop_value, "auto") == 0)         style->overflow = DOM_OVERFLOW_AUTO;
    }
    // Vertical alignment
    else if (strcmp(prop_name, "vertical-align") == 0) {
        if (strcmp(prop_value, "top") == 0)               style->vertical_align = DOM_VALIGN_TOP;
        else if (strcmp(prop_value, "middle") == 0)       style->vertical_align = DOM_VALIGN_MIDDLE;
        else if (strcmp(prop_value, "bottom") == 0)       style->vertical_align = DOM_VALIGN_BOTTOM;
        else                                               style->vertical_align = DOM_VALIGN_BASELINE;
    }
    // White space
    else if (strcmp(prop_name, "white-space") == 0) {
        if (strcmp(prop_value, "pre") == 0)               style->white_space = DOM_WHITESPACE_PRE;
        else if (strcmp(prop_value, "nowrap") == 0)       style->white_space = DOM_WHITESPACE_NOWRAP;
        else                                               style->white_space = DOM_WHITESPACE_NORMAL;
    }
    // List style type
    else if (strcmp(prop_name, "list-style-type") == 0 ||
             strcmp(prop_name, "list-style") == 0) {
        if (strcmp(prop_value, "disc") == 0)              style->list_style_type = DOM_LIST_STYLE_DISC;
        else if (strcmp(prop_value, "circle") == 0)       style->list_style_type = DOM_LIST_STYLE_CIRCLE;
        else if (strcmp(prop_value, "square") == 0)       style->list_style_type = DOM_LIST_STYLE_SQUARE;
        else if (strcmp(prop_value, "decimal") == 0)      style->list_style_type = DOM_LIST_STYLE_DECIMAL;
        else if (strcmp(prop_value, "none") == 0)         style->list_style_type = DOM_LIST_STYLE_NONE;
    }
    // Opacity (0.0-1.0 -> 0-255)
    else if (strcmp(prop_name, "opacity") == 0) {
        int whole = 0, frac = 0;
        const char *vp = prop_value;
        while (*vp && *vp >= '0' && *vp <= '9') {
            whole = whole * 10 + (*vp - '0');
            vp++;
        }
        if (*vp == '.' || *vp == ',') {
            vp++;
            if (*vp >= '0' && *vp <= '9') { frac = *vp - '0'; vp++; }
        }
        // Convert to 0-255 range
        style->opacity = (whole * 255) + (frac * 25);
        if (style->opacity < 0) style->opacity = 0;
        if (style->opacity > 255) style->opacity = 255;
    }
    // Visibility
    else if (strcmp(prop_name, "visibility") == 0) {
        if (strcmp(prop_value, "hidden") == 0)   style->visible = 0;
        else if (strcmp(prop_value, "collapse") == 0) style->visible = 0;
        else                                       style->visible = 1;
    }
    // Min/max sizing
    else if (strcmp(prop_name, "min-width") == 0)       { parse_int(prop_value, &style->min_width); }
    else if (strcmp(prop_name, "max-width") == 0)       { parse_int(prop_value, &style->max_width); }
    else if (strcmp(prop_name, "min-height") == 0)      { parse_int(prop_value, &style->min_height); }
    else if (strcmp(prop_name, "max-height") == 0)      { parse_int(prop_value, &style->max_height); }
    // Text indent
    else if (strcmp(prop_name, "text-indent") == 0)     { parse_int(prop_value, &style->padding[3]); }
}

// Apply inline style attribute to a node
static void apply_inline_style(dom_node_t *node) {
    if (!node || node->type != DOM_NODE_ELEMENT || !node->style[0]) return;

    // Parse inline style: "prop1: val1; prop2: val2;"
    const char *p = node->style;
    while (*p) {
        // Skip whitespace
        while (is_ws(*p)) p++;
        if (!*p) break;

        // Read property name
        char prop_name[DOM_MAX_ATTR_NAME];
        int pi = 0;
        while (*p && *p != ':' && !is_ws(*p)) {
            if (pi < DOM_MAX_ATTR_NAME - 1) prop_name[pi++] = *p;
            p++;
        }
        prop_name[pi] = '\0';

        // Skip whitespace and ':'
        while (is_ws(*p)) p++;
        if (*p == ':') p++;
        while (is_ws(*p)) p++;

        // Read property value (until ';' or end)
        char prop_value[DOM_MAX_ATTR_VALUE];
        int vi = 0;
        while (*p && *p != ';') {
            if (vi < DOM_MAX_ATTR_VALUE - 1) prop_value[vi++] = *p;
            p++;
        }
        prop_value[vi] = '\0';
        // Trim trailing whitespace from value
        while (vi > 0 && is_ws(prop_value[vi - 1])) {
            prop_value[--vi] = '\0';
        }

        if (pi > 0 && vi > 0) {
            apply_css_property(&node->computed_style, prop_name, prop_value);
        }

        // Skip ';'
        if (*p == ';') p++;
    }
}

// Parse CSS text into rules and store in the document's rule array
int dom_apply_css(dom_document_t *doc, const char *css_text) {
    if (!doc || !css_text) return -1;

    const char *p = css_text;
    int rules_parsed = 0;

    while (*p) {
        // Skip whitespace and comments
        p = skip_ws(p);
        if (!*p) break;

        // Skip CSS comments /* ... */
        if (*p == '/' && *(p + 1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p + 1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        // Read selector(s) - everything before '{'
        char selector_buf[DOM_MAX_SELECTOR_LEN];
        int si = 0;
        while (*p && *p != '{') {
            if (si < DOM_MAX_SELECTOR_LEN - 1) {
                selector_buf[si++] = *p;
            }
            p++;
        }
        selector_buf[si] = '\0';
        // Trim whitespace from selector
        while (si > 0 && is_ws(selector_buf[si - 1])) {
            selector_buf[--si] = '\0';
        }

        if (!*p) break;
        p++; // skip '{'

        if (si == 0) {
            // No selector, skip to '}'
            while (*p && *p != '}') p++;
            if (*p) p++;
            continue;
        }

        // For simplicity, handle only the first simple selector
        // (tag, .class, or #id) - ignore combinators and compound selectors
        dom_css_selector_type_t sel_type = DOM_CSS_SEL_TAG;
        char sel_value[DOM_MAX_SELECTOR_LEN];
        sel_value[0] = '\0';

        const char *sp = skip_ws(selector_buf);
        // Skip any leading noise (e.g., multiple selectors separated by commas)
        // We'll handle just the first one
        if (*sp == '.') {
            sel_type = DOM_CSS_SEL_CLASS;
            sp++;
            int i = 0;
            while (*sp && !is_ws(*sp) && *sp != ',' && *sp != '{' && i < DOM_MAX_SELECTOR_LEN - 1) {
                sel_value[i++] = *sp++;
            }
            sel_value[i] = '\0';
        } else if (*sp == '#') {
            sel_type = DOM_CSS_SEL_ID;
            sp++;
            int i = 0;
            while (*sp && !is_ws(*sp) && *sp != ',' && *sp != '{' && i < DOM_MAX_SELECTOR_LEN - 1) {
                sel_value[i++] = *sp++;
            }
            sel_value[i] = '\0';
        } else if (*sp == '*') {
            sel_type = DOM_CSS_SEL_TAG;
            sel_value[0] = '*';
            sel_value[1] = '\0';
        } else {
            sel_type = DOM_CSS_SEL_TAG;
            int i = 0;
            while (*sp && !is_ws(*sp) && *sp != '.' && *sp != '#' && *sp != ':' &&
                   *sp != '[' && *sp != ',' && i < DOM_MAX_SELECTOR_LEN - 1) {
                sel_value[i++] = *sp++;
            }
            sel_value[i] = '\0';
        }

        // Read property declarations until '}'
        if (doc->css_rule_count >= DOM_MAX_CSS_RULES) {
            // Skip the rest
            while (*p && *p != '}') p++;
            if (*p) p++;
            continue;
        }

        dom_css_rule_t *rule = &doc->css_rules[doc->css_rule_count];
        rule->selector_type = sel_type;
        safe_strcpy(rule->selector_value, sel_value, DOM_MAX_SELECTOR_LEN);
        rule->property_count = 0;

        while (*p && *p != '}') {
            p = skip_ws(p);
            if (*p == '}') break;

            // Read property name
            char pname[DOM_MAX_ATTR_NAME];
            int pni = 0;
            while (*p && *p != ':' && !is_ws(*p) && *p != '}') {
                if (pni < DOM_MAX_ATTR_NAME - 1) pname[pni++] = *p;
                p++;
            }
            pname[pni] = '\0';

            // Skip whitespace and ':'
            while (is_ws(*p)) p++;
            if (*p == ':') p++;
            while (is_ws(*p)) p++;

            // Read property value
            char pvalue[DOM_MAX_ATTR_VALUE];
            int pvi = 0;
            while (*p && *p != ';' && *p != '}') {
                if (pvi < DOM_MAX_ATTR_VALUE - 1) pvalue[pvi++] = *p;
                p++;
            }
            pvalue[pvi] = '\0';
            // Trim trailing whitespace
            while (pvi > 0 && is_ws(pvalue[pvi - 1])) {
                pvalue[--pvi] = '\0';
            }

            if (pni > 0 && pvi > 0 && rule->property_count < DOM_MAX_CSS_PROPS) {
                safe_strcpy(rule->properties[rule->property_count].name, pname, DOM_MAX_ATTR_NAME);
                safe_strcpy(rule->properties[rule->property_count].value, pvalue, DOM_MAX_ATTR_VALUE);
                rule->property_count++;
            }

            if (*p == ';') p++;
        }

        if (*p == '}') p++;

        if (rule->property_count > 0) {
            doc->css_rule_count++;
            rules_parsed++;
        }
    }

    return rules_parsed;
}

// Check if an element matches a CSS selector
static int element_matches_selector(dom_node_t *node, dom_css_selector_type_t sel_type,
                                     const char *sel_value) {
    if (!node || node->type != DOM_NODE_ELEMENT) return 0;

    switch (sel_type) {
        case DOM_CSS_SEL_TAG:
            if (sel_value[0] == '*') return 1;
            return str_casecmp(node->tag, sel_value) == 0;

        case DOM_CSS_SEL_CLASS:
            if (!node->class_name[0]) return 0;
            // Check if sel_value appears in the space-separated class list
            {
                const char *cls = node->class_name;
                int vlen = strlen(sel_value);
                while (*cls) {
                    while (*cls == ' ') cls++;
                    if (!*cls) break;
                    // Compare this class token
                    if (str_casencmp(cls, sel_value, vlen) == 0) {
                        // Make sure it's a whole word boundary
                        char next = cls[vlen];
                        if (next == ' ' || next == '\0') return 1;
                    }
                    // Skip to next token
                    while (*cls && *cls != ' ') cls++;
                }
            }
            return 0;

        case DOM_CSS_SEL_ID:
            return strcmp(node->id, sel_value) == 0;

        default:
            return 0;
    }
}

// Apply all CSS rules to matching elements in the subtree
static void apply_css_rules_to_tree(dom_document_t *doc, dom_node_t *node) {
    if (!node) return;

    if (node->type == DOM_NODE_ELEMENT) {
        // Apply each CSS rule
        for (int r = 0; r < doc->css_rule_count; r++) {
            dom_css_rule_t *rule = &doc->css_rules[r];
            if (element_matches_selector(node, rule->selector_type, rule->selector_value)) {
                // Apply properties
                for (int p = 0; p < rule->property_count; p++) {
                    apply_css_property(&node->computed_style,
                                       rule->properties[p].name,
                                       rule->properties[p].value);
                }
            }
        }

        // Apply inline style (highest priority)
        apply_inline_style(node);
    }

    // Recurse into children
    dom_node_t *child = node->first_child;
    while (child) {
        apply_css_rules_to_tree(doc, child);
        child = child->next_sibling;
    }
}

void dom_apply_all_stylesheets(dom_document_t *doc) {
    if (!doc) return;

    // Parse each collected stylesheet into rules
    for (int i = 0; i < doc->stylesheet_count; i++) {
        dom_apply_css(doc, doc->stylesheets[i]);
    }

    // Apply all rules to the DOM tree
    if (doc->root) {
        apply_css_rules_to_tree(doc, doc->root);
    }
}

// ============================================================================
// LAYOUT COMPUTATION
// ============================================================================

// Estimate the pixel width of a text string at a given font size
static int estimate_text_width(const char *text, int font_size) {
    if (!text) return 0;
    // Rough estimate: average character width is ~0.6 * font_size
    int len = strlen(text);
    return (len * font_size * 6) / 10;
}

// Compute the height of a text string, accounting for word wrap within a max width
static int compute_text_height(const char *text, int font_size, int max_width) {
    if (!text || !*text || max_width <= 0) return font_size;

    int line_height = (font_size * 12) / 10; // 1.2x line height
    int total_height = line_height;
    int line_width = 0;
    int char_width = (font_size * 6) / 10;
    const char *p = text;

    while (*p) {
        if (*p == '\n') {
            total_height += line_height;
            line_width = 0;
            p++;
            continue;
        }
        if (*p == ' ') {
            line_width += char_width;
            // Word wrap: if we exceeded the width, wrap
            if (line_width > max_width && char_width > 0) {
                total_height += line_height;
                line_width = 0;
            }
            p++;
            continue;
        }
        // Estimate word width
        int word_width = 0;
        while (*p && *p != ' ' && *p != '\n') {
            word_width += char_width;
            p++;
        }
        if (line_width + word_width > max_width && line_width > 0) {
            total_height += line_height;
            line_width = word_width;
        } else {
            line_width += word_width;
        }
        if (line_width > max_width && word_width > max_width) {
            // Very long word, wraps multiple lines
            while (line_width > max_width) {
                total_height += line_height;
                line_width -= max_width;
            }
        }
    }

    return total_height;
}

// Layout context passed through recursive layout
typedef struct {
    int viewport_w;
    int viewport_h;
    int cursor_x;       // Current X position in the content area
    int cursor_y;       // Current Y position
    int max_x;          // Right boundary for current inline context
    int line_height;    // Current line height for inline context
} layout_ctx_t;

// Forward declaration for recursive layout
static void layout_node(dom_document_t *doc, dom_node_t *node, layout_ctx_t *ctx);

// Layout a block-level element
static void layout_block(dom_document_t *doc, dom_node_t *node, layout_ctx_t *ctx) {
    dom_style_t *s = &node->computed_style;

    // === CSS Box Model ===
    // CSS width/height are "content-box" dimensions by default
    // The box model from outside-in is: margin -> border -> padding -> content

    // Compute content width
    int content_width;
    if (s->width > 0) {
        // Explicit width: treat as content-box width (CSS standard)
        content_width = s->width;
    } else if (s->width_pct >= 0) {
        // Percentage width: relative to containing block
        content_width = (ctx->viewport_w * s->width_pct) / 100 -
                        s->border[1].width - s->border[3].width -
                        s->padding[1] - s->padding[3];
    } else {
        // Auto width: fill remaining space
        content_width = ctx->viewport_w - s->margin[1] - s->margin[3] -
                        s->border[1].width - s->border[3].width -
                        s->padding[1] - s->padding[3];
    }

    // Apply min/max width constraints
    if (s->min_width >= 0 && content_width < s->min_width)
        content_width = s->min_width;
    if (s->max_width >= 0 && content_width > s->max_width)
        content_width = s->max_width;
    if (content_width < 0) content_width = 0;

    // Border-box dimensions (what background/border fill)
    int border_box_w = content_width + s->padding[1] + s->padding[3] +
                       s->border[1].width + s->border[3].width;
    int margin_box_w = border_box_w + s->margin[1] + s->margin[3];

    // Position: block elements start on a new line
    // layout_x/layout_y are relative to parent, in margin-box coordinates
    s->layout_x = s->margin[3];
    s->layout_y = ctx->cursor_y + s->margin[0];

    // Store box dimensions
    s->layout_w = margin_box_w;

    // Content position relative to the margin-box origin
    s->content_x = s->margin[3] + s->border[3].width + s->padding[3];
    s->content_y = s->border[0].width + s->padding[0];
    s->content_w = content_width;

    // Layout children within this block
    layout_ctx_t child_ctx;
    child_ctx.viewport_w = content_width;
    child_ctx.viewport_h = ctx->viewport_h;
    child_ctx.cursor_x = 0;
    child_ctx.cursor_y = 0;
    child_ctx.max_x = content_width;
    child_ctx.line_height = s->font_size;

    dom_node_t *child = node->first_child;
    while (child) {
        layout_node(doc, child, &child_ctx);
        child = child->next_sibling;
    }

    // Compute content height
    int content_height = child_ctx.cursor_y;
    if (s->height > 0) {
        // Explicit height: treat as content-box height
        if (content_height < s->height)
            content_height = s->height;
    } else if (s->height_pct >= 0) {
        int pct_height = (ctx->viewport_h * s->height_pct) / 100 -
                         s->border[0].width - s->border[2].width -
                         s->padding[0] - s->padding[2];
        if (content_height < pct_height)
            content_height = pct_height;
    }

    // Apply min/max height constraints
    if (s->min_height >= 0 && content_height < s->min_height)
        content_height = s->min_height;
    if (s->max_height >= 0 && content_height > s->max_height)
        content_height = s->max_height;

    // Apply line-height if set
    if (s->line_height > 0 && content_height < s->line_height) {
        content_height = s->line_height;
    }

    s->content_h = content_height;

    // Border-box height
    int border_box_h = content_height + s->padding[0] + s->padding[2] +
                       s->border[0].width + s->border[2].width;

    // Margin-box height (total space this element occupies)
    s->layout_h = border_box_h + s->margin[0] + s->margin[2];

    // Advance the parent cursor past this block's margin box
    ctx->cursor_y = s->layout_y + border_box_h + s->margin[2];
}

// Layout an inline element (or text node)
static void layout_inline(dom_document_t *doc, dom_node_t *node, layout_ctx_t *ctx) {
    dom_style_t *s = &node->computed_style;

    if (node->type == DOM_NODE_TEXT) {
        // Text node: compute how much space the text takes
        int text_w = estimate_text_width(node->text, s->font_size);
        int text_h = compute_text_height(node->text, s->font_size, ctx->max_x - ctx->cursor_x);
        int line_height = (s->font_size * 12) / 10;

        // If text doesn't fit on current line, wrap
        if (ctx->cursor_x + text_w > ctx->max_x && ctx->cursor_x > 0) {
            ctx->cursor_y += ctx->line_height;
            ctx->cursor_x = 0;
            text_h = compute_text_height(node->text, s->font_size, ctx->max_x);
        }

        s->layout_x = ctx->cursor_x;
        s->layout_y = ctx->cursor_y;
        s->layout_w = text_w;
        s->layout_h = text_h;
        s->content_x = ctx->cursor_x;
        s->content_y = ctx->cursor_y;
        s->content_w = text_w;
        s->content_h = text_h;

        ctx->cursor_x += text_w;
        if (line_height > ctx->line_height) {
            ctx->line_height = line_height;
        }
        return;
    }

    // Inline element: layout children inline
    int start_x = ctx->cursor_x;
    int start_y = ctx->cursor_y;
    int max_line_h = ctx->line_height;

    // Account for padding on inline element
    ctx->cursor_x += s->padding[3];
    ctx->cursor_y += s->padding[0];

    dom_node_t *child = node->first_child;
    while (child) {
        layout_node(doc, child, ctx);
        if (ctx->line_height > max_line_h) {
            max_line_h = ctx->line_height;
        }
        child = child->next_sibling;
    }

    ctx->cursor_x += s->padding[1];

    s->layout_x = start_x;
    s->layout_y = start_y;
    s->layout_w = ctx->cursor_x - start_x + s->padding[1];
    s->layout_h = max_line_h + s->padding[0] + s->padding[2];
    s->content_x = start_x + s->padding[3];
    s->content_y = start_y + s->padding[0];
    s->content_w = s->layout_w - s->padding[1] - s->padding[3];
    s->content_h = s->layout_h - s->padding[0] - s->padding[2];

    ctx->line_height = max_line_h;
}

static void layout_node(dom_document_t *doc, dom_node_t *node, layout_ctx_t *ctx) {
    if (!node) return;

    dom_style_t *s = &node->computed_style;

    // Skip hidden elements
    if (s->display == DOM_DISPLAY_NONE) {
        s->layout_h = 0;
        return;
    }

    if (node->type == DOM_NODE_TEXT) {
        layout_inline(doc, node, ctx);
        return;
    }

    // Element node
    switch (s->display) {
        case DOM_DISPLAY_BLOCK:
            layout_block(doc, node, ctx);
            break;
        case DOM_DISPLAY_INLINE:
        case DOM_DISPLAY_INLINE_BLOCK:
            layout_inline(doc, node, ctx);
            break;
        case DOM_DISPLAY_NONE:
        default:
            break;
    }
}

void dom_compute_styles(dom_document_t *doc, int viewport_w, int viewport_h) {
    if (!doc) return;

    doc->viewport_w = viewport_w;
    doc->viewport_h = viewport_h;

    if (!doc->body) return;

    // Start layout from the body element
    layout_ctx_t ctx;
    ctx.viewport_w = viewport_w;
    ctx.viewport_h = viewport_h;
    ctx.cursor_x = 0;
    ctx.cursor_y = 0;
    ctx.max_x = viewport_w;
    ctx.line_height = 16;

    layout_block(doc, doc->body, &ctx);

    doc->total_height = doc->body->computed_style.layout_h;
    if (doc->total_height < viewport_h) {
        doc->total_height = viewport_h;
    }
}

// ============================================================================
// RENDERING
// ============================================================================

// Render a single node and its children
// scroll_offset is now applied only at the dom_render level (via initial parent_y offset),
// not at each recursion level. This prevents double-applying the scroll offset.
static void render_node(dom_document_t *doc, dom_node_t *node,
                        uint32_t *buffer, int bx, int by, int bw, int bh,
                        int scroll_offset_unused, int parent_x, int parent_y) {
    if (!node || !buffer) return;

    dom_style_t *s = &node->computed_style;

    // Skip hidden nodes
    if (s->display == DOM_DISPLAY_NONE) return;
    if (!s->visible) return;  // visibility: hidden

    // Compute absolute screen position
    // parent_y already includes the scroll offset (applied once at top level)
    int abs_x = parent_x + s->layout_x;
    int abs_y = parent_y + s->layout_y;

    // Skip nodes that are entirely outside the viewport
    int node_h = s->layout_h;
    if (abs_y + node_h < by || abs_y > by + bh) {
        // Node is completely offscreen vertically, but children might be visible
        // For block elements with children, still recurse
        if (node->type == DOM_NODE_ELEMENT && node->first_child) {
            dom_node_t *child = node->first_child;
            while (child) {
                render_node(doc, child, buffer, bx, by, bw, bh, 0, abs_x, abs_y);
                child = child->next_sibling;
            }
        }
        return;
    }

    if (node->type == DOM_NODE_TEXT) {
        // Render text content
        if (node->text[0] && s->font_size > 0) {
            // content_x/content_y are absolute positions in the parent coordinate system,
            // NOT relative to layout_x/layout_y. So compute from parent directly.
            int text_x = parent_x + s->content_x;
            int text_y = parent_y + s->content_y;

            // Check if text is within the visible area
            if (text_y >= by && text_y < by + bh - s->font_size) {
                int font_scale = 1;
                if (s->font_size >= 24) font_scale = 2;
                // For very large headings
                if (s->font_size >= 32) font_scale = 3;

                // Handle text alignment
                int draw_x = text_x;
                if (s->text_align == DOM_TEXT_ALIGN_CENTER) {
                    int tw = estimate_text_width(node->text, s->font_size);
                    int container_w = s->content_w;
                    if (container_w > tw) {
                        draw_x = text_x + (container_w - tw) / 2;
                    }
                } else if (s->text_align == DOM_TEXT_ALIGN_RIGHT) {
                    int tw = estimate_text_width(node->text, s->font_size);
                    int container_w = s->content_w;
                    if (container_w > tw) {
                        draw_x = text_x + container_w - tw;
                    }
                }

                // Clip to viewport
                int clip_x = draw_x;
                if (clip_x < bx) clip_x = bx;
                if (clip_x > bx + bw) return;

                gfx_draw_string_scaled(draw_x, text_y, node->text, s->color, font_scale);

                // Draw text decoration (underline, line-through, overline)
                if (s->text_decoration != DOM_TEXT_DECOR_NONE) {
                    int tw = estimate_text_width(node->text, s->font_size);
                    int deco_y = text_y;
                    if (s->text_decoration == DOM_TEXT_DECOR_UNDERLINE) {
                        deco_y = text_y + s->font_size + 1;
                    } else if (s->text_decoration == DOM_TEXT_DECOR_LINE_THROUGH) {
                        deco_y = text_y + s->font_size / 2;
                    } else if (s->text_decoration == DOM_TEXT_DECOR_OVERLINE) {
                        deco_y = text_y - 1;
                    }
                    // Draw a thin line (1px) for the decoration
                    if (deco_y >= by && deco_y < by + bh) {
                        gfx_fill_rect(draw_x, deco_y, tw, 1, s->color);
                    }
                }
            }
        }
        return;
    }

    // Element node: draw background, border, then recurse into children
    if (node->type == DOM_NODE_ELEMENT) {
        // === CSS Box Model rendering ===
        // layout_x = margin[3], layout_y includes margin[0]
        // So abs_x/abs_y is at the margin-box origin
        //
        // From outside to inside:
        //   margin-box -> border-box -> padding-box -> content-box
        //
        // Background paints the border-box area (CSS standard).
        // Border paints on top of background at border-box edges.

        int margin_box_w = s->layout_w;
        int margin_box_h = s->layout_h;

        // Border-box position and size (background/border fill area)
        int bb_x = abs_x;
        int bb_y = abs_y;
        int bb_w = margin_box_w - s->margin[1] - s->margin[3];
        int bb_h = margin_box_h - s->margin[0] - s->margin[2];

        if (bb_w < 0) bb_w = 0;
        if (bb_h < 0) bb_h = 0;

        // Draw background (fills the border-box area)
        if (s->background_color != DOM_COLOR_TRANSPARENT && bb_w > 0 && bb_h > 0) {
            gfx_fill_rect(bb_x, bb_y, bb_w, bb_h, s->background_color);
        }

        // Draw borders (on top of background)
        if ((s->border[0].width > 0 || s->border[1].width > 0 ||
             s->border[2].width > 0 || s->border[3].width) &&
            (s->border[0].style != DOM_BORDER_STYLE_NONE ||
             s->border[1].style != DOM_BORDER_STYLE_NONE ||
             s->border[2].style != DOM_BORDER_STYLE_NONE ||
             s->border[3].style != DOM_BORDER_STYLE_NONE)) {

            int bw_top    = s->border[0].width;
            int bw_right  = s->border[1].width;
            int bw_bottom = s->border[2].width;
            int bw_left   = s->border[3].width;

            // Top border (full width of border-box, height = border-top)
            if (bw_top > 0 && s->border[0].style != DOM_BORDER_STYLE_NONE) {
                gfx_fill_rect(bb_x, bb_y, bb_w, bw_top, s->border[0].color);
            }
            // Bottom border
            if (bw_bottom > 0 && s->border[2].style != DOM_BORDER_STYLE_NONE) {
                gfx_fill_rect(bb_x, bb_y + bb_h - bw_bottom,
                              bb_w, bw_bottom, s->border[2].color);
            }
            // Left border (between top and bottom borders)
            if (bw_left > 0 && s->border[3].style != DOM_BORDER_STYLE_NONE) {
                gfx_fill_rect(bb_x, bb_y + bw_top,
                              bw_left, bb_h - bw_top - bw_bottom, s->border[3].color);
            }
            // Right border
            if (bw_right > 0 && s->border[1].style != DOM_BORDER_STYLE_NONE) {
                gfx_fill_rect(bb_x + bb_w - bw_right, bb_y + bw_top,
                              bw_right, bb_h - bw_top - bw_bottom, s->border[1].color);
            }
        }

        // Compute content-box origin for child rendering
        // Children's layout_x/layout_y are relative to the parent's content area,
        // so we need to pass the content-box origin as the parent position.
        // Content-box = border-box origin + border + padding
        int content_origin_x = abs_x + s->border[3].width + s->padding[3];
        int content_origin_y = abs_y + s->border[0].width + s->padding[0];

        // Recurse into children with content-box origin as parent position
        dom_node_t *child = node->first_child;
        while (child) {
            render_node(doc, child, buffer, bx, by, bw, bh, 0, content_origin_x, content_origin_y);
            child = child->next_sibling;
        }
    }
}

void dom_render(dom_document_t *doc, uint32_t *buffer,
                int x, int y, int w, int h, int scroll_offset) {
    if (!doc || !buffer || !doc->body) return;

    // Set up clipping region for the render area
    gfx_set_clip(x, y, w, h);

    // Clear the render area with white background
    gfx_fill_rect(x, y, w, h, DOM_COLOR_WHITE);

    // Render the DOM tree starting from body
    // Pass x, y-scroll_offset as initial parent position so content is drawn
    // at the correct screen coordinates. scroll_offset is applied only once
    // here; render_node just accumulates layout positions without re-applying scroll.
    render_node(doc, doc->body, buffer, x, y, w, h, 0, x, y - scroll_offset);

    // Reset clipping
    gfx_reset_clip();
}

// ============================================================================
// DOM QUERIES
// ============================================================================

// Static buffer for concatenated scripts
static char scripts_buffer[DOM_MAX_SCRIPTS * DOM_MAX_SCRIPT_LEN];

const char* dom_get_scripts(dom_document_t *doc) {
    if (!doc || doc->script_count == 0) return "";

    scripts_buffer[0] = '\0';
    int pos = 0;
    int max_size = sizeof(scripts_buffer);

    for (int i = 0; i < doc->script_count; i++) {
        int slen = strlen(doc->scripts[i]);
        if (pos + slen + 2 >= max_size) break;
        memcpy(scripts_buffer + pos, doc->scripts[i], slen);
        pos += slen;
        scripts_buffer[pos++] = '\n';
        scripts_buffer[pos] = '\0';
    }

    return scripts_buffer;
}

// Recursive ID lookup
static dom_node_t* find_by_id(dom_node_t *node, const char *id) {
    if (!node || !id) return NULL;

    if (node->type == DOM_NODE_ELEMENT && node->id[0] && strcmp(node->id, id) == 0) {
        return node;
    }

    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *found = find_by_id(child, id);
        if (found) return found;
        child = child->next_sibling;
    }

    return NULL;
}

dom_node_t* dom_get_element_by_id(dom_document_t *doc, const char *id) {
    if (!doc || !id) return NULL;
    return find_by_id(doc->root, id);
}

// Recursive selector match (returns first match)
static dom_node_t* find_by_selector(dom_node_t *node, const char *selector) {
    if (!node || !selector) return NULL;

    // Parse selector type
    dom_css_selector_type_t sel_type = DOM_CSS_SEL_TAG;
    char sel_value[DOM_MAX_SELECTOR_LEN];
    sel_value[0] = '\0';

    const char *sp = selector;
    if (*sp == '.') {
        sel_type = DOM_CSS_SEL_CLASS;
        sp++;
        safe_strcpy(sel_value, sp, DOM_MAX_SELECTOR_LEN);
    } else if (*sp == '#') {
        sel_type = DOM_CSS_SEL_ID;
        sp++;
        safe_strcpy(sel_value, sp, DOM_MAX_SELECTOR_LEN);
    } else {
        sel_type = DOM_CSS_SEL_TAG;
        safe_strcpy(sel_value, sp, DOM_MAX_SELECTOR_LEN);
    }

    // Check this node
    if (node->type == DOM_NODE_ELEMENT &&
        element_matches_selector(node, sel_type, sel_value)) {
        return node;
    }

    // Recurse into children
    dom_node_t *child = node->first_child;
    while (child) {
        dom_node_t *found = find_by_selector(child, selector);
        if (found) return found;
        child = child->next_sibling;
    }

    return NULL;
}

dom_node_t* dom_query_selector(dom_document_t *doc, const char *selector) {
    if (!doc || !selector) return NULL;
    return find_by_selector(doc->root, selector);
}

// ============================================================================
// DEBUG PRINTING
// ============================================================================

static void debug_print_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        s_printf("  ");
    }
}

static void debug_print_tree_recursive(dom_node_t *node, int depth) {
    if (!node) return;

    debug_print_indent(depth);

    if (node->type == DOM_NODE_TEXT) {
        // Truncate text for display
        char truncated[65];
        int len = strlen(node->text);
        int copy = len > 64 ? 64 : len;
        memcpy(truncated, node->text, copy);
        truncated[copy] = '\0';
        s_printf("TEXT: \"");
        s_printf(truncated);
        s_printf("\"\n");
    } else {
        s_printf("<");
        s_printf(node->tag);
        if (node->id[0]) {
            s_printf(" id=\"");
            s_printf(node->id);
            s_printf("\"");
        }
        if (node->class_name[0]) {
            s_printf(" class=\"");
            s_printf(node->class_name);
            s_printf("\"");
        }
        s_printf(">");

        char buf[32];
        sprintf(buf, " [display=%d w=%d h=%d]\n",
                node->computed_style.display,
                node->computed_style.layout_w,
                node->computed_style.layout_h);
        s_printf(buf);

        dom_node_t *child = node->first_child;
        while (child) {
            debug_print_tree_recursive(child, depth + 1);
            child = child->next_sibling;
        }
    }
}

void dom_debug_print_tree(dom_document_t *doc) {
    if (!doc) {
        s_printf("[DOM] Document is NULL\n");
        return;
    }
    char buf[64];
    sprintf(buf, "[DOM] Tree (%d nodes, %d CSS rules, %d scripts):\n",
            doc->node_count, doc->css_rule_count, doc->script_count);
    s_printf(buf);
    debug_print_tree_recursive(doc->root, 0);
}
