// common/font_v2.c - Enhanced Font System Implementation
// Version 2.0 - Professional typography for modern rendering

#include "font_v2.h"
#include "font.h"
#include "string.h"

// ============================================================================
// BUILT-IN FONT DATA
// ============================================================================

// Character width metrics for proportional rendering
// Values based on typical character widths relative to 8px
const FontCharMetrics font_metrics_8x16[96] = {
    // Space through / (ASCII 32-47)
    {4, 0, 0},   // Space - narrower
    {2, 1, 0},   // !
    {4, 0, 0},   // "
    {8, 0, 0},   // #
    {6, 0, 0},   // $
    {8, 0, 0},   // %
    {6, 0, 0},   // &
    {2, 1, 0},   // '
    {4, 1, 0},   // (
    {4, 0, 0},   // )
    {6, 0, 0},   // *
    {8, 0, 0},   // +
    {4, 0, 0},   // ,
    {8, 0, 0},   // -
    {2, 1, 0},   // .
    {8, 0, 0},   // /
    
    // 0 through 9 (ASCII 48-57)
    {6, 0, 0},   // 0
    {4, 1, 0},   // 1 - narrower
    {6, 0, 0},   // 2
    {6, 0, 0},   // 3
    {6, 0, 0},   // 4
    {6, 0, 0},   // 5
    {6, 0, 0},   // 6
    {6, 0, 0},   // 7
    {6, 0, 0},   // 8
    {6, 0, 0},   // 9
    
    // Punctuation (ASCII 58-64)
    {2, 1, 0},   // :
    {4, 0, 0},   // ;
    {6, 0, 0},   // <
    {8, 0, 0},   // =
    {6, 0, 0},   // >
    {6, 0, 0},   // ?
    {8, 0, 0},   // @
    
    // Uppercase A-Z (ASCII 65-90)
    {6, 0, 0},   // A
    {6, 0, 0},   // B
    {6, 0, 0},   // C
    {6, 0, 0},   // D
    {6, 0, 0},   // E
    {6, 0, 0},   // F
    {6, 0, 0},   // G
    {6, 0, 0},   // H
    {2, 1, 0},   // I - narrow
    {6, 0, 0},   // J
    {6, 0, 0},   // K
    {6, 0, 0},   // L
    {8, 0, 0},   // M - wider
    {6, 0, 0},   // N
    {6, 0, 0},   // O
    {6, 0, 0},   // P
    {6, 0, 0},   // Q
    {6, 0, 0},   // R
    {6, 0, 0},   // S
    {6, 0, 0},   // T
    {6, 0, 0},   // U
    {6, 0, 0},   // V
    {8, 0, 0},   // W - wider
    {6, 0, 0},   // X
    {6, 0, 0},   // Y
    {6, 0, 0},   // Z
    
    // Brackets (ASCII 91-96)
    {4, 1, 0},   // [
    {8, 0, 0},   // backslash
    {4, 0, 0},   // ]
    {6, 0, 0},   // ^
    {8, 0, 0},   // _
    {2, 1, 0},   // `
    
    // Lowercase a-z (ASCII 97-122)
    {6, 0, 0},   // a
    {6, 0, 0},   // b
    {6, 0, 0},   // c
    {6, 0, 0},   // d
    {6, 0, 0},   // e
    {4, 0, 0},   // f - narrow
    {6, 0, 0},   // g
    {6, 0, 0},   // h
    {2, 1, 0},   // i - narrow
    {4, 0, 0},   // j - narrow
    {6, 0, 0},   // k
    {4, 0, 0},   // l - narrow
    {8, 0, 0},   // m - wider
    {6, 0, 0},   // n
    {6, 0, 0},   // o
    {6, 0, 0},   // p
    {6, 0, 0},   // q
    {4, 0, 0},   // r - narrow
    {6, 0, 0},   // s
    {4, 0, 0},   // t - narrow
    {6, 0, 0},   // u
    {6, 0, 0},   // v
    {8, 0, 0},   // w - wider
    {6, 0, 0},   // x
    {6, 0, 0},   // y
    {6, 0, 0},   // z
    
    // Final punctuation (ASCII 123-126)
    {4, 1, 0},   // {
    {2, 1, 0},   // |
    {4, 0, 0},   // }
    {6, 0, 0},   // ~
};

