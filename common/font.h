#ifndef FONT_H
#define FONT_H

typedef unsigned char uint8_t;

// Standard ASCII font (chars 32-127)
extern const uint8_t font_8x16[96][16];

// Latin-1 Supplement font (chars 160-255, ISO 8859-1)
// Index 0 = char 160, index 95 = char 255
extern const uint8_t font_latin1_8x16[96][16];

// Helper: check if a character code has a renderable glyph
static inline int font_has_glyph(int c) {
    return (c >= 32 && c <= 127) || (c >= 160 && c <= 255);
}

// Helper: get font data pointer for a character code
// Renamed from font_get_glyph to avoid conflict with font_v2.h's
// Glyph* font_get_glyph(FontFace*, uint32_t).
static inline const uint8_t* font_get_glyph_bitmap(int c) {
    if (c >= 32 && c <= 127) return font_8x16[c - 32];
    if (c >= 160 && c <= 255) return font_latin1_8x16[c - 160];
    return font_8x16[0]; // fallback to space
}

#endif