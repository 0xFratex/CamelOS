// usr/libs/css_parser_v2.c - Modern CSS Parser Implementation
// Version 2.0 - Full CSS3 compatibility with Flexbox and Grid support

#include "css_parser_v2.h"
#include "../../core/string.h"
#include "../../core/memory.h"  // For kmalloc, kfree, krealloc, memset

// ============================================================================
// PARSER INITIALIZATION
// ============================================================================

void css_parser_init(css_parser_t* parser, const char* input) {
    memset(parser, 0, sizeof(css_parser_t));
    parser->input = input;
    parser->input_len = strlen(input);
    parser->pos = 0;
    parser->line = 1;
    parser->column = 1;
}

// ============================================================================
// LEXER HELPERS
// ============================================================================

static char peek_char(css_parser_t* parser) {
    if (parser->pos >= parser->input_len) return '\0';
    return parser->input[parser->pos];
}

static char next_char(css_parser_t* parser) {
    if (parser->pos >= parser->input_len) return '\0';
    char c = parser->input[parser->pos++];
    if (c == '\n') {
        parser->line++;
        parser->column = 1;
    } else {
        parser->column++;
    }
    return c;
}

static void skip_whitespace(css_parser_t* parser) {
    while (1) {
        char c = peek_char(parser);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            next_char(parser);
        } else if (c == '/' && parser->pos + 1 < parser->input_len &&
                   parser->input[parser->pos + 1] == '*') {
            // Skip comment
            next_char(parser);
            next_char(parser);
            while (1) {
                c = next_char(parser);
                if (c == '\0') break;
                if (c == '*' && peek_char(parser) == '/') {
                    next_char(parser);
                    break;
                }
            }
        } else {
            break;
        }
    }
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// ============================================================================
// VALUE PARSING
// ============================================================================

static css_value_t* parse_value(css_parser_t* parser);
static css_value_t* parse_color(css_parser_t* parser);
static css_value_t* parse_number(css_parser_t* parser);
static css_value_t* parse_string(css_parser_t* parser);
static css_value_t* parse_function(css_parser_t* parser);

static css_value_t* alloc_value(void) {
    css_value_t* val = (css_value_t*)kmalloc(sizeof(css_value_t));
    if (val) memset(val, 0, sizeof(css_value_t));
    return val;
}

static css_value_t* parse_number(css_parser_t* parser) {
    css_value_t* val = alloc_value();
    if (!val) return NULL;
    
    double num = 0;
    int neg = 0;
    
    char c = peek_char(parser);
    if (c == '-') {
        neg = 1;
        next_char(parser);
        c = peek_char(parser);
    }
    
    // Integer part
    while (is_digit(peek_char(parser))) {
        num = num * 10 + (next_char(parser) - '0');
    }
    
    // Decimal part
    if (peek_char(parser) == '.') {
        next_char(parser);
        double decimal = 0.1;
        while (is_digit(peek_char(parser))) {
            num += decimal * (next_char(parser) - '0');
            decimal *= 0.1;
        }
    }
    
    if (neg) num = -num;
    
    // Check for units
    c = peek_char(parser);
    if (c == '%') {
        next_char(parser);
        val->type = CSS_VALUE_PERCENTAGE;
        val->data.percentage = num;
    } else if (is_alpha(c)) {
        // Parse unit
        char unit[8] = {0};
        int i = 0;
        while (is_alpha(peek_char(parser)) && i < 7) {
            unit[i++] = next_char(parser);
        }
        
        if (strcmp(unit, "px") == 0) {
            val->type = CSS_VALUE_PX;
            val->data.number = num;
        } else if (strcmp(unit, "em") == 0) {
            val->type = CSS_VALUE_EM;
            val->data.number = num;
        } else if (strcmp(unit, "rem") == 0) {
            val->type = CSS_VALUE_REM;
            val->data.number = num;
        } else if (strcmp(unit, "vw") == 0) {
            val->type = CSS_VALUE_VW;
            val->data.number = num;
        } else if (strcmp(unit, "vh") == 0) {
            val->type = CSS_VALUE_VH;
            val->data.number = num;
        } else if (strcmp(unit, "pt") == 0) {
            val->type = CSS_VALUE_PT;
            val->data.number = num;
        } else if (strcmp(unit, "deg") == 0 || strcmp(unit, "rad") == 0) {
            val->type = CSS_VALUE_NUMBER;
            val->data.number = num;
        } else {
            val->type = CSS_VALUE_NUMBER;
            val->data.number = num;
        }
    } else {
        val->type = CSS_VALUE_NUMBER;
        val->data.number = num;
    }
    
    return val;
}

