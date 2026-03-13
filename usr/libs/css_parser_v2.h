// usr/libs/css_parser_v2.h - Modern CSS Parser with Flexbox and Grid Support
// Version 2.0 - Full CSS3 compatibility for modern websites

#ifndef CSS_PARSER_V2_H
#define CSS_PARSER_V2_H

#include <types.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
#define CSS_MAX_SELECTORS      512
#define CSS_MAX_PROPERTIES     64
#define CSS_MAX_RULES          256
#define CSS_MAX_VALUE_LEN      256
#define CSS_MAX_MEDIA_QUERIES  32
#define CSS_MAX_KEYFRAMES      64
#define CSS_MAX_FONTS          32

// ============================================================================
// CSS VALUE TYPES
// ============================================================================
typedef enum {
    CSS_VALUE_NONE = 0,
    CSS_VALUE_NUMBER,
    CSS_VALUE_PERCENTAGE,
    CSS_VALUE_PX,
    CSS_VALUE_EM,
    CSS_VALUE_REM,
    CSS_VALUE_VW,
    CSS_VALUE_VH,
    CSS_VALUE_VMIN,
    CSS_VALUE_VMAX,
    CSS_VALUE_CM,
    CSS_VALUE_MM,
    CSS_VALUE_IN,
    CSS_VALUE_PT,
    CSS_VALUE_PC,
    CSS_VALUE_COLOR_HEX,
    CSS_VALUE_COLOR_RGB,
    CSS_VALUE_COLOR_RGBA,
    CSS_VALUE_COLOR_HSL,
    CSS_VALUE_COLOR_HSLA,
    CSS_VALUE_COLOR_NAME,
    CSS_VALUE_STRING,
    CSS_VALUE_URL,
    CSS_VALUE_KEYWORD,
    CSS_VALUE_FUNCTION,
    CSS_VALUE_CALC,
    CSS_VALUE_VAR,
    CSS_VALUE_GRADIENT,
    CSS_VALUE_SHADOW,
    CSS_VALUE_TRANSFORM,
    CSS_VALUE_LIST          // Comma or space separated list
} css_value_type_t;

// ============================================================================
// CSS PROPERTY CATEGORIES
// ============================================================================
typedef enum {
    CSS_CAT_LAYOUT = 0,
    CSS_CAT_FLEXBOX,
    CSS_CAT_GRID,
    CSS_CAT_BOX_MODEL,
    CSS_CAT_TYPOGRAPHY,
    CSS_CAT_BACKGROUND,
    CSS_CAT_BORDER,
    CSS_CAT_EFFECTS,
    CSS_CAT_TRANSFORM,
    CSS_CAT_ANIMATION,
    CSS_CAT_TRANSITION,
    CSS_CAT_POSITIONING
} css_property_category_t;

// ============================================================================
// CSS VALUE STRUCTURE
// ============================================================================
typedef struct css_value css_value_t;

struct css_value {
    css_value_type_t type;
    union {
        double number;
        double percentage;
        char string[CSS_MAX_VALUE_LEN];
        struct {
            uint8_t r, g, b, a;
        } color;
        struct {
            css_value_t* items;
            int count;
            char separator;     // ' ' or ','
        } list;
        struct {
            char name[64];
            css_value_t* args;
            int arg_count;
        } function;
        struct {
            char var_name[64];
            css_value_t* fallback;
        } var_ref;
        struct {
            char type[16];      // linear, radial, conic
            css_value_t* stops;
            int stop_count;
            double angle;
            double x1, y1, x2, y2;
        } gradient;
        struct {
            double x, y, blur, spread;
            uint32_t color;
            int inset;
        } shadow;
        struct {
            char func[32];      // translate, rotate, scale, skew, matrix
            double values[6];
            int value_count;
        } transform;
    } data;
    css_value_t* next;          // For linked lists
};

// ============================================================================
// DISPLAY TYPES
// ============================================================================
typedef enum {
    CSS_DISPLAY_NONE = 0,
    CSS_DISPLAY_BLOCK,
    CSS_DISPLAY_INLINE,
    CSS_DISPLAY_INLINE_BLOCK,
    CSS_DISPLAY_FLEX,
    CSS_DISPLAY_INLINE_FLEX,
    CSS_DISPLAY_GRID,
    CSS_DISPLAY_INLINE_GRID,
    CSS_DISPLAY_TABLE,
    CSS_DISPLAY_TABLE_ROW,
    CSS_DISPLAY_TABLE_CELL,
    CSS_DISPLAY_TABLE_COLUMN,
    CSS_DISPLAY_TABLE_HEADER_GROUP,
    CSS_DISPLAY_TABLE_ROW_GROUP,
    CSS_DISPLAY_TABLE_FOOTER_GROUP,
    CSS_DISPLAY_LIST_ITEM,
    CSS_DISPLAY_RUN_IN,
    CSS_DISPLAY_CONTENTS,
    CSS_DISPLAY_FLOW_ROOT
} css_display_t;

