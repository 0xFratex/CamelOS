// usr/libs/browser_bridge.h - Browser-JS Bridge Header
// Interface between JavaScript Engine V2 and Browser DOM

#ifndef BROWSER_BRIDGE_H
#define BROWSER_BRIDGE_H

#include <types.h>
#include "js_engine_v2.h"

// ============================================================================// DOM TYPES (shared with browser_cdl.c)
// ============================================================================

#define MAX_URL 256

typedef enum {
    DOM_DOCUMENT,
    DOM_ELEMENT,
    DOM_TEXT,
    DOM_COMMENT
} dom_node_type_t;

typedef enum {
    ELEM_UNKNOWN = 0,
    ELEM_HTML, ELEM_HEAD, ELEM_BODY, ELEM_TITLE,
    ELEM_DIV, ELEM_SPAN, ELEM_P, ELEM_BR, ELEM_HR,
    ELEM_H1, ELEM_H2, ELEM_H3, ELEM_H4, ELEM_H5, ELEM_H6,
    ELEM_A, ELEM_IMG, ELEM_UL, ELEM_OL, ELEM_LI,
    ELEM_TABLE, ELEM_TR, ELEM_TD, ELEM_TH, ELEM_THEAD, ELEM_TBODY,
    ELEM_FORM, ELEM_INPUT, ELEM_BUTTON, ELEM_TEXTAREA, ELEM_LABEL, ELEM_SELECT, ELEM_OPTION,
    ELEM_STRONG, ELEM_B, ELEM_EM, ELEM_I, ELEM_U,
    ELEM_CODE, ELEM_PRE, ELEM_BLOCKQUOTE,
    ELEM_SCRIPT, ELEM_STYLE, ELEM_META, ELEM_LINK,
    ELEM_HEADER, ELEM_FOOTER, ELEM_NAV, ELEM_MAIN, ELEM_SECTION, ELEM_ARTICLE,
    ELEM_ASIDE, ELEM_FIGURE, ELEM_FIGCAPTION, ELEM_DETAILS, ELEM_SUMMARY,
    ELEM_VIDEO, ELEM_AUDIO, ELEM_CANVAS, ELEM_IFRAME, ELEM_PICTURE, ELEM_SOURCE,
    ELEM_EMBED, ELEM_OBJECT, ELEM_PARAM, ELEM_TRACK, ELEM_SVG, ELEM_MATH,
    ELEM_DIALOG, ELEM_MENU, ELEM_MENUITEM, ELEM_PROGRESS, ELEM_METER,
    ELEM_TIME, ELEM_MARK, ELEM_RUBY, ELEM_RT, ELEM_RP, ELEM_BDI, ELEM_BDO,
    ELEM_WBR, ELEM_DATA, ELEM_OUTPUT, ELEM_DATALIST, ELEM_KEYGEN
} element_type_t;

// CSS Style structure
typedef struct {
    uint32_t fg_color;
    uint32_t bg_color;
    int font_size;
    int font_weight;
    int font_style;
    int text_decoration;
    int text_align;
    int display;
    int margin_top;
    int margin_bottom;
    int margin_left;
    int margin_right;
    int padding_top;
    int padding_bottom;
    int padding_left;
    int padding_right;
    int border_radius;
    int is_link;
    int target_blank;
    int flex_direction;
    int justify_content;
    int align_items;
    int align_self;
    int flex_wrap;
    int flex_grow;
    int flex_shrink;
    int flex_basis;
    int gap;
    int width;
    int height;
    int min_width;
    int max_width;
    uint32_t border_color;
    int border_width;
    int border_style;
    int border_top;
    int border_right;
    int border_bottom;
    int border_left;
    int line_height;
    int overflow;
    int visibility;
    int position;
    int z_index;
    int box_shadow_x;
    int box_shadow_y;
    int box_shadow_blur;
    uint32_t box_shadow_color;
} css_style_t;