// Spacing presets
const SpacingPreset spacing_presets[] = {
    // SPACING_TIGHT
    {"Tight", -1, -2, 14, 4},
    // SPACING_NORMAL
    {"Normal", 0, 0, 16, 8},
    // SPACING_RELAXED
    {"Relaxed", 1, 2, 18, 12},
    // SPACING_WIDE
    {"Wide", 2, 4, 20, 16},
    // SPACING_MONOSPACE
    {"Monospace", 0, 8, 16, 8},
};

// ============================================================================
// KERNING TABLE
// ============================================================================

// Common kerning pairs (values in 64ths of a pixel)
static const KerningPair kerning_table[] = {
    // Letter combinations that need adjustment
    {'A', 'V', -20}, {'A', 'W', -16}, {'A', 'Y', -18},
    {'V', 'A', -16}, {'W', 'A', -14}, {'Y', 'A', -18},
    {'T', 'a', -8},  {'T', 'e', -8},  {'T', 'o', -8},
    {'f', 'f', -8},  {'f', 'i', -6},  {'f', 'l', -6},
    {'r', 'n', -4},  {'r', 'o', -4},  {'r', 'a', -4},
    {'P', 'A', -12}, {'F', 'A', -10},
    {'L', 'V', -14}, {'L', 'Y', -12},
};

#define KERNING_TABLE_SIZE (sizeof(kerning_table) / sizeof(KerningPair))

// ============================================================================
// FONT SYSTEM INITIALIZATION
// ============================================================================

static int font_system_initialized = 0;
static FontFace builtin_fonts[4];
static int builtin_font_count = 0;

void font_system_init(void) {
    if (font_system_initialized) return;
    
    // Initialize built-in fonts
    font_init_builtin_fonts();
    
    font_system_initialized = 1;
}

void font_system_cleanup(void) {
    if (!font_system_initialized) return;
    
    // Free any loaded fonts
    for (int i = 0; i < builtin_font_count; i++) {
        if (builtin_fonts[i].glyphs) {
            // Free glyph memory
        }
    }
    
    font_system_initialized = 0;
}

void font_init_builtin_fonts(void) {
    // Initialize 8x16 monospace font
    FontFace* mono = &builtin_fonts[0];
    memset(mono, 0, sizeof(FontFace));
    
    strcpy(mono->name, "VGA Mono 8x16");
    strcpy(mono->family, "VGA");
    strcpy(mono->style, "Mono");
    mono->size = 12;
    mono->dpi = 96;
    
    mono->metrics.units_per_em = 2048;
    mono->metrics.ascent = 12;
    mono->metrics.descent = 4;
    mono->metrics.line_gap = 0;
    mono->metrics.max_advance = 8;
    mono->metrics.is_monospace = 1;
    mono->metrics.has_antialiasing = 0;
    
    // Set default spacing
    mono->metrics.letter_spacing = 0;
    mono->metrics.word_spacing = 8;
    mono->metrics.line_height = 16;
    mono->metrics.paragraph_spacing = 8;
    
    // Build character map for ASCII
    for (int i = 0; i < 96; i++) {
        mono->charmap[32 + i] = i;
    }
    
    builtin_font_count++;
}

// ============================================================================
// FONT LOADING
// ============================================================================

FontFace* font_load_system(uint16_t size) {
    if (!font_system_initialized) {
        font_system_init();
    }
    
    // Return appropriate built-in font based on size
    if (size <= 12) {
        return &builtin_fonts[0];
    }
    
    return &builtin_fonts[0]; // Default to mono
}

void font_free(FontFace* font) {
    if (font == NULL) return;
    
    // Don't free built-in fonts
    for (int i = 0; i < builtin_font_count; i++) {
        if (font == &builtin_fonts[i]) return;
    }
    
    // Free custom font memory
    if (font->glyphs) {
        // Would free glyph memory
    }
    if (font->kerning) {
        // Would free kerning table
    }
}

// ============================================================================
// FONT METRICS
// ============================================================================

int font_get_line_height(FontFace* font) {
    if (font == NULL) return 16;
    
    if (font->metrics.line_height > 0) {
        return font->metrics.line_height;
    }
    
    return font->metrics.ascent - font->metrics.descent + font->metrics.line_gap;
}

int font_get_ascent(FontFace* font) {
    if (font == NULL) return 12;
    return font->metrics.ascent;
}