// ============================================================================
// FLEXBOX PROPERTIES
// ============================================================================
typedef enum {
    CSS_FLEX_DIR_ROW = 0,
    CSS_FLEX_DIR_ROW_REVERSE,
    CSS_FLEX_DIR_COLUMN,
    CSS_FLEX_DIR_COLUMN_REVERSE
} css_flex_direction_t;

typedef enum {
    CSS_FLEX_WRAP_NOWRAP = 0,
    CSS_FLEX_WRAP_WRAP,
    CSS_FLEX_WRAP_WRAP_REVERSE
} css_flex_wrap_t;

typedef enum {
    CSS_JUSTIFY_FLEX_START = 0,
    CSS_JUSTIFY_FLEX_END,
    CSS_JUSTIFY_CENTER,
    CSS_JUSTIFY_SPACE_BETWEEN,
    CSS_JUSTIFY_SPACE_AROUND,
    CSS_JUSTIFY_SPACE_EVENLY
} css_justify_content_t;

typedef enum {
    CSS_ALIGN_AUTO = 0,
    CSS_ALIGN_FLEX_START,
    CSS_ALIGN_FLEX_END,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_BASELINE,
    CSS_ALIGN_STRETCH
} css_align_t;

typedef struct {
    css_flex_direction_t direction;
    css_flex_wrap_t wrap;
    css_justify_content_t justify_content;
    css_align_t align_items;
    css_align_t align_content;
    double gap;
    double row_gap;
    double column_gap;
    
    // Item properties
    double grow;
    double shrink;
    css_value_t* basis;
    css_align_t align_self;
    int order;
} css_flexbox_t;

// ============================================================================
// GRID PROPERTIES
// ============================================================================
typedef struct {
    char* tracks;           // Grid track definitions
    int track_count;
    double* sizes;          // Computed sizes
    char* areas;            // Named grid areas
    double gap;
    double row_gap;
    double column_gap;
    css_justify_content_t justify_items;
    css_justify_content_t justify_content;
    css_align_t align_items;
    css_align_t align_content;
    
    // Item properties
    int column_start;
    int column_end;
    int row_start;
    int row_end;
    char* area_name;
    css_justify_content_t justify_self;
    css_align_t align_self;
    int order;
} css_grid_t;

// ============================================================================
// COMPLEX BACKGROUND
// ============================================================================
typedef struct {
    css_value_t* color;
    css_value_t* image;
    css_value_t* position_x;
    css_value_t* position_y;
    css_value_t* size_x;
    css_value_t* size_y;
    char repeat[16];
    char attachment[16];
    char clip[16];
    char origin[16];
} css_background_t;

// ============================================================================
// BORDER PROPERTIES
// ============================================================================
typedef struct {
    double width;
    char style[16];         // solid, dashed, dotted, double, groove, ridge, inset, outset, none
    uint32_t color;
    double radius[4];       // TL, TR, BR, BL
    double radius_x[4];
    double radius_y[4];
} css_border_side_t;

typedef struct {
    css_border_side_t top;
    css_border_side_t right;
    css_border_side_t bottom;
    css_border_side_t left;
    css_value_t* image;
    double image_slice[4];
    double image_width[4];
    double image_outset[4];
    char image_repeat[16];
} css_border_t;

// ============================================================================
// TRANSFORM AND ANIMATION
// ============================================================================
typedef struct {
    char func[32];
    double values[6];
} css_transform_item_t;

typedef struct {
    css_transform_item_t* items;
    int count;
    char origin_x[32];
    char origin_y[32];
    char origin_z[32];
    char style[16];         // flat, preserve-3d
    double perspective;
    char perspective_origin[64];
    char backface_visibility[16];
} css_transform_t;

typedef struct {
    char name[64];
    double duration;
    double delay;
    char timing_function[64];
    int iteration_count;    // -1 for infinite
    char direction[16];
    char fill_mode[16];
    char play_state[16];
} css_animation_t;

typedef struct {
    char property[64];
    double duration;
    double delay;
    char timing_function[64];
} css_transition_t;

