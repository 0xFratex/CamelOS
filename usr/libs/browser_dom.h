// usr/libs/browser_dom.h - CamelOS Browser DOM Engine Header
// Version 1.0 - HTML parsing, CSS styling, layout, and rendering
// Designed for kernel-mode operation with no standard library
#ifndef BROWSER_DOM_H
#define BROWSER_DOM_H

#include <types.h>

// ============================================================================
// CONFIGURATION LIMITS
// ============================================================================
#define DOM_MAX_NODES           1024
#define DOM_MAX_CSS_RULES       512
#define DOM_MAX_ATTRS           16
// Bumped from 8 * 4096 / 4 * 8192: modern bundled JS (webpack/vite output,
// React/Vue/jQuery runtime) routinely inlines 20-200KB of JS in a single
// <script> block and 10-50KB of CSS in a single <style>. The old 4KB cap
// silently truncated mid-token, causing mujs to throw a syntax error and
// the entire script to be skipped - which is why pages with inline JS
// rendered as if they had no JS at all.
// Memory cost: 1664KB + 832KB = ~1.3MB per dom_document_t. Acceptable
// for now; if memory becomes tight, switch to kmalloc'd per-entry buffers.
#define DOM_MAX_SCRIPTS         16
#define DOM_MAX_SCRIPT_LEN      65536
#define DOM_MAX_STYLESHEETS     12
#define DOM_MAX_STYLESHEET_LEN  65536
#define DOM_MAX_CSS_PROPS       24
#define DOM_MAX_SELECTOR_LEN    128
#define DOM_MAX_TAG_LEN         32
#define DOM_MAX_ATTR_NAME       32
#define DOM_MAX_ATTR_VALUE      256
#define DOM_MAX_ID_LEN          64
#define DOM_MAX_CLASS_LEN       128
#define DOM_MAX_STYLE_LEN       512
#define DOM_MAX_URL_LEN         256
#define DOM_MAX_TEXT_LEN        2048
#define DOM_MAX_CHILDREN        64

// ============================================================================
// COLOR HELPERS (ARGB format)
// ============================================================================
#define DOM_COLOR(r, g, b)      ((uint32_t)(0xFF000000 | ((r) << 16) | ((g) << 8) | (b)))
#define DOM_COLOR_A(a, r, g, b) ((uint32_t)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))
#define DOM_COLOR_TRANSPARENT   ((uint32_t)0x00000000)
#define DOM_COLOR_WHITE         ((uint32_t)0xFFFFFFFF)
#define DOM_COLOR_BLACK         ((uint32_t)0xFF000000)
#define DOM_COLOR_RED           ((uint32_t)0xFFFF0000)
#define DOM_COLOR_GREEN         ((uint32_t)0xFF00FF00)
#define DOM_COLOR_BLUE          ((uint32_t)0xFF0000FF)
#define DOM_COLOR_GRAY          ((uint32_t)0xFF808080)
#define DOM_COLOR_LIGHT_GRAY    ((uint32_t)0xFFC0C0C0)

// ============================================================================
// ENUMERATIONS
// ============================================================================
typedef enum {
    DOM_NODE_ELEMENT = 0,
    DOM_NODE_TEXT
} dom_node_type_t;

typedef enum {
    DOM_DISPLAY_BLOCK = 0,
    DOM_DISPLAY_INLINE,
    DOM_DISPLAY_INLINE_BLOCK,
    DOM_DISPLAY_NONE
} dom_display_t;

typedef enum {
    DOM_FONT_WEIGHT_NORMAL = 0,
    DOM_FONT_WEIGHT_BOLD
} dom_font_weight_t;

typedef enum {
    DOM_FONT_STYLE_NORMAL = 0,
    DOM_FONT_STYLE_ITALIC
} dom_font_style_t;