int font_get_descent(FontFace* font) {
    if (font == NULL) return 4;
    return font->metrics.descent;
}

int font_get_em_width(FontFace* font) {
    if (font == NULL) return 8;
    return font->metrics.max_advance;
}

int font_get_char_width(FontFace* font, uint32_t codepoint) {
    if (font == NULL) return 8;
    
    if (codepoint >= 32 && codepoint < 128) {
        int idx = codepoint - 32;
        if (font->metrics.is_monospace) {
            return 8;
        }
        return font_metrics_8x16[idx].width + font->metrics.letter_spacing;
    }
    
    return 8; // Default width
}

int font_get_text_width(FontFace* font, const char* text) {
    if (font == NULL || text == NULL) return 0;
    
    int width = 0;
    int prev_char = 0;
    
    while (*text) {
        uint32_t codepoint = (uint8_t)*text;
        int char_width = font_get_char_width(font, codepoint);
        
        // Apply kerning
        if (prev_char != 0) {
            int kern = font_get_kerning(font, prev_char, codepoint);
            char_width += kern;
        }
        
        width += char_width;
        prev_char = codepoint;
        text++;
    }
    
    return width;
}

int font_get_text_height(FontFace* font, const char* text) {
    if (font == NULL || text == NULL) return 0;
    
    int lines = 1;
    while (*text) {
        if (*text == '\n') lines++;
        text++;
    }
    
    return lines * font_get_line_height(font);
}

// ============================================================================
// GLYPH OPERATIONS
// ============================================================================

Glyph* font_get_glyph(FontFace* font, uint32_t codepoint) {
    if (font == NULL) return NULL;
    
    // For built-in font, return pointer to bitmap data
    if ((codepoint >= 32 && codepoint <= 127) || (codepoint >= 160 && codepoint <= 255)) {
        static Glyph glyph;
        int idx;
        const uint8_t* bitmap_data;
        int char_w = 8; // Default width for Latin-1 glyphs
        
        if (codepoint >= 32 && codepoint <= 127) {
            idx = codepoint - 32;
            bitmap_data = (uint8_t*)font_8x16[idx];
            char_w = font_metrics_8x16[idx].width;
        } else {
            idx = codepoint - 160;
            bitmap_data = (uint8_t*)font_latin1_8x16[idx];
        }
        
        glyph.codepoint = codepoint;
        glyph.width = char_w;
        glyph.height = 16;
        glyph.advance_x = char_w * 64;
        glyph.advance_y = 0;
        glyph.bearing_x = (codepoint <= 127 ? font_metrics_8x16[codepoint - 32].bearing : 0) * 64;
        glyph.bearing_y = 12 * 64;
        glyph.bitmap = (uint8_t*)bitmap_data;
        glyph.grayscale = 1;
        
        return &glyph;
    }
    
    return NULL;
}

int font_render_glyph(FontFace* font, uint32_t codepoint, uint8_t* buffer) {
    if (font == NULL || buffer == NULL) return 0;
    
    Glyph* glyph = font_get_glyph(font, codepoint);
    if (glyph == NULL) return 0;
    
    // Copy glyph bitmap to buffer
    memcpy(buffer, glyph->bitmap, glyph->height * ((glyph->width + 7) / 8));
    
    return 1;
}

// ============================================================================
// KERNING
// ============================================================================

int font_get_kerning(FontFace* font, uint32_t left, uint32_t right) {
    if (font == NULL) return 0;
    if (!font->metrics.has_kerning) return 0;
    
    // Search kerning table
    for (int i = 0; i < KERNING_TABLE_SIZE; i++) {
        if (kerning_table[i].left == left && kerning_table[i].right == right) {
            return kerning_table[i].adjustment;
        }
    }
    
    return 0;
}

// ============================================================================
// TEXT CONTEXT
// ============================================================================