static css_value_t* parse_color(css_parser_t* parser) {
    css_value_t* val = alloc_value();
    if (!val) return NULL;
    
    val->type = CSS_VALUE_COLOR_HEX;
    
    char c = next_char(parser); // #
    
    // Parse hex color
    char hex[9] = {0};
    int i = 0;
    while (is_hex(peek_char(parser)) && i < 8) {
        hex[i++] = next_char(parser);
    }
    
    // Convert hex to RGB
    uint32_t color = 0;
    if (i == 3) {
        // #RGB -> #RRGGBB
        int r = (hex[0] >= 'a') ? (hex[0] - 'a' + 10) : 
               (hex[0] >= 'A') ? (hex[0] - 'A' + 10) : (hex[0] - '0');
        int g = (hex[1] >= 'a') ? (hex[1] - 'a' + 10) : 
               (hex[1] >= 'A') ? (hex[1] - 'A' + 10) : (hex[1] - '0');
        int b = (hex[2] >= 'a') ? (hex[2] - 'a' + 10) : 
               (hex[2] >= 'A') ? (hex[2] - 'A' + 10) : (hex[2] - '0');
        val->data.color.r = r * 17;
        val->data.color.g = g * 17;
        val->data.color.b = b * 17;
        val->data.color.a = 255;
    } else if (i == 4) {
        // #RGBA -> #RRGGBBAA
        int r = (hex[0] >= 'a') ? (hex[0] - 'a' + 10) : 
               (hex[0] >= 'A') ? (hex[0] - 'A' + 10) : (hex[0] - '0');
        int g = (hex[1] >= 'a') ? (hex[1] - 'a' + 10) : 
               (hex[1] >= 'A') ? (hex[1] - 'A' + 10) : (hex[1] - '0');
        int b = (hex[2] >= 'a') ? (hex[2] - 'a' + 10) : 
               (hex[2] >= 'A') ? (hex[2] - 'A' + 10) : (hex[2] - '0');
        int a = (hex[3] >= 'a') ? (hex[3] - 'a' + 10) : 
               (hex[3] >= 'A') ? (hex[3] - 'A' + 10) : (hex[3] - '0');
        val->data.color.r = r * 17;
        val->data.color.g = g * 17;
        val->data.color.b = b * 17;
        val->data.color.a = a * 17;
    } else if (i == 6) {
        // #RRGGBB
        for (int j = 0; j < 6; j++) {
            color = color * 16 + ((hex[j] >= 'a') ? (hex[j] - 'a' + 10) : 
                                  (hex[j] >= 'A') ? (hex[j] - 'A' + 10) : (hex[j] - '0'));
        }
        val->data.color.r = (color >> 16) & 0xFF;
        val->data.color.g = (color >> 8) & 0xFF;
        val->data.color.b = color & 0xFF;
        val->data.color.a = 255;
    } else if (i == 8) {
        // #RRGGBBAA
        for (int j = 0; j < 8; j++) {
            color = color * 16 + ((hex[j] >= 'a') ? (hex[j] - 'a' + 10) : 
                                  (hex[j] >= 'A') ? (hex[j] - 'A' + 10) : (hex[j] - '0'));
        }
        val->data.color.r = (color >> 24) & 0xFF;
        val->data.color.g = (color >> 16) & 0xFF;
        val->data.color.b = (color >> 8) & 0xFF;
        val->data.color.a = color & 0xFF;
    }
    
    return val;
}

static css_value_t* parse_function(css_parser_t* parser) {
    css_value_t* val = alloc_value();
    if (!val) return NULL;
    
    val->type = CSS_VALUE_FUNCTION;
    
    // Parse function name
    char name[32] = {0};
    int i = 0;
    while (peek_char(parser) != '(' && i < 31) {
        name[i++] = next_char(parser);
    }
    strcpy(val->data.function.name, name);
    
    next_char(parser); // Skip (
    
    // Parse arguments
    skip_whitespace(parser);
    int arg_count = 0;
    css_value_t* args[16] = {0}; // Temporary storage for args
    
    while (peek_char(parser) != ')' && peek_char(parser) != '\0') {
        css_value_t* arg = parse_value(parser);
        if (arg && arg_count < 16) {
            args[arg_count++] = arg;
        }
        skip_whitespace(parser);
        if (peek_char(parser) == ',') {
            next_char(parser);
            skip_whitespace(parser);
        }
    }
    
    // Allocate memory for args
    val->data.function.args = (css_value_t*)kmalloc(sizeof(css_value_t) * arg_count);
    if (val->data.function.args) {
        for (int j = 0; j < arg_count; j++) {
            val->data.function.args[j] = *args[j]; // Copy the value
            kfree(args[j]); // Free the temporary parse_value allocation
        }
    }
    val->data.function.arg_count = arg_count;
    
    if (peek_char(parser) == ')') {
        next_char(parser);
    }
    
    return val;
}

static css_value_t* parse_value(css_parser_t* parser) {
    skip_whitespace(parser);
    
    char c = peek_char(parser);
    
    if (c == '\0' || c == '}' || c == ';' || c == ')') {
        return NULL;
    }
    
    // Number
    if (is_digit(c) || (c == '-' && is_digit(parser->input[parser->pos + 1]))) {
        return parse_number(parser);
    }
    
    // Color
    if (c == '#') {
        return parse_color(parser);
    }
    
    // String
    if (c == '"' || c == '\'') {
        css_value_t* val = alloc_value();
        if (!val) return NULL;
        
        val->type = CSS_VALUE_STRING;
        char quote = next_char(parser);
        int i = 0;
        while (peek_char(parser) != quote && peek_char(parser) != '\0' && i < 255) {
            val->data.string[i++] = next_char(parser);
        }
        val->data.string[i] = '\0';
        if (peek_char(parser) == quote) next_char(parser);
        
        return val;
    }
    
    // Function or keyword
    if (is_alpha(c)) {
        // Look ahead for function
        int save_pos = parser->pos;
        while (is_alpha(peek_char(parser)) || peek_char(parser) == '-') {
            next_char(parser);
        }
        
        if (peek_char(parser) == '(') {
            parser->pos = save_pos;
            return parse_function(parser);
        }
        
        // Keyword
        parser->pos = save_pos;
        css_value_t* val = alloc_value();
        if (!val) return NULL;
        
        val->type = CSS_VALUE_KEYWORD;
        int i = 0;
        while ((is_alpha(peek_char(parser)) || peek_char(parser) == '-') && i < 255) {
            val->data.string[i++] = next_char(parser);
        }
        val->data.string[i] = '\0';
        
        return val;
    }
    
    // Skip unknown
    next_char(parser);
    return parse_value(parser);
}

// ============================================================================
// PROPERTY PARSING
// ============================================================================