// ============================================================================
// FILTER EFFECTS
// ============================================================================
typedef struct {
    double blur;
    double brightness;
    double contrast;
    double grayscale;
    double hue_rotate;
    double invert;
    double opacity;
    double saturate;
    double sepia;
    char drop_shadow[64];
} css_filter_t;

// ============================================================================
// COMPLETE STYLE STRUCTURE
// ============================================================================
typedef struct {
    // Display and visibility
    css_display_t display;
    char visibility[16];
    char overflow[16];
    char overflow_x[16];
    char overflow_y[16];
    float opacity;
    int z_index;
    
    // Box model
    css_value_t* width;
    css_value_t* height;
    css_value_t* min_width;
    css_value_t* min_height;
    css_value_t* max_width;
    css_value_t* max_height;
    css_value_t* margin[4];
    css_value_t* padding[4];
    css_value_t* box_sizing;
    
    // Positioning
    char position[16];      // static, relative, absolute, fixed, sticky
    css_value_t* top;
    css_value_t* right;
    css_value_t* bottom;
    css_value_t* left;
    float inset[4];
    
    // Flexbox
    css_flexbox_t flex;
    
    // Grid
    css_grid_t grid;
    
    // Typography
    css_value_t* font_size;
    char font_family[256];
    char font_weight[16];
    char font_style[16];
    char font_stretch[16];
    char font_variant[16];
    css_value_t* line_height;
    css_value_t* letter_spacing;
    css_value_t* word_spacing;
    char text_align[16];
    char text_decoration[16];
    css_value_t* text_indent;
    char text_transform[16];
    char white_space[16];
    char word_break[16];
    char word_wrap[16];
    char direction[16];
    char unicode_bidi[16];
    css_value_t* text_shadow;
    css_value_t* tab_size;
    char writing_mode[16];
    
    // Colors
    uint32_t color;
    uint32_t background_color;
    css_background_t background;
    
    // Borders
    css_border_t border;
    css_value_t* outline;
    char outline_style[16];
    css_value_t* outline_width;
    uint32_t outline_color;
    css_value_t* outline_offset;
    
    // Transform
    css_transform_t transform;
    
    // Animation & Transition
    css_animation_t* animations;
    int animation_count;
    css_transition_t* transitions;
    int transition_count;
    
    // Effects
    css_filter_t filter;
    css_value_t* backdrop_filter;
    css_value_t* mix_blend_mode;
    css_value_t* isolation;
    
    // List
    char list_style_type[32];
    char list_style_position[16];
    css_value_t* list_style_image;
    
    // Table
    char border_collapse[16];
    css_value_t* border_spacing;
    char empty_cells[16];
    char caption_side[16];
    char table_layout[16];
    
    // User Interface
    css_value_t* cursor;
    char resize[16];
    char user_select[16];
    char pointer_events[16];
    
    // Content
    css_value_t* content;
    css_value_t* quotes;
    css_value_t* counter_reset;
    css_value_t* counter_increment;
    
    // Variables (Custom Properties)
    struct {
        char name[64];
        css_value_t* value;
    } variables[32];
    int variable_count;
    
    // Computed values (for rendering)
    double computed_width;
    double computed_height;
    double computed_x;
    double computed_y;
    double computed_margin[4];
    double computed_padding[4];
    double computed_border_width[4];
    
    // Flags
    uint32_t flags;
    
} css_computed_style_t;

// ============================================================================
// SELECTOR STRUCTURE
// ============================================================================
typedef enum {
    CSS_SEL_TYPE = 0,          // div, span
    CSS_SEL_CLASS,             // .class
    CSS_SEL_ID,                // #id
    CSS_SEL_UNIVERSAL,         // *
    CSS_SEL_ATTRIBUTE,         // [attr=value]
    CSS_SEL_PSEUDO_CLASS,      // :hover, :nth-child()
    CSS_SEL_PSEUDO_ELEMENT     // ::before, ::after
} css_selector_type_t;

typedef struct {
    css_selector_type_t type;
    char value[64];
    char attribute[32];        // For attribute selectors
    char operator[4];          // =, ~=, |=, ^=, $=, *=
    char pseudo_arg[64];       // For pseudo-class args like nth-child(2)
    int specificity;           // Calculated specificity
} css_selector_part_t;

typedef struct {
    css_selector_part_t* parts;
    int part_count;
    char combinator;           // ' ' (descendant), '>' (child), '+' (adjacent), '~' (sibling)
    int specificity;
} css_selector_t;

// ============================================================================
// CSS RULE
// ============================================================================
typedef struct {
    css_selector_t* selectors;
    int selector_count;
    css_computed_style_t style;
    uint32_t source_line;
    char* source_file;
} css_rule_t;