typedef enum {
    DOM_TEXT_DECOR_NONE = 0,
    DOM_TEXT_DECOR_UNDERLINE,
    DOM_TEXT_DECOR_LINE_THROUGH,
    DOM_TEXT_DECOR_OVERLINE
} dom_text_decoration_t;

typedef enum {
    DOM_VALIGN_BASELINE = 0,
    DOM_VALIGN_TOP,
    DOM_VALIGN_MIDDLE,
    DOM_VALIGN_BOTTOM
} dom_vertical_align_t;

typedef enum {
    DOM_WHITESPACE_NORMAL = 0,
    DOM_WHITESPACE_PRE,
    DOM_WHITESPACE_NOWRAP
} dom_whitespace_t;

typedef enum {
    DOM_TEXT_TRANSFORM_NONE = 0,
    DOM_TEXT_TRANSFORM_UPPERCASE,
    DOM_TEXT_TRANSFORM_LOWERCASE,
    DOM_TEXT_TRANSFORM_CAPITALIZE
} dom_text_transform_t;

typedef enum {
    DOM_OVERFLOW_VISIBLE = 0,
    DOM_OVERFLOW_HIDDEN,
    DOM_OVERFLOW_SCROLL,
    DOM_OVERFLOW_AUTO
} dom_overflow_t;

typedef enum {
    DOM_LIST_STYLE_DISC = 0,
    DOM_LIST_STYLE_CIRCLE,
    DOM_LIST_STYLE_SQUARE,
    DOM_LIST_STYLE_DECIMAL,
    DOM_LIST_STYLE_NONE
} dom_list_style_t;

typedef enum {
    DOM_POSITION_STATIC = 0,
    DOM_POSITION_RELATIVE,
    DOM_POSITION_ABSOLUTE,
    DOM_POSITION_FIXED
} dom_position_t;

typedef enum {
    DOM_TEXT_ALIGN_LEFT = 0,
    DOM_TEXT_ALIGN_CENTER,
    DOM_TEXT_ALIGN_RIGHT
} dom_text_align_t;

typedef enum {
    DOM_BORDER_STYLE_NONE = 0,
    DOM_BORDER_STYLE_SOLID,
    DOM_BORDER_STYLE_DASHED,
    DOM_BORDER_STYLE_DOTTED
} dom_border_style_t;

typedef enum {
    DOM_CSS_SEL_TAG = 0,
    DOM_CSS_SEL_CLASS,
    DOM_CSS_SEL_ID
} dom_css_selector_type_t;

// ============================================================================
// STRUCTURES
// ============================================================================
typedef struct {
    char name[DOM_MAX_ATTR_NAME];
    char value[DOM_MAX_ATTR_VALUE];
} dom_attr_t;

typedef struct {
    int width;
    dom_border_style_t style;
    uint32_t color;
} dom_border_side_t;

typedef struct {
    uint32_t color;
    uint32_t background_color;
    int font_size;
    dom_font_weight_t font_weight;
    dom_font_style_t font_style;
    dom_text_align_t text_align;
    dom_text_decoration_t text_decoration;
    int line_height;
    int letter_spacing;
    int word_spacing;
    dom_text_transform_t text_transform;
    int font_family_monospace;
    dom_display_t display;
    int margin[4];
    int padding[4];
    int width;
    int height;
    int width_pct;
    int height_pct;
    int min_width;
    int max_width;
    int min_height;
    int max_height;
    dom_border_side_t border[4];
    int border_radius;
    dom_position_t position;
    int top;
    int left;
    int right;
    int bottom;
    int z_index;
    dom_overflow_t overflow;
    dom_vertical_align_t vertical_align;
    dom_whitespace_t white_space;
    dom_list_style_t list_style_type;
    int opacity;
    int visible;
    int layout_x;
    int layout_y;
    int layout_w;
    int layout_h;
    int content_x;
    int content_y;
    int content_w;
    int content_h;
} dom_style_t;

struct dom_node;
typedef struct dom_node dom_node_t;