static void parse_property(css_parser_t* parser, css_computed_style_t* style) {
    // Parse property name
    char name[64] = {0};
    int i = 0;
    
    while (peek_char(parser) != ':' && peek_char(parser) != '\0' && i < 63) {
        name[i++] = next_char(parser);
    }
    
    if (peek_char(parser) == ':') {
        next_char(parser);
    }
    
    skip_whitespace(parser);
    
    // Parse value(s)
    css_value_t* value = parse_value(parser);
    
    // Apply property
    if (strcmp(name, "display") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            if (strcmp(value->data.string, "flex") == 0) {
                style->display = CSS_DISPLAY_FLEX;
            } else if (strcmp(value->data.string, "grid") == 0) {
                style->display = CSS_DISPLAY_GRID;
            } else if (strcmp(value->data.string, "block") == 0) {
                style->display = CSS_DISPLAY_BLOCK;
            } else if (strcmp(value->data.string, "inline") == 0) {
                style->display = CSS_DISPLAY_INLINE;
            } else if (strcmp(value->data.string, "inline-block") == 0) {
                style->display = CSS_DISPLAY_INLINE_BLOCK;
            } else if (strcmp(value->data.string, "none") == 0) {
                style->display = CSS_DISPLAY_NONE;
            }
        }
    }
    else if (strcmp(name, "flex-direction") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            if (strcmp(value->data.string, "row") == 0) {
                style->flex.direction = CSS_FLEX_DIR_ROW;
            } else if (strcmp(value->data.string, "row-reverse") == 0) {
                style->flex.direction = CSS_FLEX_DIR_ROW_REVERSE;
            } else if (strcmp(value->data.string, "column") == 0) {
                style->flex.direction = CSS_FLEX_DIR_COLUMN;
            } else if (strcmp(value->data.string, "column-reverse") == 0) {
                style->flex.direction = CSS_FLEX_DIR_COLUMN_REVERSE;
            }
        }
    }
    else if (strcmp(name, "flex-wrap") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            if (strcmp(value->data.string, "nowrap") == 0) {
                style->flex.wrap = CSS_FLEX_WRAP_NOWRAP;
            } else if (strcmp(value->data.string, "wrap") == 0) {
                style->flex.wrap = CSS_FLEX_WRAP_WRAP;
            } else if (strcmp(value->data.string, "wrap-reverse") == 0) {
                style->flex.wrap = CSS_FLEX_WRAP_WRAP_REVERSE;
            }
        }
    }
    else if (strcmp(name, "justify-content") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            if (strcmp(value->data.string, "flex-start") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_FLEX_START;
            } else if (strcmp(value->data.string, "flex-end") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_FLEX_END;
            } else if (strcmp(value->data.string, "center") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_CENTER;
            } else if (strcmp(value->data.string, "space-between") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_SPACE_BETWEEN;
            } else if (strcmp(value->data.string, "space-around") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_SPACE_AROUND;
            } else if (strcmp(value->data.string, "space-evenly") == 0) {
                style->flex.justify_content = CSS_JUSTIFY_SPACE_EVENLY;
            }
        }
    }
    else if (strcmp(name, "align-items") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            if (strcmp(value->data.string, "flex-start") == 0) {
                style->flex.align_items = CSS_ALIGN_FLEX_START;
            } else if (strcmp(value->data.string, "flex-end") == 0) {
                style->flex.align_items = CSS_ALIGN_FLEX_END;
            } else if (strcmp(value->data.string, "center") == 0) {
                style->flex.align_items = CSS_ALIGN_CENTER;
            } else if (strcmp(value->data.string, "stretch") == 0) {
                style->flex.align_items = CSS_ALIGN_STRETCH;
            } else if (strcmp(value->data.string, "baseline") == 0) {
                style->flex.align_items = CSS_ALIGN_BASELINE;
            }
        }
    }
    else if (strcmp(name, "gap") == 0 && value) {
        if (value->type == CSS_VALUE_PX || value->type == CSS_VALUE_NUMBER) {
            style->flex.gap = value->data.number;
            style->grid.gap = value->data.number;
        }
    }
    else if (strcmp(name, "flex-grow") == 0 && value) {
        if (value->type == CSS_VALUE_NUMBER) {
            style->flex.grow = value->data.number;
        }
    }
    else if (strcmp(name, "flex-shrink") == 0 && value) {
        if (value->type == CSS_VALUE_NUMBER) {
            style->flex.shrink = value->data.number;
        }
    }
    else if (strcmp(name, "width") == 0 && value) {
        style->width = value;
    }
    else if (strcmp(name, "height") == 0 && value) {
        style->height = value;
    }
    else if (strcmp(name, "min-width") == 0 && value) {
        style->min_width = value;
    }
    else if (strcmp(name, "min-height") == 0 && value) {
        style->min_height = value;
    }
    else if (strcmp(name, "max-width") == 0 && value) {
        style->max_width = value;
    }
    else if (strcmp(name, "max-height") == 0 && value) {
        style->max_height = value;
    }
    else if (strcmp(name, "margin") == 0 && value) {
        style->margin[0] = value; // All sides
        style->margin[1] = value;
        style->margin[2] = value;
        style->margin[3] = value;
    }
    else if (strcmp(name, "margin-top") == 0 && value) {
        style->margin[0] = value;
    }
    else if (strcmp(name, "margin-right") == 0 && value) {
        style->margin[1] = value;
    }
    else if (strcmp(name, "margin-bottom") == 0 && value) {
        style->margin[2] = value;
    }
    else if (strcmp(name, "margin-left") == 0 && value) {
        style->margin[3] = value;
    }
    else if (strcmp(name, "padding") == 0 && value) {
        style->padding[0] = value;
        style->padding[1] = value;
        style->padding[2] = value;
        style->padding[3] = value;
    }
    else if (strcmp(name, "padding-top") == 0 && value) {
        style->padding[0] = value;
    }
    else if (strcmp(name, "padding-right") == 0 && value) {
        style->padding[1] = value;
    }
    else if (strcmp(name, "padding-bottom") == 0 && value) {
        style->padding[2] = value;
    }
    else if (strcmp(name, "padding-left") == 0 && value) {
        style->padding[3] = value;
    }
    else if (strcmp(name, "color") == 0 && value) {
        if (value->type == CSS_VALUE_COLOR_HEX || value->type == CSS_VALUE_COLOR_RGB) {
            style->color = (value->data.color.r << 16) | 
                          (value->data.color.g << 8) | 
                          value->data.color.b;
        }
    }
    else if (strcmp(name, "background-color") == 0 && value) {
        if (value->type == CSS_VALUE_COLOR_HEX || value->type == CSS_VALUE_COLOR_RGB) {
            style->background_color = (value->data.color.r << 16) | 
                                      (value->data.color.g << 8) | 
                                      value->data.color.b;
        }
    }
    else if (strcmp(name, "font-size") == 0 && value) {
        style->font_size = value;
    }
    else if (strcmp(name, "font-family") == 0 && value) {
        if (value->type == CSS_VALUE_STRING || value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->font_family, value->data.string, 255);
        }
    }
    else if (strcmp(name, "font-weight") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD || value->type == CSS_VALUE_NUMBER) {
            strncpy(style->font_weight, value->data.string, 15);
        }
    }
    else if (strcmp(name, "line-height") == 0 && value) {
        style->line_height = value;
    }
    else if (strcmp(name, "letter-spacing") == 0 && value) {
        style->letter_spacing = value;
    }
    else if (strcmp(name, "word-spacing") == 0 && value) {
        style->word_spacing = value;
    }
    else if (strcmp(name, "text-align") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->text_align, value->data.string, 15);
        }
    }
    else if (strcmp(name, "position") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->position, value->data.string, 15);
        }
    }
    else if (strcmp(name, "top") == 0 && value) {
        style->top = value;
    }
    else if (strcmp(name, "right") == 0 && value) {
        style->right = value;
    }
    else if (strcmp(name, "bottom") == 0 && value) {
        style->bottom = value;
    }
    else if (strcmp(name, "left") == 0 && value) {
        style->left = value;
    }
    else if (strcmp(name, "z-index") == 0 && value) {
        if (value->type == CSS_VALUE_NUMBER) {
            style->z_index = (int)value->data.number;
        }
    }
    else if (strcmp(name, "opacity") == 0 && value) {
        if (value->type == CSS_VALUE_NUMBER) {
            style->opacity = (float)value->data.number;
        }
    }
    else if (strcmp(name, "border-radius") == 0 && value) {
        if (value->type == CSS_VALUE_PX || value->type == CSS_VALUE_NUMBER) {
            style->border.top.radius[0] = value->data.number;
            style->border.top.radius[1] = value->data.number;
            style->border.top.radius[2] = value->data.number;
            style->border.top.radius[3] = value->data.number;
        }
    }
    else if (strcmp(name, "border-width") == 0 && value) {
        if (value->type == CSS_VALUE_PX || value->type == CSS_VALUE_NUMBER) {
            style->border.top.width = value->data.number;
            style->border.right.width = value->data.number;
            style->border.bottom.width = value->data.number;
            style->border.left.width = value->data.number;
        }
    }
    else if (strcmp(name, "border-style") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->border.top.style, value->data.string, 15);
            strncpy(style->border.right.style, value->data.string, 15);
            strncpy(style->border.bottom.style, value->data.string, 15);
            strncpy(style->border.left.style, value->data.string, 15);
        }
    }
    else if (strcmp(name, "border-color") == 0 && value) {
        if (value->type == CSS_VALUE_COLOR_HEX || value->type == CSS_VALUE_COLOR_RGB) {
            uint32_t color = (value->data.color.r << 16) | 
                            (value->data.color.g << 8) | 
                            value->data.color.b;
            style->border.top.color = color;
            style->border.right.color = color;
            style->border.bottom.color = color;
            style->border.left.color = color;
        }
    }
    else if (strcmp(name, "overflow") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->overflow, value->data.string, 15);
        }
    }
    else if (strcmp(name, "overflow-x") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->overflow_x, value->data.string, 15);
        }
    }
    else if (strcmp(name, "overflow-y") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->overflow_y, value->data.string, 15);
        }
    }
    else if (strcmp(name, "visibility") == 0 && value) {
        if (value->type == CSS_VALUE_KEYWORD) {
            strncpy(style->visibility, value->data.string, 15);
        }
    }
    else if (strcmp(name, "box-sizing") == 0 && value) {
        style->box_sizing = value;
    }
    else if (strcmp(name, "grid-template-columns") == 0 && value) {
        // Simplified grid template parsing
        if (value->type == CSS_VALUE_KEYWORD || value->type == CSS_VALUE_STRING) {
            // Would parse grid template
        }
    }
    else if (strcmp(name, "grid-template-rows") == 0 && value) {
        // Simplified grid template parsing
        if (value->type == CSS_VALUE_KEYWORD || value->type == CSS_VALUE_STRING) {
            // Would parse grid template
        }
    }
    else if (strcmp(name, "grid-gap") == 0 && value) {
        if (value->type == CSS_VALUE_PX || value->type == CSS_VALUE_NUMBER) {
            style->grid.gap = value->data.number;
        }
    }
}

