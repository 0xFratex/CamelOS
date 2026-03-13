// common/font_v2.h - Enhanced Font System with Spacing and Padding Support
// Version 2.0 - Professional typography for modern rendering

#ifndef FONT_V2_H
#define FONT_V2_H

#include <types.h>

// ============================================================================
// FONT CONFIGURATION
// ============================================================================
#define FONT_MAX_CHARSETS   256
#define FONT_MAX_GLYPHS     1024
#define FONT_MAX_NAME_LEN   64
#define FONT_MAX_KERNING    512

// ============================================================================
// GLYPH STRUCTURE
// ============================================================================

typedef struct {
    uint16_t codepoint;         // Unicode codepoint
    int16_t  advance_x;         // Horizontal advance (pixels * 64)
    int16_t  advance_y;         // Vertical advance (pixels * 64)
    int16_t  bearing_x;         // Left bearing (pixels * 64)
    int16_t  bearing_y;         // Top bearing (pixels * 64)
    uint8_t  width;             // Glyph bitmap width
    uint8_t  height;            // Glyph bitmap height
    uint8_t* bitmap;            // Glyph bitmap (anti-aliased)
    uint8_t  grayscale;         // Grayscale levels (1, 2, 4, 8 bits)
} Glyph;

// ============================================================================
// KERNING PAIR
// ============================================================================

typedef struct {
    uint16_t left;              // Left glyph codepoint
    uint16_t right;             // Right glyph codepoint
    int16_t  adjustment;        // Kerning adjustment (pixels * 64)
} KerningPair;

// ============================================================================
// FONT METRICS
// ============================================================================

typedef struct {
    // Basic metrics
    uint16_t units_per_em;      // Design units per em
    int16_t  ascent;            // Distance from baseline to top
    int16_t  descent;           // Distance from baseline to bottom
    int16_t  line_gap;          // Recommended line spacing
    int16_t  max_advance;       // Maximum advance width
    
    // Spacing adjustments
    int16_t  letter_spacing;    // Additional letter spacing (pixels * 64)
    int16_t  word_spacing;      // Additional word spacing (pixels * 64)
    int16_t  line_height;       // Custom line height override
    int16_t  paragraph_spacing; // Space between paragraphs
    
    // Padding (for rendering context)
    int8_t   padding_top;
    int8_t   padding_bottom;
    int8_t   padding_left;
    int8_t   padding_right;
    
    // Font features
    uint8_t  is_monospace;
    uint8_t  is_bold;
    uint8_t  is_italic;
    uint8_t  has_kerning;
    uint8_t  has_antialiasing;
    
} FontMetrics;

// ============================================================================
// FONT FACE
// ============================================================================

typedef struct {
    char         name[FONT_MAX_NAME_LEN];
    char         family[FONT_MAX_NAME_LEN];
    char         style[FONT_MAX_NAME_LEN];  // Regular, Bold, Italic, etc.
    uint16_t     size;                      // Size in points
    uint16_t     dpi;                       // DPI for sizing
    
    FontMetrics  metrics;
    
    // Glyph storage
    Glyph*       glyphs;
    uint16_t     glyph_count;
    uint16_t     glyph_capacity;
    
    // Character to glyph mapping
    uint16_t     charmap[65536];            // Unicode to glyph index
    
    // Kerning table
    KerningPair* kerning;
    uint16_t     kerning_count;
    
    // Render cache
    uint8_t*     cache;
    uint32_t     cache_size;
    
} FontFace;

// ============================================================================
// TEXT RENDERING CONTEXT
// ============================================================================

