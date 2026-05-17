// usr/libs/browser_dom.h - CamelOS Browser DOM Engine Header
// Version 1.0 - HTML parsing, CSS styling, layout, and rendering
// Designed for kernel-mode operation with no standard library

#ifndef BROWSER_DOM_H
#define BROWSER_DOM_H

#include <types.h>

// ============================================================================
// CONFIGURATION LIMITS
// ============================================================================

#define DOM_MAX_NODES           512
#define DOM_MAX_CSS_RULES       64
#define DOM_MAX_ATTRS           32
#define DOM_MAX_SCRIPTS         8
#define DOM_MAX_SCRIPT_LEN      4096
#define DOM_MAX_STYLESHEETS     4
#define DOM_MAX_STYLESHEET_LEN  8192
#define DOM_MAX_CSS_PROPS       16
#define DOM_MAX_SELECTOR_LEN    64
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

// DOM node types
typedef enum {
    DOM_NODE_ELEMENT = 0,
    DOM_NODE_TEXT
} dom_node_type_t;

// CSS display values
typedef enum {
    DOM_DISPLAY_BLOCK = 0,
    DOM_DISPLAY_INLINE,
    DOM_DISPLAY_INLINE_BLOCK,
    DOM_DISPLAY_NONE
} dom_display_t;

// CSS font weight
typedef enum {
    DOM_FONT_WEIGHT_NORMAL = 0,
    DOM_FONT_WEIGHT_BOLD
} dom_font_weight_t;

// CSS font style
typedef enum {
    DOM_FONT_STYLE_NORMAL = 0,
    DOM_FONT_STYLE_ITALIC
} dom_font_style_t;

// CSS text decoration
typedef enum {
    DOM_TEXT_DECOR_NONE = 0,
    DOM_TEXT_DECOR_UNDERLINE,
    DOM_TEXT_DECOR_LINE_THROUGH,
    DOM_TEXT_DECOR_OVERLINE
} dom_text_decoration_t;

// CSS vertical align
typedef enum {
    DOM_VALIGN_BASELINE = 0,
    DOM_VALIGN_TOP,
    DOM_VALIGN_MIDDLE,
    DOM_VALIGN_BOTTOM
} dom_vertical_align_t;

// CSS white space
typedef enum {
    DOM_WHITESPACE_NORMAL = 0,
    DOM_WHITESPACE_PRE,
    DOM_WHITESPACE_NOWRAP
} dom_whitespace_t;

// CSS text transform
typedef enum {
    DOM_TEXT_TRANSFORM_NONE = 0,
    DOM_TEXT_TRANSFORM_UPPERCASE,
    DOM_TEXT_TRANSFORM_LOWERCASE,
    DOM_TEXT_TRANSFORM_CAPITALIZE
} dom_text_transform_t;

// CSS overflow
typedef enum {
    DOM_OVERFLOW_VISIBLE = 0,
    DOM_OVERFLOW_HIDDEN,
    DOM_OVERFLOW_SCROLL,
    DOM_OVERFLOW_AUTO
} dom_overflow_t;

// CSS list style type
typedef enum {
    DOM_LIST_STYLE_DISC = 0,
    DOM_LIST_STYLE_CIRCLE,
    DOM_LIST_STYLE_SQUARE,
    DOM_LIST_STYLE_DECIMAL,
    DOM_LIST_STYLE_NONE
} dom_list_style_t;

// CSS position
typedef enum {
    DOM_POSITION_STATIC = 0,
    DOM_POSITION_RELATIVE,
    DOM_POSITION_ABSOLUTE,
    DOM_POSITION_FIXED
} dom_position_t;

// CSS text alignment
typedef enum {
    DOM_TEXT_ALIGN_LEFT = 0,
    DOM_TEXT_ALIGN_CENTER,
    DOM_TEXT_ALIGN_RIGHT
} dom_text_align_t;

// CSS border style
typedef enum {
    DOM_BORDER_STYLE_NONE = 0,
    DOM_BORDER_STYLE_SOLID,
    DOM_BORDER_STYLE_DASHED,
    DOM_BORDER_STYLE_DOTTED
} dom_border_style_t;

// CSS selector types
typedef enum {
    DOM_CSS_SEL_TAG = 0,
    DOM_CSS_SEL_CLASS,
    DOM_CSS_SEL_ID
} dom_css_selector_type_t;

// ============================================================================
// STRUCTURES
// ============================================================================

// DOM attribute
typedef struct {
    char name[DOM_MAX_ATTR_NAME];
    char value[DOM_MAX_ATTR_VALUE];
} dom_attr_t;

// CSS border computed style (one side)
typedef struct {
    int width;                          // px, 0 = none
    dom_border_style_t style;
    uint32_t color;                     // ARGB
} dom_border_side_t;