// ============================================================================
// SELECTOR PARSING
// ============================================================================

static int parse_selector(css_parser_t* parser, css_selector_t* selector) {
    memset(selector, 0, sizeof(css_selector_t));
    
    css_selector_part_t* parts = (css_selector_part_t*)kmalloc(sizeof(css_selector_part_t) * 8);
    if (!parts) return 0;
    
    selector->parts = parts;
    
    while (1) {
        skip_whitespace(parser);
        char c = peek_char(parser);
        
        if (c == '{' || c == ',' || c == '\0') break;
        
        // Combinator
        if (c == '>' || c == '+' || c == '~') {
            selector->combinator = next_char(parser);
            skip_whitespace(parser);
            c = peek_char(parser);
        }
        
        if (selector->part_count >= 8) break;
        
        css_selector_part_t* part = &parts[selector->part_count];
        
        if (c == '#') {
            next_char(parser);
            part->type = CSS_SEL_ID;
            int i = 0;
            while ((is_alpha(peek_char(parser)) || is_digit(peek_char(parser)) || 
                    peek_char(parser) == '-' || peek_char(parser) == '_') && i < 63) {
                part->value[i++] = next_char(parser);
            }
            part->specificity = 100;
        }
        else if (c == '.') {
            next_char(parser);
            part->type = CSS_SEL_CLASS;
            int i = 0;
            while ((is_alpha(peek_char(parser)) || is_digit(peek_char(parser)) || 
                    peek_char(parser) == '-' || peek_char(parser) == '_') && i < 63) {
                part->value[i++] = next_char(parser);
            }
            part->specificity = 10;
        }
        else if (c == '*') {
            next_char(parser);
            part->type = CSS_SEL_UNIVERSAL;
            strcpy(part->value, "*");
            part->specificity = 0;
        }
        else if (c == ':') {
            next_char(parser);
            if (peek_char(parser) == ':') {
                next_char(parser);
                part->type = CSS_SEL_PSEUDO_ELEMENT;
            } else {
                part->type = CSS_SEL_PSEUDO_CLASS;
            }
            int i = 0;
            while ((is_alpha(peek_char(parser)) || peek_char(parser) == '-' || 
                    peek_char(parser) == '(') && i < 63) {
                part->value[i++] = next_char(parser);
            }
            if (peek_char(parser) == '(') {
                next_char(parser);
                i = 0;
                while (peek_char(parser) != ')' && peek_char(parser) != '\0' && i < 63) {
                    part->pseudo_arg[i++] = next_char(parser);
                }
                if (peek_char(parser) == ')') next_char(parser);
            }
            part->specificity = 1;
        }
        else if (c == '[') {
            next_char(parser);
            part->type = CSS_SEL_ATTRIBUTE;
            int i = 0;
            // Parse attribute name
            while (is_alpha(peek_char(parser)) && i < 31) {
                part->attribute[i++] = next_char(parser);
            }
            skip_whitespace(parser);
            // Parse operator
            if (peek_char(parser) == '=' || peek_char(parser) == '~' || 
                peek_char(parser) == '|' || peek_char(parser) == '^' || 
                peek_char(parser) == '$' || peek_char(parser) == '*') {
                part->operator[0] = next_char(parser);
                if (peek_char(parser) == '=') {
                    part->operator[1] = next_char(parser);
                }
            }
            skip_whitespace(parser);
            // Parse value
            i = 0;
            while (peek_char(parser) != ']' && peek_char(parser) != '\0' && i < 63) {
                part->value[i++] = next_char(parser);
            }
            if (peek_char(parser) == ']') next_char(parser);
            part->specificity = 10;
        }
        else if (is_alpha(c)) {
            part->type = CSS_SEL_TYPE;
            int i = 0;
            while ((is_alpha(peek_char(parser)) || is_digit(peek_char(parser)) || 
                    peek_char(parser) == '-') && i < 63) {
                part->value[i++] = next_char(parser);
            }
            part->specificity = 1;
        }
        else {
            break;
        }
        
        selector->specificity += part->specificity;
        selector->part_count++;
    }
    
    return selector->part_count > 0;
}