// Forward-declare png_image_t so dom_node_t can hold a decoded image
// without dragging the full png_decoder.h into every consumer of browser_dom.h.
struct png_image_t;
typedef struct png_image_t png_image_decoded_t;

struct dom_node {
    dom_node_type_t type;
    int in_use;
    char tag[DOM_MAX_TAG_LEN];
    dom_attr_t attrs[DOM_MAX_ATTRS];
    int attr_count;
    char id[DOM_MAX_ID_LEN];
    char class_name[DOM_MAX_CLASS_LEN];
    char style[DOM_MAX_STYLE_LEN];
    char href[DOM_MAX_URL_LEN];
    char src[DOM_MAX_URL_LEN];
    char text[DOM_MAX_TEXT_LEN];
    dom_node_t *parent;
    dom_node_t *first_child;
    dom_node_t *last_child;
    dom_node_t *next_sibling;
    dom_node_t *prev_sibling;
    int child_count;
    dom_style_t computed_style;
    // ── Image support (browser image loading) ──
    // For <img> nodes: decoded PNG pixel data + natural dimensions.
    // NULL until dom_load_images() has been called for this node.
    void*    image;            // png_image_t* (opaque to avoid include)
    int      image_w;          // natural width  (0 if no image / not loaded)
    int      image_h;          // natural height (0 if no image / not loaded)
    int      image_load_attempted;  // 1 = we tried (success or fail), don't retry
};

typedef struct {
    char name[DOM_MAX_ATTR_NAME];
    char value[DOM_MAX_ATTR_VALUE];
} dom_css_prop_t;

typedef struct {
    dom_css_selector_type_t selector_type;
    char selector_value[DOM_MAX_SELECTOR_LEN];
    dom_css_prop_t properties[DOM_MAX_CSS_PROPS];
    int property_count;
} dom_css_rule_t;

typedef struct {
    dom_node_t *root;
    dom_node_t *head;
    dom_node_t *body;
    dom_css_rule_t css_rules[DOM_MAX_CSS_RULES];
    int css_rule_count;
    char scripts[DOM_MAX_SCRIPTS][DOM_MAX_SCRIPT_LEN];
    int script_count;
    char stylesheets[DOM_MAX_STYLESHEETS][DOM_MAX_STYLESHEET_LEN];
    int stylesheet_count;
    char base_url[DOM_MAX_URL_LEN];
    dom_node_t node_pool[DOM_MAX_NODES];
    int node_count;
    int viewport_w;
    int viewport_h;
    int total_height;
} dom_document_t;

// ============================================================================
// DOCUMENT LIFECYCLE
// ============================================================================
dom_document_t* dom_document_create(void);
void dom_document_destroy(dom_document_t *doc);

// ============================================================================
// HTML PARSING
// ============================================================================
int dom_parse_html(dom_document_t *doc, const char *html_body);

// ============================================================================
// CSS OPERATIONS
// ============================================================================
int dom_apply_css(dom_document_t *doc, const char *css_text);
void dom_apply_all_stylesheets(dom_document_t *doc);

// ============================================================================
// LAYOUT
// ============================================================================
void dom_compute_styles(dom_document_t *doc, int viewport_w, int viewport_h);

// ============================================================================
// IMAGE LOADING (post-parse pass)
// ============================================================================
// Walks the DOM tree, finds every <img> node, fetches its `src` URL via
// the kernel HTTP client, decodes the PNG (or JPEG-as-PNG fallback), and
// stores the decoded pixels on the node. After this call, render_node()
// will blit the image. Nodes that fail to load are silently skipped
// (image_w/image_h stay 0, so layout treats them as zero-size).
void dom_load_images(dom_document_t *doc);

// ============================================================================
// RENDERING
// ============================================================================
void dom_render(dom_document_t *doc, uint32_t *buffer,
                int x, int y, int w, int h, int scroll_offset);