// Full DOM node structure
typedef struct dom_node {
    dom_node_type_t type;
    element_type_t elem_type;
    char tag_name[32];
    char* text_content;
    int text_len;
    
    // Attributes
    char href[MAX_URL];
    char src[MAX_URL];
    char alt[128];
    char id[64];
    char class_name[64];
    char target[16];
    char style_attr[256];
    char type_attr[32];
    
    // Style
    css_style_t style;
    
    // Tree structure
    struct dom_node* parent;
    struct dom_node* first_child;
    struct dom_node* last_child;
    struct dom_node* next_sibling;
    
    // Layout info
    int x, y;
    int width, height;
    int layout_computed;
} dom_node_t;

// ============================================================================
// BRIDGE INITIALIZATION
// ============================================================================

// Initialize the bridge between JS engine and browser DOM
// Must be called after JS engine is initialized and DOM is ready
void browser_js_bridge_init(js_v2_engine_t* engine);

// Set the document root for DOM queries
void browser_set_document_root(void* doc_root);

// Set the current URL for resolving relative URLs
void browser_set_current_url(const char* url);

// ============================================================================
// DOM OPERATIONS (called from JS)
// ============================================================================

// Find element by ID
void* browser_dom_getElementById(const char* id);

// Query selector (simple: #id, .class, tag)
void* browser_dom_querySelector(const char* selector);

// Query selector all
int browser_dom_querySelectorAll(const char* selector, void** results, int max_results);

// Create element
void* browser_dom_createElement(const char* tag_name);

// Create text node
void* browser_dom_createTextNode(const char* text);

// Get/set element attribute
const char* browser_dom_getAttribute(void* element, const char* attr);
void browser_dom_setAttribute(void* element, const char* attr, const char* value);

// Get/set element property
const char* browser_dom_getProperty(void* element, const char* prop);
void browser_dom_setProperty(void* element, const char* prop, const char* value);

// Get/set innerHTML
const char* browser_dom_getInnerHTML(void* element);
void browser_dom_setInnerHTML(void* element, const char* html);

// Get/set textContent
const char* browser_dom_getTextContent(void* element);
void browser_dom_setTextContent(void* element, const char* text);

// Append child
void browser_dom_appendChild(void* parent, void* child);

// Remove child
void browser_dom_removeChild(void* parent, void* child);

// Add event listener
void browser_dom_addEventListener(void* element, const char* event, void* handler);

// ============================================================================
// NETWORK OPERATIONS (called from JS)
// ============================================================================

// Fetch URL and return response
int browser_fetch(const char* url, const char* method, const char* body,
                  char* response, int response_size);

// Get response headers
const char* browser_fetch_get_header(const char* name);

// ============================================================================
// EXTERNAL RESOURCE LOADING
// ============================================================================

// Load external CSS file
char* browser_load_css(const char* url);

// Load external JS file  
char* browser_load_js(const char* url);

// Process link tags in HTML (loads external CSS)
void browser_process_link_tags(const char* html);

// Process script tags in HTML (loads external JS, collects inline scripts)
void browser_process_script_tags(const char* html, char* inline_scripts, int max_inline_len);

// ============================================================================
// CSS PROCESSING
// ============================================================================

// Apply CSS rules to DOM
void browser_apply_css_rules(const char* css_content);

// ============================================================================
// CACHE MANAGEMENT
// ============================================================================

// Clear resource cache
void browser_clear_resource_cache(void);

// Get cache statistics
int browser_get_cache_size(void);
int browser_get_cache_count(void);

// ============================================================================
// JS ENGINE INTEGRATION
// ============================================================================

// Execute JavaScript in browser context
int browser_execute_js(js_v2_engine_t* engine, const char* script);

// Execute JavaScript and update DOM
int browser_execute_js_with_dom_updates(js_v2_engine_t* engine, const char* script);

// Register browser-specific JS functions
void browser_register_js_globals(js_v2_engine_t* engine);

#endif // BROWSER_BRIDGE_H