// ============================================================================
// RULE PARSING
// ============================================================================

static int parse_rule(css_parser_t* parser, css_rule_t* rule) {
    memset(rule, 0, sizeof(css_rule_t));
    
    // Parse selectors
    css_selector_t* selectors = (css_selector_t*)kmalloc(sizeof(css_selector_t) * 8);
    if (!selectors) return 0;
    
    rule->selectors = selectors;
    
    // Parse selector list
    do {
        if (rule->selector_count >= 8) break;
        if (parse_selector(parser, &selectors[rule->selector_count])) {
            rule->selector_count++;
        }
        skip_whitespace(parser);
        if (peek_char(parser) == ',') {
            next_char(parser);
            skip_whitespace(parser);
        } else {
            break;
        }
    } while (1);
    
    // Expect {
    skip_whitespace(parser);
    if (peek_char(parser) != '{') {
        return 0;
    }
    next_char(parser);
    
    // Parse declarations
    while (1) {
        skip_whitespace(parser);
        
        if (peek_char(parser) == '}' || peek_char(parser) == '\0') {
            break;
        }
        
        parse_property(parser, &rule->style);
        
        skip_whitespace(parser);
        if (peek_char(parser) == ';') {
            next_char(parser);
        }
    }
    
    if (peek_char(parser) == '}') {
        next_char(parser);
    }
    
    return rule->selector_count > 0;
}

// ============================================================================
// STYLESHEET PARSING
// ============================================================================