// Computed style for a DOM node
typedef struct {
    // Colors
    uint32_t color;                     // Text color (ARGB)
    uint32_t background_color;          // Background color (ARGB)

    // Typography
    int font_size;                      // px (default 16)
    dom_font_weight_t font_weight;
    dom_font_style_t font_style;        // normal/italic
    dom_text_align_t text_align;
    dom_text_decoration_t text_decoration; // underline/line-through/overline/none
    int line_height;                    // px (0 = auto, use 1.2x font_size)
    int letter_spacing;                 // px (0 = normal)
    int word_spacing;                   // px (0 = normal)
    dom_text_transform_t text_transform; // uppercase/lowercase/capitalize/none

    // Font family (simplified - just stores preference)
    int font_family_monospace;          // 1 = monospace, 0 = proportional

    // Display
    dom_display_t display;

    // Box model - margins (top, right, bottom, left)
    int margin[4];
    // Box model - padding (top, right, bottom, left)
    int padding[4];

    // Sizing
    int width;                          // px, -1 = auto
    int height;                         // px, -1 = auto
    int width_pct;                      // percentage, -1 = not set
    int height_pct;                     // percentage, -1 = not set

    // Min/max sizing
    int min_width;                      // px, -1 = not set
    int max_width;                      // px, -1 = not set
    int min_height;                     // px, -1 = not set
    int max_height;                     // px, -1 = not set

    // Borders (top, right, bottom, left)
    dom_border_side_t border[4];

    // Border radius
    int border_radius;                  // px, 0 = sharp corners

    // Position
    dom_position_t position;            // static/relative/absolute/fixed
    int top;                            // px offset, 0 = not set
    int left;                           // px offset, 0 = not set
    int right;                          // px offset, -1 = not set
    int bottom;                         // px offset, -1 = not set
    int z_index;                        // stacking order, 0 = auto

    // Overflow
    dom_overflow_t overflow;            // visible/hidden/scroll/auto

    // Vertical alignment
    dom_vertical_align_t vertical_align;

    // White space
    dom_whitespace_t white_space;       // normal/pre/nowrap

    // List style
    dom_list_style_t list_style_type;   // disc/circle/square/decimal/none

    // Opacity (0-255, where 255 = fully opaque)
    int opacity;                        // 0-255

    // Visibility (1 = visible, 0 = hidden)
    int visible;                        // 1=visible, 0=hidden (occupies space)

    // Computed layout positions (filled by dom_compute_styles)
    int layout_x;                       // Absolute x position
    int layout_y;                       // Absolute y position
    int layout_w;                       // Total box width
    int layout_h;                       // Total box height
    int content_x;                      // Content area x offset
    int content_y;                      // Content area y offset
    int content_w;                      // Content area width
    int content_h;                      // Content area height
} dom_style_t;

// Forward declaration
struct dom_node;
typedef struct dom_node dom_node_t;

// DOM node
struct dom_node {
    dom_node_type_t type;
    int in_use;                         // Pool allocation flag

    // Element fields (valid when type == DOM_NODE_ELEMENT)
    char tag[DOM_MAX_TAG_LEN];
    dom_attr_t attrs[DOM_MAX_ATTRS];
    int attr_count;

    // Convenience cached attribute values
    char id[DOM_MAX_ID_LEN];
    char class_name[DOM_MAX_CLASS_LEN];
    char style[DOM_MAX_STYLE_LEN];
    char href[DOM_MAX_URL_LEN];
    char src[DOM_MAX_URL_LEN];

    // Text fields (valid when type == DOM_NODE_TEXT)
    char text[DOM_MAX_TEXT_LEN];

    // Tree structure
    dom_node_t *parent;
    dom_node_t *first_child;
    dom_node_t *last_child;
    dom_node_t *next_sibling;
    dom_node_t *prev_sibling;
    int child_count;

    // Computed style
    dom_style_t computed_style;
};

// CSS property (name/value pair)
typedef struct {
    char name[DOM_MAX_ATTR_NAME];
    char value[DOM_MAX_ATTR_VALUE];
} dom_css_prop_t;

// CSS rule
typedef struct {
    dom_css_selector_type_t selector_type;
    char selector_value[DOM_MAX_SELECTOR_LEN];   // tag name, class name (without .), or id (without #)
    dom_css_prop_t properties[DOM_MAX_CSS_PROPS];
    int property_count;
} dom_css_rule_t;

// DOM document
typedef struct {
    dom_node_t *root;                   // <html> node
    dom_node_t *head;                   // <head> node
    dom_node_t *body;                   // <body> node

    // CSS rules collected from <style> tags and dom_apply_css calls
    dom_css_rule_t css_rules[DOM_MAX_CSS_RULES];
    int css_rule_count;

    // Collected script text from <script> tags
    char scripts[DOM_MAX_SCRIPTS][DOM_MAX_SCRIPT_LEN];
    int script_count;

    // Collected raw CSS text from <style> tags
    char stylesheets[DOM_MAX_STYLESHEETS][DOM_MAX_STYLESHEET_LEN];
    int stylesheet_count;

    // Base URL for resolving relative URLs
    char base_url[DOM_MAX_URL_LEN];

    // Node pool (static allocation)
    dom_node_t node_pool[DOM_MAX_NODES];
    int node_count;                     // Number of nodes in use

    // Layout state
    int viewport_w;
    int viewport_h;
    int total_height;                   // Total document height after layout
} dom_document_t;