TextContext* text_context_create(FontFace* font, uint8_t* framebuffer, 
                                  int width, int height, int pitch) {
    TextContext* ctx = (TextContext*)malloc(sizeof(TextContext));
    if (ctx == NULL) return NULL;
    
    memset(ctx, 0, sizeof(TextContext));
    
    ctx->font = font ? font : font_load_system(12);
    ctx->framebuffer = framebuffer;
    ctx->fb_width = width;
    ctx->fb_height = height;
    ctx->fb_pitch = pitch;
    
    // Default settings
    ctx->foreground_color = 0xFFFFFFFF;
    ctx->background_color = 0x00000000;
    ctx->letter_spacing = ctx->font->metrics.letter_spacing;
    ctx->word_spacing = ctx->font->metrics.word_spacing;
    ctx->line_height = font_get_line_height(ctx->font);
    ctx->tab_width = 32;
    ctx->text_align = 0;
    
    // Default clipping
    ctx->clip_x = 0;
    ctx->clip_y = 0;
    ctx->clip_w = width;
    ctx->clip_h = height;
    
    return ctx;
}

void text_context_destroy(TextContext* ctx) {
    if (ctx) {
        free(ctx);
    }
}

void text_set_font(TextContext* ctx, FontFace* font) {
    if (ctx && font) ctx->font = font;
}

void text_set_color(TextContext* ctx, uint32_t color) {
    if (ctx) ctx->foreground_color = color;
}

void text_set_background(TextContext* ctx, uint32_t color) {
    if (ctx) ctx->background_color = color;
}

void text_set_spacing(TextContext* ctx, int letter_spacing, int word_spacing) {
    if (ctx) {
        ctx->letter_spacing = letter_spacing;
        ctx->word_spacing = word_spacing;
    }
}

void text_set_line_height(TextContext* ctx, int line_height) {
    if (ctx) ctx->line_height = line_height;
}

void text_set_alignment(TextContext* ctx, int align) {
    if (ctx) ctx->text_align = align;
}

void text_set_clip(TextContext* ctx, int x, int y, int w, int h) {
    if (ctx) {
        ctx->clip_x = x;
        ctx->clip_y = y;
        ctx->clip_w = w;
        ctx->clip_h = h;
    }
}

void text_move_to(TextContext* ctx, int x, int y) {
    if (ctx) {
        ctx->cursor_x = x;
        ctx->cursor_y = y;
        ctx->baseline_y = y + font_get_ascent(ctx->font);
    }
}

void text_newline(TextContext* ctx) {
    if (ctx) {
        ctx->cursor_x = ctx->clip_x;
        ctx->cursor_y += ctx->line_height;
        ctx->baseline_y = ctx->cursor_y + font_get_ascent(ctx->font);
    }
}

// ============================================================================
// TEXT RENDERING
// ============================================================================

int text_draw_char(TextContext* ctx, uint32_t codepoint) {
    if (ctx == NULL || ctx->font == NULL) return 0;
    
    Glyph* glyph = font_get_glyph(ctx->font, codepoint);
    if (glyph == NULL) return 0;
    
    int x = ctx->cursor_x;
    int y = ctx->baseline_y - (glyph->bearing_y / 64);
    
    // Render glyph
    font_render_glyph_fb(ctx->font, codepoint, ctx->framebuffer,
                         ctx->fb_width, ctx->fb_height, ctx->fb_pitch,
                         x, y, ctx->foreground_color);
    
    // Advance cursor
    int advance = (glyph->advance_x / 64) + ctx->letter_spacing;
    ctx->cursor_x += advance;
    
    return advance;
}

int text_draw_char_at(TextContext* ctx, uint32_t codepoint, int x, int y) {
    if (ctx == NULL) return 0;
    
    text_move_to(ctx, x, y);
    return text_draw_char(ctx, codepoint);
}

int text_draw_string(TextContext* ctx, const char* text) {
    if (ctx == NULL || text == NULL) return 0;
    
    int total_width = 0;
    int prev_char = 0;
    
    while (*text) {
        uint32_t codepoint = (uint8_t)*text;
        
        // Handle special characters
        if (codepoint == '\n') {
            text_newline(ctx);
            prev_char = 0;
            text++;
            continue;
        }
        
        if (codepoint == '\t') {
            int tab_stop = ctx->tab_width;
            int next_tab = ((ctx->cursor_x / tab_stop) + 1) * tab_stop;
            ctx->cursor_x = next_tab;
            total_width = next_tab;
            prev_char = 0;
            text++;
            continue;
        }
        
        if (codepoint == ' ') {
            ctx->cursor_x += ctx->word_spacing;
            total_width += ctx->word_spacing;
            prev_char = 0;
            text++;
            continue;
        }
        
        // Apply kerning
        if (prev_char != 0) {
            int kern = font_get_kerning(ctx->font, prev_char, codepoint);
            ctx->cursor_x += kern;
        }
        
        int advance = text_draw_char(ctx, codepoint);
        total_width += advance;
        prev_char = codepoint;
        text++;
    }
    
    return total_width;
}