// ============================================================================
// MEDIA QUERY
// ============================================================================
typedef struct {
    char media_type[32];       // screen, print, all
    struct {
        char property[32];
        char operator[4];      // min, max
        css_value_t* value;
    } conditions[8];
    int condition_count;
    css_rule_t* rules;
    int rule_count;
    int matches;               // Runtime evaluation
} css_media_query_t;

// ============================================================================
// KEYFRAMES
// ============================================================================
typedef struct {
    char name[64];
    struct {
        int percentage;
        css_computed_style_t style;
    } keyframes[20];
    int keyframe_count;
} css_keyframes_t;

// ============================================================================
// FONT FACE
// ============================================================================
typedef struct {
    char font_family[128];
    char src[256];
    char font_style[16];
    char font_weight[16];
    char font_stretch[16];
    char unicode_range[64];
} css_font_face_t;

// ============================================================================
// CSS STYLESHEET
// ============================================================================
typedef struct {
    css_rule_t* rules;
    int rule_count;
    css_media_query_t* media_queries;
    int media_query_count;
    css_keyframes_t* keyframes;
    int keyframe_count;
    css_font_face_t* fonts;
    int font_count;
    char* source_url;
    uint32_t parse_time;
} css_stylesheet_t;

// ============================================================================
// PARSER CONTEXT
// ============================================================================
typedef struct {
    const char* input;
    int input_len;
    int pos;
    int line;
    int column;
    char error[256];
    int has_error;
    
    // Parsed output
    css_stylesheet_t stylesheet;
    
    // State
    int in_at_rule;
    char current_at_rule[32];
    
} css_parser_t;

// ============================================================================
// PARSER API
// ============================================================================

// Initialize parser
void css_parser_init(css_parser_t* parser, const char* input);

// Parse complete stylesheet
int css_parse_stylesheet(css_parser_t* parser);

// Parse inline style
int css_parse_inline_style(css_parser_t* parser, css_computed_style_t* style);

// Get error message
const char* css_get_error(css_parser_t* parser);

// Free stylesheet resources
void css_stylesheet_free(css_stylesheet_t* stylesheet);

// ============================================================================
// STYLE COMPUTATION API
// ============================================================================

// Apply a rule to a computed style (cascade)
void css_apply_rule(css_computed_style_t* style, css_rule_t* rule, int importance);

// Compute final style for an element
void css_compute_style(css_computed_style_t* style, css_stylesheet_t* stylesheet,
                       const char* tag_name, const char* id, const char** classes, int class_count,
                       const char** attributes, int attr_count);

// Resolve CSS variables
css_value_t* css_resolve_var(css_computed_style_t* style, const char* var_name);

// Compute relative units (em, rem, vh, vw, etc.)
double css_compute_length(css_value_t* value, css_computed_style_t* parent_style, 
                          double viewport_width, double viewport_height, double root_font_size);

// ============================================================================
// FLEXBOX LAYOUT ENGINE
// ============================================================================

typedef struct css_layout_node css_layout_node_t;

struct css_layout_node {
    css_computed_style_t* style;
    css_layout_node_t* parent;
    css_layout_node_t* children;
    css_layout_node_t* next_sibling;
    int child_count;
    
    // Layout results
    double x, y;
    double width, height;
    double content_width, content_height;
    double baseline;
    
    // For flexbox
    double flex_base_size;
    double hypothetical_size;
    double main_size;
    double cross_size;
    double flexed_main_size;
    int line_index;
    int line_position;
};

// Perform flexbox layout
void css_layout_flexbox(css_layout_node_t* container, double available_width, double available_height);

// ============================================================================
// GRID LAYOUT ENGINE
// ============================================================================

void css_layout_grid(css_layout_node_t* container, double available_width, double available_height);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Parse color value
int css_parse_color(const char* value, uint32_t* color);

// Parse gradient
int css_parse_gradient(const char* value, css_value_t* gradient);

// Calculate specificity
int css_calc_specificity(css_selector_t* selector);

// Match selector against element
int css_match_selector(css_selector_t* selector, const char* tag_name, const char* id,
                       const char** classes, int class_count);

// Clone computed style
void css_clone_style(css_computed_style_t* dest, css_computed_style_t* src);

// Initialize default style
void css_default_style(css_computed_style_t* style);

// Compare styles for transition detection
int css_style_diff(css_computed_style_t* a, css_computed_style_t* b);

#endif // CSS_PARSER_V2_H