// ============================================================================
// DOCUMENT LIFECYCLE
// ============================================================================

// Create a new DOM document (allocates from static pool, zeroed)
dom_document_t* dom_document_create(void);

// Destroy a DOM document and free all resources
void dom_document_destroy(dom_document_t *doc);

// ============================================================================
// HTML PARSING
// ============================================================================

// Parse an HTML string into the document's DOM tree.
// Extracts <style> content into stylesheets, <script> content into scripts.
// Returns 0 on success, -1 on error.
int dom_parse_html(dom_document_t *doc, const char *html_body);

// ============================================================================
// CSS OPERATIONS
// ============================================================================

// Parse and apply CSS text to matching elements in the document.
// Supports tag selectors, .class selectors, and #id selectors.
// Returns the number of rules parsed, or -1 on error.
int dom_apply_css(dom_document_t *doc, const char *css_text);

// Apply all collected stylesheets to the DOM tree.
// Call after dom_parse_html() to process inline <style> tags.
void dom_apply_all_stylesheets(dom_document_t *doc);

// ============================================================================
// LAYOUT
// ============================================================================

// Compute layout for the entire document.
// Uses simple block layout: block elements stack vertically,
// inline elements flow horizontally.
// viewport_w/h define the available rendering area.
void dom_compute_styles(dom_document_t *doc, int viewport_w, int viewport_h);

// ============================================================================
// RENDERING
// ============================================================================

// Render the DOM tree to a graphics buffer.
// buffer: pointer to the pixel buffer (32-bit ARGB)
// x, y, w, h: rendering viewport within the buffer
// scroll_offset: vertical scroll offset in pixels
void dom_render(dom_document_t *doc, uint32_t *buffer,
                int x, int y, int w, int h, int scroll_offset);

// ============================================================================
// DOM QUERIES
// ============================================================================

// Get concatenated script text for JS execution.
// Returns pointer to a static buffer containing all scripts joined with \n.
// Caller should not free the returned pointer.
const char* dom_get_scripts(dom_document_t *doc);

// Find an element by its id attribute.
// Returns the node pointer, or NULL if not found.
dom_node_t* dom_get_element_by_id(dom_document_t *doc, const char *id);

// Simple query selector supporting:
//   "tagname"  - matches by element tag name
//   ".class"   - matches by class name
//   "#id"      - matches by id
// Returns the first matching element, or NULL.
dom_node_t* dom_query_selector(dom_document_t *doc, const char *selector);

// ============================================================================
// NODE OPERATIONS
// ============================================================================

// Allocate a node from the document pool.
// Returns NULL if the pool is exhausted.
dom_node_t* dom_node_alloc(dom_document_t *doc);

// Append a child node to a parent node.
void dom_node_append_child(dom_node_t *parent, dom_node_t *child);

// Set an attribute on an element node.
// Returns 0 on success, -1 if the node is not an element or attrs are full.
int dom_node_set_attr(dom_node_t *node, const char *name, const char *value);

// Get an attribute value from an element node.
// Returns the attribute value string, or NULL if not found.
const char* dom_node_get_attr(dom_node_t *node, const char *name);

// Get the text content of an element (recursive, concatenates child text nodes).
// Writes into buf (max buf_size bytes). Returns bytes written.
int dom_node_get_text_content(dom_node_t *node, char *buf, int buf_size);

// ============================================================================
// STYLE DEFAULTS
// ============================================================================

// Initialize a dom_style_t with default values.
void dom_style_init_defaults(dom_style_t *style);

// ============================================================================
// COLOR PARSING
// ============================================================================

// Parse a CSS color string into a 32-bit ARGB value.
// Supports: #RGB, #RRGGBB, rgb(r,g,b), rgba(r,g,b,a), named colors.
// Returns 0 on success, -1 on failure. Result stored in *out_color.
int dom_parse_color(const char *str, uint32_t *out_color);

// ============================================================================
// DEBUG
// ============================================================================

// Print the DOM tree structure to the serial console (for debugging).
void dom_debug_print_tree(dom_document_t *doc);

// ============================================================================
// JS BRIDGE API - High-level convenience functions
// Used by browser_js_bridge.c for JavaScript DOM manipulation
// ============================================================================

// Get the global document singleton (created on first call)
dom_document_t* dom_get_document(void);

// Set the bridge document to the browser's current DOM document
// Must be called when a new page loads so JS bridge queries use the correct doc
void dom_set_bridge_document(dom_document_t* doc);

// Create an element node in the global document
dom_node_t* dom_create_element(const char* tag_name);

// Create a text node in the global document
dom_node_t* dom_create_text_node(const char* text);

// Append a child node to a parent element
void dom_append_child(dom_node_t* parent, dom_node_t* child);

// Set an attribute on a DOM element
void dom_set_attribute(dom_node_t* node, const char* name, const char* value);

// Set innerHTML on a node (replaces children with content)
void dom_set_inner_html(dom_node_t* node, const char* html);

#endif // BROWSER_DOM_H