int text_draw_string_at(TextContext* ctx, const char* text, int x, int y) {
    if (ctx == NULL) return 0;
    
    text_move_to(ctx, x, y);
    return text_draw_string(ctx, text);
}

int text_draw_string_len(TextContext* ctx, const char* text, uint32_t length) {
    if (ctx == NULL || text == NULL) return 0;
    
    int total_width = 0;
    for (uint32_t i = 0; i < length && text[i]; i++) {
        total_width += text_draw_char(ctx, (uint8_t)text[i]);
    }
    
    return total_width;
}

int text_draw_wrapped(TextContext* ctx, const char* text, int max_width) {
    if (ctx == NULL || text == NULL) return 0;
    
    int start_x = ctx->cursor_x;
    int line_start = 0;
    int word_start = 0;
    int current_width = 0;
    int lines = 1;
    
    for (int i = 0; text[i]; i++) {
        uint32_t c = (uint8_t)text[i];
        
        if (c == '\n') {
            // Explicit newline
            text_draw_string_len(ctx, text + line_start, i - line_start);
            text_newline(ctx);
            line_start = i + 1;
            word_start = i + 1;
            current_width = 0;
            lines++;
            continue;
        }
        
        int char_width = font_get_char_width(ctx->font, c);
        
        if (c == ' ' || c == '\t') {
            word_start = i + 1;
        }
        
        if (current_width + char_width > max_width) {
            // Need to wrap
            if (word_start > line_start) {
                // Wrap at word boundary
                text_draw_string_len(ctx, text + line_start, word_start - line_start);
                i = word_start - 1; // Will be incremented
            } else {
                // Wrap at character
                text_draw_string_len(ctx, text + line_start, i - line_start);
                i--; // Re-process this character
            }
            
            text_newline(ctx);
            line_start = word_start;
            current_width = 0;
            lines++;
        } else {
            current_width += char_width;
        }
    }
    
    // Draw remaining text
    if (text[line_start]) {
        text_draw_string(ctx, text + line_start);
    }
    
    return lines;
}

// ============================================================================
// FRAMEBUFFER RENDERING
// ============================================================================