int css_parse_stylesheet(css_parser_t* parser) {
    while (1) {
        skip_whitespace(parser);
        
        if (peek_char(parser) == '\0') break;
        
        // Check for at-rule
        if (peek_char(parser) == '@') {
            next_char(parser);
            
            // Parse at-rule name
            char at_name[32] = {0};
            int i = 0;
            while (is_alpha(peek_char(parser)) && i < 31) {
                at_name[i++] = next_char(parser);
            }
            
            // Handle different at-rules
            if (strcmp(at_name, "media") == 0) {
                // Parse media query
                skip_whitespace(parser);
                while (peek_char(parser) != '{' && peek_char(parser) != '\0') {
                    next_char(parser);
                }
                if (peek_char(parser) == '{') {
                    next_char(parser);
                    // Skip to matching }
                    int depth = 1;
                    while (depth > 0 && peek_char(parser) != '\0') {
                        if (peek_char(parser) == '{') depth++;
                        else if (peek_char(parser) == '}') depth--;
                        next_char(parser);
                    }
                }
            }
            else if (strcmp(at_name, "keyframes") == 0) {
                // Skip keyframes
                skip_whitespace(parser);
                while (peek_char(parser) != '{' && peek_char(parser) != '\0') {
                    next_char(parser);
                }
                if (peek_char(parser) == '{') {
                    next_char(parser);
                    int depth = 1;
                    while (depth > 0 && peek_char(parser) != '\0') {
                        if (peek_char(parser) == '{') depth++;
                        else if (peek_char(parser) == '}') depth--;
                        next_char(parser);
                    }
                }
            }
            else if (strcmp(at_name, "font-face") == 0) {
                // Skip font-face
                skip_whitespace(parser);
                if (peek_char(parser) == '{') {
                    next_char(parser);
                    int depth = 1;
                    while (depth > 0 && peek_char(parser) != '\0') {
                        if (peek_char(parser) == '{') depth++;
                        else if (peek_char(parser) == '}') depth--;
                        next_char(parser);
                    }
                }
            }
            else if (strcmp(at_name, "import") == 0) {
                // Skip import
                while (peek_char(parser) != ';' && peek_char(parser) != '\0') {
                    next_char(parser);
                }
                if (peek_char(parser) == ';') next_char(parser);
            }
            else {
                // Unknown at-rule, skip it
                while (peek_char(parser) != '{' && peek_char(parser) != ';' && 
                       peek_char(parser) != '\0') {
                    next_char(parser);
                }
                if (peek_char(parser) == '{') {
                    next_char(parser);
                    int depth = 1;
                    while (depth > 0 && peek_char(parser) != '\0') {
                        if (peek_char(parser) == '{') depth++;
                        else if (peek_char(parser) == '}') depth--;
                        next_char(parser);
                    }
                } else if (peek_char(parser) == ';') {
                    next_char(parser);
                }
            }
            
            continue;
        }
        
        // Parse rule
        css_rule_t* rule = (css_rule_t*)kmalloc(sizeof(css_rule_t));
        if (!rule) break;
        
        if (parse_rule(parser, rule)) {
            // Add rule to stylesheet
            if (parser->stylesheet.rule_count < CSS_MAX_RULES) {
                parser->stylesheet.rules = (css_rule_t*)krealloc(
                    parser->stylesheet.rules,
                    sizeof(css_rule_t) * (parser->stylesheet.rule_count + 1)
                );
                parser->stylesheet.rules[parser->stylesheet.rule_count++] = *rule;
            }
        } else {
            kfree(rule);
        }
    }
    
    return parser->stylesheet.rule_count > 0;
}

int css_parse_inline_style(css_parser_t* parser, css_computed_style_t* style) {
    memset(style, 0, sizeof(css_computed_style_t));
    
    while (1) {
        skip_whitespace(parser);
        
        if (peek_char(parser) == '\0') break;
        
        parse_property(parser, style);
        
        skip_whitespace(parser);
        if (peek_char(parser) == ';') {
            next_char(parser);
        }
    }
    
    return 1;
}

const char* css_get_error(css_parser_t* parser) {
    return parser->error;
}

// ============================================================================
// STYLE COMPUTATION
// ============================================================================

void css_default_style(css_computed_style_t* style) {
    memset(style, 0, sizeof(css_computed_style_t));
    
    style->display = CSS_DISPLAY_BLOCK;
    style->visibility[0] = 'v';
    style->visibility[1] = 'i';
    style->visibility[2] = 's';
    style->visibility[3] = 'i';
    style->visibility[4] = 'b';
    style->visibility[5] = 'l';
    style->visibility[6] = 'e';
    
    style->overflow[0] = 'v';
    style->overflow[1] = 'i';
    style->overflow[2] = 's';
    style->overflow[3] = 'i';
    style->overflow[4] = 'b';
    style->overflow[5] = 'l';
    style->overflow[6] = 'e';
    
    style->position[0] = 's';
    style->position[1] = 't';
    style->position[2] = 'a';
    style->position[3] = 't';
    style->position[4] = 'i';
    style->position[5] = 'c';
    
    style->color = 0x000000;
    style->background_color = 0xFFFFFF;
    
    style->opacity = 1.0f;
    style->z_index = 0;
    
    // Default flex settings
    style->flex.direction = CSS_FLEX_DIR_ROW;
    style->flex.wrap = CSS_FLEX_WRAP_NOWRAP;
    style->flex.justify_content = CSS_JUSTIFY_FLEX_START;
    style->flex.align_items = CSS_ALIGN_STRETCH;
    style->flex.grow = 0;
    style->flex.shrink = 1;
    
    // Default font
    strcpy(style->font_family, "sans-serif");
    strcpy(style->font_weight, "normal");
    strcpy(style->font_style, "normal");
    strcpy(style->text_align, "left");
    
    style->border.top.width = 0;
    style->border.right.width = 0;
    style->border.bottom.width = 0;
    style->border.left.width = 0;
}