typedef struct {
    FontFace*    font;
    
    // Current position
    int32_t      cursor_x;
    int32_t      cursor_y;
    int32_t      baseline_y;
    
    // Clipping
    int16_t      clip_x;
    int16_t      clip_y;
    int16_t      clip_w;
    int16_t      clip_h;
    
    // Colors
    uint32_t     foreground_color;
    uint32_t     background_color;
    uint32_t     selection_color;
    
    // Text attributes
    uint8_t      underline;
    uint8_t      strikethrough;
    uint8_t      overline;
    
    // Spacing overrides
    int16_t      letter_spacing;
    int16_t      word_spacing;
    int16_t      line_height;
    int16_t      tab_width;
    
    // Alignment
    uint8_t      text_align;        // 0=left, 1=center, 2=right, 3=justify
    uint8_t      vertical_align;    // 0=top, 1=middle, 2=bottom, 3=baseline
    
    // Word wrap
    uint8_t      word_wrap;
    int16_t      wrap_width;
    
    // Render target
    uint8_t*     framebuffer;
    int          fb_width;
    int          fb_height;
    int          fb_pitch;
    
} TextContext;

// ============================================================================
// TEXT RUN (for rich text)
// ============================================================================

typedef struct {
    uint32_t     start;
    uint32_t     length;
    FontFace*    font;
    uint32_t     color;
    uint8_t      underline;
    uint8_t      bold;
    uint8_t      italic;
} TextRun;

// ============================================================================
// PARAGRAPH LAYOUT
// ============================================================================

typedef struct {
    int32_t  x;
    int32_t  y;
    int32_t  width;
    int32_t  height;
    int32_t  baseline;
    uint32_t start_glyph;
    uint32_t glyph_count;
    int8_t   alignment;
} LineLayout;

typedef struct {
    LineLayout*  lines;
    uint32_t     line_count;
    int32_t      total_width;
    int32_t      total_height;
    uint8_t*     glyph_positions;  // Packed x,y positions
} ParagraphLayout;

// ============================================================================
// STANDARD SIZES (in points)
// ============================================================================

typedef enum {
    FONT_SIZE_TINY = 8,
    FONT_SIZE_SMALL = 10,
    FONT_SIZE_NORMAL = 12,
    FONT_SIZE_MEDIUM = 14,
    FONT_SIZE_LARGE = 16,
    FONT_SIZE_TITLE = 20,
    FONT_SIZE_HEADING = 24,
    FONT_SIZE_LARGE_TITLE = 32,
    FONT_SIZE_DISPLAY = 48
} FontSize;

// ============================================================================
// FONT API
// ============================================================================

// Initialization
void font_system_init(void);
void font_system_cleanup(void);

// Font loading
FontFace* font_load_system(uint16_t size);
FontFace* font_load_memory(const uint8_t* data, uint32_t size, uint16_t pt_size);
void font_free(FontFace* font);

// Font metrics
int font_get_line_height(FontFace* font);
int font_get_ascent(FontFace* font);
int font_get_descent(FontFace* font);
int font_get_em_width(FontFace* font);
int font_get_char_width(FontFace* font, uint32_t codepoint);
int font_get_text_width(FontFace* font, const char* text);
int font_get_text_height(FontFace* font, const char* text);

// Glyph operations
Glyph* font_get_glyph(FontFace* font, uint32_t codepoint);
int font_render_glyph(FontFace* font, uint32_t codepoint, uint8_t* buffer);

// Kerning
int font_get_kerning(FontFace* font, uint32_t left, uint32_t right);

// ============================================================================
// TEXT CONTEXT API
// ============================================================================

TextContext* text_context_create(FontFace* font, uint8_t* framebuffer, 
                                  int width, int height, int pitch);
void text_context_destroy(TextContext* ctx);

void text_set_font(TextContext* ctx, FontFace* font);
void text_set_color(TextContext* ctx, uint32_t color);
void text_set_background(TextContext* ctx, uint32_t color);
void text_set_spacing(TextContext* ctx, int letter_spacing, int word_spacing);
void text_set_line_height(TextContext* ctx, int line_height);
void text_set_alignment(TextContext* ctx, int align);
void text_set_clip(TextContext* ctx, int x, int y, int w, int h);