// ============================================================================
// DOM QUERIES
// ============================================================================
const char* dom_get_scripts(dom_document_t *doc);
dom_node_t* dom_get_element_by_id(dom_document_t *doc, const char *id);
dom_node_t* dom_query_selector(dom_document_t *doc, const char *selector);

// ============================================================================
// NODE OPERATIONS
// ============================================================================
dom_node_t* dom_node_alloc(dom_document_t *doc);
void dom_node_append_child(dom_node_t *parent, dom_node_t *child);
int dom_node_set_attr(dom_node_t *node, const char *name, const char *value);
const char* dom_node_get_attr(dom_node_t *node, const char *name);
int dom_node_get_text_content(dom_node_t *node, char *buf, int buf_size);

// ============================================================================
// STYLE DEFAULTS
// ============================================================================
void dom_style_init_defaults(dom_style_t *style);

// ============================================================================
// COLOR PARSING
// ============================================================================
int dom_parse_color(const char *str, uint32_t *out_color);

// ============================================================================
// DEBUG
// ============================================================================
void dom_debug_print_tree(dom_document_t *doc);

// ============================================================================
// JS BRIDGE API - High-level convenience functions
// ============================================================================
dom_document_t* dom_get_document(void);
void dom_set_bridge_document(dom_document_t* doc);
dom_node_t* dom_create_element(const char* tag_name);
dom_node_t* dom_create_text_node(const char* text);
void dom_append_child(dom_node_t* parent, dom_node_t* child);
void dom_set_attribute(dom_node_t* node, const char* name, const char* value);
void dom_set_inner_html(dom_node_t* node, const char* html);

// ============================================================================
// JS BRIDGE API - Node mutation / traversal extensions
// (Used by the mujs DOM bridge so document.createElement(...).appendChild(...)
//  and el.innerHTML = "..." actually mutate the live tree.)
// ============================================================================
void        dom_node_remove_child(dom_node_t *parent, dom_node_t *child);
void        dom_node_insert_before(dom_node_t *parent, dom_node_t *newn, dom_node_t *ref);
void        dom_node_replace_child(dom_node_t *parent, dom_node_t *newn, dom_node_t *old);
dom_node_t* dom_node_clone_node(dom_node_t *node, int deep);
void        dom_node_set_text_content(dom_node_t *node, const char *text);
int         dom_node_has_attr(dom_node_t *node, const char *name);
void        dom_node_remove_attr(dom_node_t *node, const char *name);
int         dom_classlist_contains(dom_node_t *n, const char *cls);
void        dom_classlist_add(dom_node_t *n, const char *cls);
void        dom_classlist_remove(dom_node_t *n, const char *cls);
int         dom_collect_by_tag(dom_node_t *root, const char *tag, dom_node_t **out, int max);
int         dom_collect_by_class(dom_node_t *root, const char *cls, dom_node_t **out, int max);
int         dom_collect_by_selector(dom_node_t *root, const char *sel, dom_node_t **out, int max);
int         dom_collect_children(dom_node_t *n, dom_node_t **out, int max);
// Re-run the CSS cascade (rules + inline styles) over the whole tree.
// Call AFTER JavaScript has built/mutated nodes so they get styled & laid out.
void        dom_reapply_styles(dom_document_t *doc);

// ============================================================================
// HIT TESTING - find the deepest <a> element whose rendered bounding box
// contains the given (x, y) screen coordinates. Returns the node, or NULL
// if no link is at that position. Used by the browser's click handler to
// navigate to the correct href when the page is rendered with the DOM
// engine (the old line-based links[] array has stale positions that don't
// correspond to the DOM layout).
//
// origin_x / origin_y: the top-left of the content area on screen.
// scroll_offset: vertical scroll in pixels (matches dom_render's param).
// ============================================================================
dom_node_t* dom_hit_test_link(dom_document_t *doc, int x, int y,
                              int origin_x, int origin_y, int scroll_offset);

#endif // BROWSER_DOM_H