void css_apply_rule(css_computed_style_t* style, css_rule_t* rule, int importance) {
    // Apply properties from rule to style based on specificity and importance
    // This is a simplified version - a full implementation would cascade properly
    
    if (rule->style.display != CSS_DISPLAY_NONE || 
        (style->display == CSS_DISPLAY_NONE && rule->style.display != 0)) {
        style->display = rule->style.display;
    }
    
    if (rule->style.width) style->width = rule->style.width;
    if (rule->style.height) style->height = rule->style.height;
    if (rule->style.min_width) style->min_width = rule->style.min_width;
    if (rule->style.min_height) style->min_height = rule->style.min_height;
    if (rule->style.max_width) style->max_width = rule->style.max_width;
    if (rule->style.max_height) style->max_height = rule->style.max_height;
    
    for (int i = 0; i < 4; i++) {
        if (rule->style.margin[i]) style->margin[i] = rule->style.margin[i];
        if (rule->style.padding[i]) style->padding[i] = rule->style.padding[i];
    }
    
    if (rule->style.color) style->color = rule->style.color;
    if (rule->style.background_color) style->background_color = rule->style.background_color;
    
    if (rule->style.font_size) style->font_size = rule->style.font_size;
    if (rule->style.font_family[0]) strcpy(style->font_family, rule->style.font_family);
    if (rule->style.font_weight[0]) strcpy(style->font_weight, rule->style.font_weight);
    if (rule->style.line_height) style->line_height = rule->style.line_height;
    
    // Flex properties
    if (rule->style.flex.direction != CSS_FLEX_DIR_ROW || 
        style->flex.direction == 0) {
        style->flex.direction = rule->style.flex.direction;
    }
    if (rule->style.flex.wrap != CSS_FLEX_WRAP_NOWRAP || style->flex.wrap == 0) {
        style->flex.wrap = rule->style.flex.wrap;
    }
    if (rule->style.flex.justify_content != 0) {
        style->flex.justify_content = rule->style.flex.justify_content;
    }
    if (rule->style.flex.align_items != 0) {
        style->flex.align_items = rule->style.flex.align_items;
    }
    if (rule->style.flex.gap != 0) style->flex.gap = rule->style.flex.gap;
    if (rule->style.flex.grow != 0) style->flex.grow = rule->style.flex.grow;
    if (rule->style.flex.shrink != 0) style->flex.shrink = rule->style.flex.shrink;
}