void text_move_to(TextContext* ctx, int x, int y);
void text_newline(TextContext* ctx);

// ============================================================================
// TEXT RENDERING API
// ============================================================================

// Render single character
int text_draw_char(TextContext* ctx, uint32_t codepoint);
int text_draw_char_at(TextContext* ctx, uint32_t codepoint, int x, int y);

// Render string
int text_draw_string(TextContext* ctx, const char* text);
int text_draw_string_at(TextContext* ctx, const char* text, int x, int y);
int text_draw_string_len(TextContext* ctx, const char* text, uint32_t length);

// Render with formatting
int text_draw_formatted(TextContext* ctx, const char* format, ...);

// Render with word wrap
int text_draw_wrapped(TextContext* ctx, const char* text, int max_width);

// Render justified text
int text_draw_justified(TextContext* ctx, const char* text, int width);

// ============================================================================
// LAYOUT API
// ============================================================================

ParagraphLayout* text_layout_paragraph(TextContext* ctx, const char* text, int max_width);
void text_layout_free(ParagraphLayout* layout);

// Measure text
int text_measure_width(TextContext* ctx, const char* text);
int text_measure_height(TextContext* ctx, const char* text, int max_width);
void text_measure_bounds(TextContext* ctx, const char* text, 
                          int* width, int* height, int* ascent, int* descent);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// UTF-8 handling
uint32_t utf8_decode(const char** ptr);
int utf8_encode(uint32_t codepoint, char* out);
int utf8_strlen(const char* str);

// Color utilities
uint32_t color_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t color_blend(uint32_t fg, uint32_t bg, uint8_t alpha);

// ============================================================================
// BUILT-IN FONTS
// ============================================================================

// System monospace font (8x16)
extern FontFace font_system_mono_8x16;

// System proportional font (variable width)
extern FontFace font_system_prop_12;

// Large display font
extern FontFace font_display_24;

// Initialize built-in fonts
void font_init_builtin_fonts(void);

// ============================================================================
// RENDERING HELPERS (for integration with gfx_hal)
// ============================================================================

// Render glyph to framebuffer
void font_render_glyph_fb(FontFace* font, uint32_t codepoint, 
                           uint8_t* fb, int fb_w, int fb_h, int pitch,
                           int x, int y, uint32_t color);

// Render string to framebuffer
void font_render_string_fb(FontFace* font, const char* text,
                            uint8_t* fb, int fb_w, int fb_h, int pitch,
                            int x, int y, uint32_t color);

// Render with background
void font_render_string_bg_fb(FontFace* font, const char* text,
                               uint8_t* fb, int fb_w, int fb_h, int pitch,
                               int x, int y, uint32_t fg_color, uint32_t bg_color);

// ============================================================================
// ENHANCED VGA FONT WITH SPACING
// ============================================================================

// Extended font data with spacing information
typedef struct {
    uint8_t width;      // Actual character width (not fixed 8)
    int8_t  bearing;    // Left bearing adjustment
    int8_t  kern_left;  // Kerning adjustment for following chars
} FontCharMetrics;

// Enhanced VGA font with proper widths
extern const uint8_t font_enhanced_8x16[96][16];
extern const FontCharMetrics font_metrics_8x16[96];

// ============================================================================
// SPACING PRESETS
// ============================================================================

typedef struct {
    const char* name;
    int16_t letter_spacing;
    int16_t word_spacing;
    int16_t line_height;
    int16_t paragraph_spacing;
} SpacingPreset;

extern const SpacingPreset spacing_presets[];

typedef enum {
    SPACING_TIGHT,
    SPACING_NORMAL,
    SPACING_RELAXED,
    SPACING_WIDE,
    SPACING_MONOSPACE,
    SPACING_COUNT
} SpacingPresetType;

// Get preset
const SpacingPreset* font_get_spacing_preset(int preset);

#endif // FONT_V2_H