void font_render_glyph_fb(FontFace* font, uint32_t codepoint,
                           uint8_t* fb, int fb_w, int fb_h, int pitch,
                           int x, int y, uint32_t color) {
    if (font == NULL || fb == NULL) return;
    
    const uint8_t* bitmap;
    if (codepoint >= 32 && codepoint <= 127) {
        bitmap = font_8x16[codepoint - 32];
    } else if (codepoint >= 160 && codepoint <= 255) {
        bitmap = font_latin1_8x16[codepoint - 160];
    } else {
        return; // No glyph available
    }
    int idx = (codepoint >= 160) ? codepoint - 160 + 96 : codepoint - 32;
    int char_width = font->metrics.is_monospace ? 8 : 8; // Fallback width
    
    // Extract RGBA from color
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    // Render each row
    for (int row = 0; row < 16; row++) {
        if (y + row < 0 || y + row >= fb_h) continue;
        
        uint8_t row_data = bitmap[row];
        
        for (int col = 0; col < char_width; col++) {
            if (x + col < 0 || x + col >= fb_w) continue;
            
            int bit = 7 - col;
            if (row_data & (1 << bit)) {
                uint32_t* pixel = (uint32_t*)(fb + (y + row) * pitch + (x + col) * 4);
                *pixel = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void font_render_string_fb(FontFace* font, const char* text,
                            uint8_t* fb, int fb_w, int fb_h, int pitch,
                            int x, int y, uint32_t color) {
    if (font == NULL || text == NULL || fb == NULL) return;
    
    int cursor_x = x;
    int cursor_y = y;
    int prev_char = 0;
    
    while (*text) {
        uint32_t c = (uint8_t)*text;
        
        if (c == '\n') {
            cursor_x = x;
            cursor_y += 16;
            prev_char = 0;
            text++;
            continue;
        }
        
        if (c == '\t') {
            cursor_x = ((cursor_x / 32) + 1) * 32;
            prev_char = 0;
            text++;
            continue;
        }
        
        // Apply kerning
        if (prev_char != 0 && font->metrics.has_kerning) {
            cursor_x += font_get_kerning(font, prev_char, c);
        }
        
        font_render_glyph_fb(font, c, fb, fb_w, fb_h, pitch, cursor_x, cursor_y, color);
        
        // Advance with spacing
        int char_width = font->metrics.is_monospace ? 8 : font_metrics_8x16[c - 32].width;
        cursor_x += char_width + font->metrics.letter_spacing;
        prev_char = c;
        text++;
    }
}

void font_render_string_bg_fb(FontFace* font, const char* text,
                               uint8_t* fb, int fb_w, int fb_h, int pitch,
                               int x, int y, uint32_t fg_color, uint32_t bg_color) {
    if (font == NULL || text == NULL || fb == NULL) return;
    
    // Calculate text dimensions
    int width = font_get_text_width(font, text);
    int height = font_get_line_height(font);
    
    // Draw background
    for (int row = 0; row < height; row++) {
        if (y + row < 0 || y + row >= fb_h) continue;
        
        for (int col = 0; col < width; col++) {
            if (x + col < 0 || x + col >= fb_w) continue;
            
            uint32_t* pixel = (uint32_t*)(fb + (y + row) * pitch + (x + col) * 4);
            *pixel = bg_color;
        }
    }
    
    // Draw text
    font_render_string_fb(font, text, fb, fb_w, fb_h, pitch, x, y, fg_color);
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

uint32_t utf8_decode(const char** ptr) {
    const uint8_t* p = (const uint8_t*)*ptr;
    uint32_t codepoint;
    
    if ((p[0] & 0x80) == 0) {
        codepoint = p[0];
        *ptr += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        codepoint = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *ptr += 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        codepoint = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *ptr += 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        codepoint = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | 
                    ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *ptr += 4;
    } else {
        codepoint = p[0];
        *ptr += 1;
    }
    
    return codepoint;
}

int utf8_encode(uint32_t codepoint, char* out) {
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    } else if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
}

int utf8_strlen(const char* str) {
    int len = 0;
    while (*str) {
        if ((*str & 0xC0) != 0x80) len++;
        str++;
    }
    return len;
}

uint32_t color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

uint32_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t color_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 0) return bg;
    if (alpha == 255) return fg;
    
    uint8_t fg_r = (fg >> 16) & 0xFF;
    uint8_t fg_g = (fg >> 8) & 0xFF;
    uint8_t fg_b = fg & 0xFF;
    
    uint8_t bg_r = (bg >> 16) & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bg_b = bg & 0xFF;
    
    uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    
    return (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

const SpacingPreset* font_get_spacing_preset(int preset) {
    if (preset < 0 || preset >= SPACING_COUNT) {
        preset = SPACING_NORMAL;
    }
    return &spacing_presets[preset];
}

// ============================================================================
// TEXT MEASURING
// ============================================================================

int text_measure_width(TextContext* ctx, const char* text) {
    if (ctx == NULL || text == NULL) return 0;
    return font_get_text_width(ctx->font, text);
}

int text_measure_height(TextContext* ctx, const char* text, int max_width) {
    if (ctx == NULL || text == NULL) return 0;
    
    int lines = 1;
    int current_width = 0;
    int word_start = 0;
    
    for (int i = 0; text[i]; i++) {
        uint32_t c = (uint8_t)text[i];
        
        if (c == '\n') {
            lines++;
            current_width = 0;
            word_start = i + 1;
            continue;
        }
        
        int char_width = font_get_char_width(ctx->font, c);
        
        if (c == ' ' || c == '\t') {
            word_start = i + 1;
        }
        
        if (current_width + char_width > max_width) {
            lines++;
            if (word_start > 0 && word_start < i) {
                i = word_start;
            }
            current_width = 0;
        } else {
            current_width += char_width;
        }
    }
    
    return lines * ctx->line_height;
}

void text_measure_bounds(TextContext* ctx, const char* text,
                          int* width, int* height, int* ascent, int* descent) {
    if (ctx == NULL || text == NULL) return;
    
    if (width) *width = font_get_text_width(ctx->font, text);
    if (height) *height = font_get_line_height(ctx->font);
    if (ascent) *ascent = font_get_ascent(ctx->font);
    if (descent) *descent = font_get_descent(ctx->font);
}