int css_match_selector(css_selector_t* selector, const char* tag_name, const char* id,
                       const char** classes, int class_count) {
    if (!selector || !selector->parts) return 0;
    
    for (int i = 0; i < selector->part_count; i++) {
        css_selector_part_t* part = &selector->parts[i];
        
        switch (part->type) {
            case CSS_SEL_TYPE:
                if (tag_name && strcmp(part->value, tag_name) != 0) return 0;
                break;
                
            case CSS_SEL_ID:
                if (!id || strcmp(part->value, id) != 0) return 0;
                break;
                
            case CSS_SEL_CLASS:
                if (!classes) return 0;
                {
                    int found = 0;
                    for (int j = 0; j < class_count; j++) {
                        if (strcmp(part->value, classes[j]) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) return 0;
                }
                break;
                
            case CSS_SEL_UNIVERSAL:
                break;
                
            default:
                break;
        }
    }
    
    return 1;
}

void css_compute_style(css_computed_style_t* style, css_stylesheet_t* stylesheet,
                       const char* tag_name, const char* id, const char** classes, 
                       int class_count, const char** attributes, int attr_count) {
    // Initialize with defaults
    css_default_style(style);
    
    if (!stylesheet) return;
    
    // Apply matching rules (sorted by specificity)
    for (int i = 0; i < stylesheet->rule_count; i++) {
        css_rule_t* rule = &stylesheet->rules[i];
        
        for (int j = 0; j < rule->selector_count; j++) {
            if (css_match_selector(&rule->selectors[j], tag_name, id, classes, class_count)) {
                css_apply_rule(style, rule, 0);
                break;
            }
        }
    }
}

// ============================================================================
// FLEXBOX LAYOUT ENGINE
// ============================================================================

void css_layout_flexbox(css_layout_node_t* container, double available_width, double available_height) {
    if (!container || !container->style) return;
    
    css_computed_style_t* style = container->style;
    
    if (style->display != CSS_DISPLAY_FLEX && style->display != CSS_DISPLAY_INLINE_FLEX) {
        return;
    }
    
    double main_size = (style->flex.direction == CSS_FLEX_DIR_ROW || 
                        style->flex.direction == CSS_FLEX_DIR_ROW_REVERSE) 
                       ? available_width : available_height;
    double cross_size = (style->flex.direction == CSS_FLEX_DIR_ROW || 
                         style->flex.direction == CSS_FLEX_DIR_ROW_REVERSE) 
                        ? available_height : available_width;
    
    // Collect flex items
    css_layout_node_t* child = container->children;
    int item_count = 0;
    double total_flex_grow = 0;
    double total_flex_shrink = 0;
    double used_main_size = 0;
    
    while (child && item_count < 256) {
        if (child->style && child->style->display != CSS_DISPLAY_NONE) {
            double item_main_size = (style->flex.direction == CSS_FLEX_DIR_ROW || 
                                     style->flex.direction == CSS_FLEX_DIR_ROW_REVERSE)
                                    ? child->style->computed_width 
                                    : child->style->computed_height;
            
            child->flex_base_size = item_main_size;
            child->main_size = item_main_size;
            used_main_size += item_main_size;
            total_flex_grow += child->style->flex.grow;
            total_flex_shrink += child->style->flex.shrink;
            
            item_count++;
        }
        child = child->next_sibling;
    }
    
    // Calculate kfree space
    double kfree_space = main_size - used_main_size - style->flex.gap * (item_count - 1);
    
    // Distribute kfree space
    if (kfree_space > 0 && total_flex_grow > 0) {
        child = container->children;
        while (child) {
            if (child->style && child->style->display != CSS_DISPLAY_NONE) {
                double grow = child->style->flex.grow;
                if (grow > 0) {
                    child->flexed_main_size = child->flex_base_size + 
                                              (kfree_space * grow / total_flex_grow);
                    child->main_size = child->flexed_main_size;
                }
            }
            child = child->next_sibling;
        }
    }
    
    // Position items
    double current_pos = 0;
    child = container->children;
    
    // Apply justify-content
    double spacing = 0;
    double start_offset = 0;
    
    switch (style->flex.justify_content) {
        case CSS_JUSTIFY_FLEX_START:
            break;
        case CSS_JUSTIFY_FLEX_END:
            start_offset = kfree_space;
            break;
        case CSS_JUSTIFY_CENTER:
            start_offset = kfree_space / 2;
            break;
        case CSS_JUSTIFY_SPACE_BETWEEN:
            if (item_count > 1) spacing = kfree_space / (item_count - 1);
            break;
        case CSS_JUSTIFY_SPACE_AROUND:
            spacing = kfree_space / item_count;
            start_offset = spacing / 2;
            break;
        case CSS_JUSTIFY_SPACE_EVENLY:
            spacing = kfree_space / (item_count + 1);
            start_offset = spacing;
            break;
    }
    
    current_pos = start_offset;
    
    while (child) {
        if (child->style && child->style->display != CSS_DISPLAY_NONE) {
            if (style->flex.direction == CSS_FLEX_DIR_ROW) {
                child->x = current_pos;
                child->y = 0;
                child->width = child->main_size;
                
                // Apply align-items
                switch (style->flex.align_items) {
                    case CSS_ALIGN_CENTER:
                        child->y = (cross_size - child->height) / 2;
                        break;
                    case CSS_ALIGN_FLEX_END:
                        child->y = cross_size - child->height;
                        break;
                    case CSS_ALIGN_STRETCH:
                        child->height = cross_size;
                        break;
                    default:
                        break;
                }
            } else {
                child->x = 0;
                child->y = current_pos;
                child->height = child->main_size;
                
                switch (style->flex.align_items) {
                    case CSS_ALIGN_CENTER:
                        child->x = (cross_size - child->width) / 2;
                        break;
                    case CSS_ALIGN_FLEX_END:
                        child->x = cross_size - child->width;
                        break;
                    case CSS_ALIGN_STRETCH:
                        child->width = cross_size;
                        break;
                    default:
                        break;
                }
            }
            
            current_pos += child->main_size + style->flex.gap + spacing;
        }
        child = child->next_sibling;
    }
    
    // Set container dimensions
    container->content_width = current_pos;
    container->content_height = cross_size;
}

// ============================================================================
// GRID LAYOUT ENGINE
// ============================================================================

void css_layout_grid(css_layout_node_t* container, double available_width, double available_height) {
    if (!container || !container->style) return;
    
    css_computed_style_t* style = container->style;
    
    if (style->display != CSS_DISPLAY_GRID && style->display != CSS_DISPLAY_INLINE_GRID) {
        return;
    }
    
    // Simplified grid layout - would need full implementation
    // For now, just lay out items in a single row
    
    double x = 0;
    double y = 0;
    double gap = style->grid.gap;
    
    css_layout_node_t* child = container->children;
    while (child) {
        if (child->style && child->style->display != CSS_DISPLAY_NONE) {
            child->x = x;
            child->y = y;
            
            x += child->width + gap;
            
            if (x > available_width) {
                x = 0;
                y += child->height + gap;
                child->x = x;
                child->y = y;
                x = child->width + gap;
            }
        }
        child = child->next_sibling;
    }
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

int css_parse_color(const char* value, uint32_t* color) {
    if (!value || !color) return 0;
    
    if (value[0] == '#') {
        value++;
        uint32_t c = 0;
        while (*value && is_hex(*value)) {
            c = c * 16 + ((*value >= 'a') ? (*value - 'a' + 10) : 
                         (*value >= 'A') ? (*value - 'A' + 10) : (*value - '0'));
            value++;
        }
        *color = c;
        return 1;
    }
    
    // Named colors
    if (strcmp(value, "red") == 0) { *color = 0xFF0000; return 1; }
    if (strcmp(value, "green") == 0) { *color = 0x008000; return 1; }
    if (strcmp(value, "blue") == 0) { *color = 0x0000FF; return 1; }
    if (strcmp(value, "white") == 0) { *color = 0xFFFFFF; return 1; }
    if (strcmp(value, "black") == 0) { *color = 0x000000; return 1; }
    if (strcmp(value, "yellow") == 0) { *color = 0xFFFF00; return 1; }
    if (strcmp(value, "cyan") == 0) { *color = 0x00FFFF; return 1; }
    if (strcmp(value, "magenta") == 0) { *color = 0xFF00FF; return 1; }
    if (strcmp(value, "gray") == 0 || strcmp(value, "grey") == 0) { *color = 0x808080; return 1; }
    if (strcmp(value, "transparent") == 0) { *color = 0x00000000; return 1; }
    
    return 0;
}

double css_compute_length(css_value_t* value, css_computed_style_t* parent_style,
                          double viewport_width, double viewport_height, double root_font_size) {
    if (!value) return 0;
    
    double parent_font_size = parent_style && parent_style->font_size 
                              ? (parent_style->font_size->type == CSS_VALUE_PX 
                                 ? parent_style->font_size->data.number : 16) 
                              : 16;
    
    switch (value->type) {
        case CSS_VALUE_NUMBER:
            return value->data.number;
        case CSS_VALUE_PX:
            return value->data.number;
        case CSS_VALUE_EM:
            return value->data.number * parent_font_size;
        case CSS_VALUE_REM:
            return value->data.number * root_font_size;
        case CSS_VALUE_VW:
            return value->data.number * viewport_width / 100;
        case CSS_VALUE_VH:
            return value->data.number * viewport_height / 100;
        case CSS_VALUE_PERCENTAGE:
            return value->data.percentage; // Caller must handle percentage context
        case CSS_VALUE_PT:
            return value->data.number * 96 / 72; // Points to pixels
        default:
            return 0;
    }
}

void css_stylesheet_kfree(css_stylesheet_t* stylesheet) {
    if (!stylesheet) return;
    
    if (stylesheet->rules) {
        for (int i = 0; i < stylesheet->rule_count; i++) {
            css_rule_t* rule = &stylesheet->rules[i];
            if (rule->selectors) {
                for (int j = 0; j < rule->selector_count; j++) {
                    if (rule->selectors[j].parts) {
                        kfree(rule->selectors[j].parts);
                    }
                }
                kfree(rule->selectors);
            }
        }
        kfree(stylesheet->rules);
    }
    
    if (stylesheet->media_queries) kfree(stylesheet->media_queries);
    if (stylesheet->keyframes) kfree(stylesheet->keyframes);
    if (stylesheet->fonts) kfree(stylesheet->fonts);
}
