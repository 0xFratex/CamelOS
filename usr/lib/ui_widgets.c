// usr/lib/ui_widgets.c - Shared widget library for CamelOS surfaces.
//
// Classic Aqua (Mac OS X 10.4-10.6) inspired widget set:
//   * glossy candy buttons with top-half sheen
//   * pinstriped utility surfaces (rendered procedurally — 1px lines every 2px)
//   * soft multi-layer drop shadows
//   * Aqua "sphere" hero background (radial highlight on a streaked field)
//   * brushed-metal utility chrome
//
// Every primitive sources its colors from core/theme.h so a single
// theme_set() flips the whole UI light/dark. Mouse state is passed
// explicitly so the same code works in the system (sys_mouse_read)
// and the installer (poll_input globals).

#include "ui_widgets.h"
#include "../../core/string.h"
#include "../../core/memory.h"

// ─────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────

static int hit(int x, int y, int w, int h, const widget_mouse_t* m) {
    return m && m->x >= x && m->x < x + w && m->y >= y && m->y < y + h;
}

static int str_width(const char* s) {
    return (int)strlen(s) * 8;   // matches gfx_draw_string advance
}

// ─────────────────────────────────────────────────────────────────────────
// Drop shadow (4-layer, themeable)
// ─────────────────────────────────────────────────────────────────────────

void widget_shadow(int x, int y, int w, int h, int radius) {
    const theme_t* t = theme_get_current();
    // Outermost (large, very light)
    gfx_fill_rounded_rect_aa(x - 3 + 6, y - 3 + 6, w + 6, h + 6,
                             t->shadow_soft, radius + 6);
    // Middle
    gfx_fill_rounded_rect_aa(x - 2 + 4, y - 2 + 4, w + 4, h + 4,
                             t->shadow_medium, radius + 4);
    // Inner
    gfx_fill_rounded_rect_aa(x - 1 + 2, y - 1 + 2, w + 2, h + 2,
                             t->shadow_medium, radius + 2);
    // Contact
    gfx_fill_rounded_rect_aa(x, y + h, w, 4, t->shadow_soft, 2);
}

// ─────────────────────────────────────────────────────────────────────────
// Gloss overlay (Aqua candy-button top-half sheen)
// Draws a translucent white rounded rect over the top ~50% of the bounds.
// ─────────────────────────────────────────────────────────────────────────

void widget_gloss_overlay(int x, int y, int w, int h, int radius) {
    const theme_t* t = theme_get_current();
    int gh = (h - 2) / 2;
    if (gh <= 2 || w <= 4) return;
    int inner_r = radius - 1;
    if (inner_r < 1) inner_r = 1;
    gfx_fill_rounded_rect(x + 2, y + 2, w - 4, gh, t->gloss_highlight, inner_r);
}

// ─────────────────────────────────────────────────────────────────────────
// Pinstripe overlay (Aqua 10.4 utility window pinstripes)
// Draws subtle horizontal 1px lines every 2px. Very low alpha.
// ─────────────────────────────────────────────────────────────────────────

void widget_pinstripe_overlay(int x, int y, int w, int h) {
    const theme_t* t = theme_get_current();
    for (int dy = 0; dy < h; dy += 2) {
        gfx_fill_rect(x, y + dy, w, 1, t->pinstripe);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Brushed metal (vertical gradient + faint horizontal streaks)
// ─────────────────────────────────────────────────────────────────────────

void widget_brushed_metal(int x, int y, int w, int h) {
    const theme_t* t = theme_get_current();
    // Vertical gradient
    uint8_t tr = (t->metal_light >> 16) & 0xFF;
    uint8_t tg = (t->metal_light >> 8) & 0xFF;
    uint8_t tb = t->metal_light & 0xFF;
    uint8_t br = (t->metal_dark  >> 16) & 0xFF;
    uint8_t bg = (t->metal_dark  >> 8) & 0xFF;
    uint8_t bb = t->metal_dark & 0xFF;
    for (int dy = 0; dy < h; dy++) {
        uint8_t r = tr + ((br - tr) * dy) / (h > 1 ? (h - 1) : 1);
        uint8_t g = tg + ((bg - tg) * dy) / (h > 1 ? (h - 1) : 1);
        uint8_t b = tb + ((bb - tb) * dy) / (h > 1 ? (h - 1) : 1);
        gfx_fill_rect(x, y + dy, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }
    // Faint horizontal streaks every 3px (brushed effect)
    for (int dy = 0; dy < h; dy += 3) {
        gfx_fill_rect(x, y + dy, w, 1, 0x08000000);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Aqua sphere background — installer/welcome hero
// Procedurally draws the classic Mac OS X installer "streak":
//   * soft pinstriped background
//   * large radial highlight suggesting a sphere lit from upper-left
// ─────────────────────────────────────────────────────────────────────────

void widget_aqua_sphere_bg(int x, int y, int w, int h) {
    const theme_t* t = theme_get_current();

    // 1. Vertical base gradient (page_bg → page_bg_bottom)
    uint8_t tr = (t->page_bg >> 16) & 0xFF;
    uint8_t tg = (t->page_bg >> 8) & 0xFF;
    uint8_t tb = t->page_bg & 0xFF;
    uint8_t br = (t->page_bg_bottom >> 16) & 0xFF;
    uint8_t bg = (t->page_bg_bottom >> 8) & 0xFF;
    uint8_t bb = t->page_bg_bottom & 0xFF;
    for (int dy = 0; dy < h; dy++) {
        uint8_t r = tr + ((br - tr) * dy) / (h > 1 ? (h - 1) : 1);
        uint8_t g = tg + ((bg - tg) * dy) / (h > 1 ? (h - 1) : 1);
        uint8_t b = tb + ((bb - tb) * dy) / (h > 1 ? (h - 1) : 1);
        gfx_fill_rect(x, y + dy, w, 1, 0xFF000000 | (r << 16) | (g << 8) | b);
    }

    // 2. Subtle pinstripes (Aqua utility look)
    widget_pinstripe_overlay(x, y, w, h);

    // 3. Sphere highlight — large soft radial gradient using stacked
    //    translucent circles. Center near upper-third, offset left.
    int cx = x + w / 2;
    int cy = y + h / 3;
    int max_r = (w > h ? w : h) * 3 / 4;
    if (max_r < 100) max_r = 100;

    uint32_t top_facet = t->aqua_sphere_top;   // lighter
    uint32_t bot_facet = t->aqua_sphere_bot;   // darker

    // Draw concentric translucent circles, fading from top_facet at
    // the rim to bot_facet toward the center, producing a soft 3D bulge.
    for (int r = max_r; r > 0; r -= 8) {
        uint32_t col;
        // Outer rings = lighter, inner = darker (suggests lit sphere top)
        if (r > max_r * 2 / 3) {
            col = top_facet;
        } else if (r > max_r / 3) {
            // Lerp top_facet → bot_facet
            int f = (max_r * 2 / 3 - r) * 256 / (max_r / 3 + 1);
            uint8_t a = (top_facet >> 24) & 0xFF;
            uint8_t tr2 = (top_facet >> 16) & 0xFF;
            uint8_t tg2 = (top_facet >> 8) & 0xFF;
            uint8_t tb2 = top_facet & 0xFF;
            uint8_t br2 = (bot_facet >> 16) & 0xFF;
            uint8_t bg2 = (bot_facet >> 8) & 0xFF;
            uint8_t bb2 = bot_facet & 0xFF;
            uint8_t r2 = tr2 + ((br2 - tr2) * f) / 256;
            uint8_t g2 = tg2 + ((bg2 - tg2) * f) / 256;
            uint8_t b2 = tb2 + ((bb2 - tb2) * f) / 256;
            col = ((uint32_t)a << 24) | (r2 << 16) | (g2 << 8) | b2;
        } else {
            col = bot_facet;
        }
        gfx_fill_rect(cx - r, cy - r / 2, r * 2, r, col);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Buttons
// ─────────────────────────────────────────────────────────────────────────

int widget_button(int x, int y, int w, int h,
                  const char* label,
                  widget_button_style_t style,
                  const widget_mouse_t* m) {
    const theme_t* t = theme_get_current();

    int is_pill = (style == BTN_PILL_PRIMARY || style == BTN_PILL_SECONDARY);
    int radius = is_pill ? (h / 2) : AQUA_RADIUS_BUTTON;
    if (radius < 1) radius = 1;

    uint32_t fill, text_col, border_col;
    switch (style) {
        case BTN_PRIMARY:
        case BTN_PILL_PRIMARY:
            fill = t->accent_color; text_col = 0xFFFFFFFF;
            border_col = theme_darken(t->accent_color, 60);
            break;
        case BTN_DESTRUCTIVE:
            fill = t->danger_color; text_col = 0xFFFFFFFF;
            border_col = theme_darken(t->danger_color, 60);
            break;
        case BTN_GHOST:
            fill = 0x00000000; text_col = t->accent_color;
            border_col = 0x00000000;
            break;
        case BTN_SECONDARY:
        case BTN_PILL_SECONDARY:
        default:
            fill = t->card_bg; text_col = t->text_primary;
            border_col = t->card_border;
            break;
    }

    int hover   = hit(x, y, w, h, m);
    int pressed = (hover && m && m->left_down);
    int clicked = (hover && m && m->clicked);

    // Hover darkens the fill (skip for ghost — it lights up instead)
    if (hover && !pressed && style != BTN_GHOST) {
        fill = theme_darken(fill, 12);
    }
    if (pressed && style != BTN_GHOST) {
        fill = theme_darken(fill, 20);
    }

    int yy = y + (pressed ? 1 : 0);

    if (style == BTN_GHOST) {
        // Ghost: only show a subtle highlight on hover
        if (hover) {
            gfx_fill_rounded_rect(x, y, w, h, theme_alpha(t->accent_color, 0x18), radius);
        }
    } else {
        // Shadow
        gfx_fill_rounded_rect(x + 2, yy + 3, w, h, t->shadow_medium, radius);
        // Border
        gfx_fill_rounded_rect(x, yy, w, h, border_col, radius);
        // Body (inset 1px)
        int ir = radius - 1; if (ir < 1) ir = 1;
        gfx_fill_rounded_rect(x + 1, yy + 1, w - 2, h - 2, fill, ir);
        // Top-half gloss
        if (h > 8) {
            int gh = (h - 2) / 2;
            gfx_fill_rounded_rect(x + 2, yy + 2, w - 4, gh,
                                  t->gloss_highlight, ir - 1 > 0 ? ir - 1 : 1);
        }
    }

    // Label — centered
    if (label && label[0]) {
        int tw = str_width(label);
        int tx = x + (w - tw) / 2;
        int ty = yy + (h - 16) / 2;
        // Subtle text shadow for primary buttons on saturated fills
        if (style == BTN_PRIMARY || style == BTN_PILL_PRIMARY || style == BTN_DESTRUCTIVE) {
            gfx_draw_string(tx + 1, ty + 1, label, 0x40000000);
        }
        gfx_draw_string(tx, ty, label, text_col);
    }

    return clicked ? 1 : 0;
}

void widget_button_disabled(int x, int y, int w, int h,
                            const char* label,
                            widget_button_style_t style) {
    const theme_t* t = theme_get_current();
    int is_pill = (style == BTN_PILL_PRIMARY || style == BTN_PILL_SECONDARY);
    int radius = is_pill ? (h / 2) : AQUA_RADIUS_BUTTON;
    if (radius < 1) radius = 1;

    uint32_t fill = theme_alpha(t->card_bg, 0x80);
    uint32_t border = theme_alpha(t->card_border, 0x80);
    uint32_t text_col = theme_alpha(t->text_secondary, 0x80);

    gfx_fill_rounded_rect(x, y, w, h, border, radius);
    int ir = radius - 1; if (ir < 1) ir = 1;
    gfx_fill_rounded_rect(x + 1, y + 1, w - 2, h - 2, fill, ir);

    if (label) {
        int tw = str_width(label);
        gfx_draw_string(x + (w - tw) / 2, y + (h - 16) / 2, label, text_col);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Card / panel
// ─────────────────────────────────────────────────────────────────────────

void widget_card(int x, int y, int w, int h) {
    const theme_t* t = theme_get_current();
    // Multi-layer shadow
    gfx_fill_rounded_rect_aa(x - 1 + 4, y - 1 + 4, w + 2, h + 2,
                             t->shadow_medium, AQUA_RADIUS_CARD + 2);
    gfx_fill_rounded_rect_aa(x - 1 + 2, y - 1 + 2, w + 2, h + 2,
                             t->shadow_soft, AQUA_RADIUS_CARD + 2);
    // Body
    gfx_fill_rounded_rect_aa(x, y, w, h, t->card_bg, AQUA_RADIUS_CARD);
    // Border
    gfx_stroke_rounded_rect(x, y, w, h, t->card_border, AQUA_RADIUS_CARD, 1);
}

void widget_card_pinstriped(int x, int y, int w, int h) {
    const theme_t* t = theme_get_current();
    // Shadow
    gfx_fill_rounded_rect_aa(x - 1 + 3, y - 1 + 3, w + 2, h + 2,
                             t->shadow_soft, AQUA_RADIUS_CARD + 2);
    // Body
    gfx_fill_rounded_rect_aa(x, y, w, h, t->sidebar_bg, AQUA_RADIUS_CARD);
    // Pinstripes
    widget_pinstripe_overlay(x, y, w, h);
    // Border
    gfx_stroke_rounded_rect(x, y, w, h, t->card_border, AQUA_RADIUS_CARD, 1);
}

// ─────────────────────────────────────────────────────────────────────────
// Text fields
// ─────────────────────────────────────────────────────────────────────────

void widget_text_field(int x, int y, int w, int h,
                       const char* text, const char* placeholder,
                       int focused) {
    const theme_t* t = theme_get_current();

    // Shadow
    gfx_fill_rounded_rect(x + 1, y + 1, w, h, t->shadow_soft, AQUA_RADIUS_BUTTON);
    // Body
    gfx_fill_rounded_rect(x, y, w, h, t->input_bg, AQUA_RADIUS_BUTTON);
    // Border (accent if focused)
    uint32_t border = focused ? t->accent_color : t->input_border;
    gfx_stroke_rounded_rect(x, y, w, h, border, AQUA_RADIUS_BUTTON, focused ? 2 : 1);

    // Text or placeholder
    int tx = x + 8;
    int ty = y + (h - 16) / 2;
    if (text && text[0]) {
        gfx_draw_string(tx, ty, text, t->text_primary);
    } else if (placeholder) {
        gfx_draw_string(tx, ty, placeholder, t->text_secondary);
    }
}

void widget_password_field(int x, int y, int w, int h,
                           const char* text, const char* placeholder,
                           int focused) {
    const theme_t* t = theme_get_current();

    gfx_fill_rounded_rect(x + 1, y + 1, w, h, t->shadow_soft, AQUA_RADIUS_BUTTON);
    gfx_fill_rounded_rect(x, y, w, h, t->input_bg, AQUA_RADIUS_BUTTON);
    uint32_t border = focused ? t->accent_color : t->input_border;
    gfx_stroke_rounded_rect(x, y, w, h, border, AQUA_RADIUS_BUTTON, focused ? 2 : 1);

    int tx = x + 8;
    int ty = y + (h - 16) / 2;
    if (text && text[0]) {
        // Draw dots
        int n = (int)strlen(text);
        int dot_r = 3;
        int spacing = 10;
        for (int i = 0; i < n; i++) {
            int dx = tx + i * spacing + dot_r;
            int dy = ty + 8;
            // Simple filled circle (4 stacked rects for AA-ish look)
            gfx_fill_rect(dx - dot_r, dy - 1, dot_r * 2, 3, t->text_primary);
            gfx_fill_rect(dx - 1, dy - dot_r, 3, dot_r * 2, t->text_primary);
        }
    } else if (placeholder) {
        gfx_draw_string(tx, ty, placeholder, t->text_secondary);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Progress bar (Aqua gloss)
// ─────────────────────────────────────────────────────────────────────────

void widget_progress_bar(int x, int y, int w, int h,
                         int pct, uint32_t color) {
    const theme_t* t = theme_get_current();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int r = h / 2;
    if (r < 2) r = 2;

    // Shadow
    gfx_fill_rounded_rect(x + 1, y + 2, w, h, t->shadow_soft, r);
    // White pill background
    gfx_fill_rounded_rect(x, y, w, h, 0xFFFFFFFF, r);
    // Border
    gfx_stroke_rounded_rect(x, y, w, h, t->card_border, r, 1);

    // Filled portion
    if (pct > 0) {
        int fw = (w * pct) / 100;
        if (fw < h) fw = h;   // never narrower than the radius
        gfx_fill_rounded_rect(x, y, fw, h, color, r);
        // Gloss on top half of fill
        int gh = (h - 2) / 2;
        if (gh > 1) {
            gfx_fill_rounded_rect(x + 2, y + 1, fw - 4, gh,
                                  t->gloss_highlight, r - 1 > 0 ? r - 1 : 1);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Pill badge
// ─────────────────────────────────────────────────────────────────────────

void widget_pill_badge(int x, int y, const char* label, uint32_t color) {
    if (!label) return;
    int tw = str_width(label);
    int pad_x = 8;
    int w = tw + pad_x * 2;
    int h = 18;
    int r = h / 2;

    gfx_fill_rounded_rect(x, y, w, h, color, r);
    // Tiny gloss
    gfx_fill_rounded_rect(x + 2, y + 1, w - 4, h / 2 - 1,
                          theme_alpha(0xFFFFFF, 0x55), r - 1 > 0 ? r - 1 : 1);

    uint32_t text_col = theme_is_light(color) ? 0xFF1C1C1E : 0xFFFFFFFF;
    gfx_draw_string(x + pad_x, y + (h - 16) / 2 + 1, label, text_col);
}

// ─────────────────────────────────────────────────────────────────────────
// Separator
// ─────────────────────────────────────────────────────────────────────────

void widget_separator(int x, int y, int w) {
    const theme_t* t = theme_get_current();
    gfx_fill_rect(x, y, w, 1, t->separator);
}

// ─────────────────────────────────────────────────────────────────────────
// Breadcrumb dots
// ─────────────────────────────────────────────────────────────────────────

void widget_breadcrumb(int y, const char* steps[], int count, int current) {
    const theme_t* t = theme_get_current();
    if (count <= 0) return;

    int sw = gfx_get_width();
    int dot_r = 11;
    int dot_spacing = 28;
    int total_w = (count - 1) * dot_spacing;
    int start_x = (sw - total_w) / 2;

    // Connecting line
    if (count > 1) {
        gfx_fill_rect(start_x + dot_r, y + dot_r - 1,
                      total_w - dot_r * 2 + 2, 2, t->separator);
    }

    for (int i = 0; i < count; i++) {
        int cx = start_x + i * dot_spacing;
        int done = (i < current);
        int active = (i == current);

        uint32_t fill;
        uint32_t text_col;
        if (done) {
            fill = t->success_color;
            text_col = 0xFFFFFFFF;
        } else if (active) {
            fill = t->accent_color;
            text_col = 0xFFFFFFFF;
        } else {
            fill = t->card_bg;
            text_col = t->text_secondary;
        }

        // Shadow for the dot
        gfx_fill_rounded_rect_aa(cx + 1, y + 2, dot_r * 2, dot_r * 2,
                                 t->shadow_soft, dot_r);
        gfx_fill_rounded_rect_aa(cx, y, dot_r * 2, dot_r * 2, fill, dot_r);
        gfx_stroke_rounded_rect(cx, y, dot_r * 2, dot_r * 2,
                                t->card_border, dot_r, 1);

        // Number or checkmark
        char buf[4];
        if (done) {
            // Checkmark
            int px = cx + dot_r;
            int py = y + dot_r;
            gfx_draw_line(px - 4, py,     px - 1, py + 3, 0xFFFFFFFF);
            gfx_draw_line(px - 1, py + 3, px + 4, py - 3, 0xFFFFFFFF);
        } else {
            // Number
            buf[0] = '1' + i;
            buf[1] = 0;
            int tw = str_width(buf);
            gfx_draw_string(cx + dot_r - tw / 2, y + dot_r - 8, buf, text_col);
        }

        // Label under dot (if provided)
        if (steps && steps[i]) {
            int tw = str_width(steps[i]);
            gfx_draw_string(cx + dot_r - tw / 2, y + dot_r * 2 + 6,
                            steps[i], t->text_secondary);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Modal overlay — dim background + return centered card bounds
// Caller is responsible for drawing the card itself (typically via widget_card).
// ─────────────────────────────────────────────────────────────────────────

void widget_modal_begin(int card_w, int card_h,
                        int* out_x, int* out_y) {
    const theme_t* t = theme_get_current();
    int sw = gfx_get_width();
    int sh = gfx_get_height();

    // Dim background
    gfx_fill_rect(0, 0, sw, sh, t->modal_dim);

    // Centered card position
    *out_x = (sw - card_w) / 2;
    *out_y = (sh - card_h) / 2;
}

// ─────────────────────────────────────────────────────────────────────────
// Section label (small caps style, secondary color)
// ─────────────────────────────────────────────────────────────────────────

void widget_section_label(int x, int y, const char* label) {
    const theme_t* t = theme_get_current();
    if (!label) return;
    gfx_draw_string(x, y, label, t->text_secondary);
}

// ─────────────────────────────────────────────────────────────────────────
// Status icon
// ─────────────────────────────────────────────────────────────────────────

void widget_status_icon(int cx, int cy, int r, widget_status_t s) {
    const theme_t* t = theme_get_current();
    uint32_t col;
    switch (s) {
        case STATUS_OK:    col = t->success_color; break;
        case STATUS_WARN:  col = t->warning_color; break;
        case STATUS_ERROR: col = t->error_color;   break;
        case STATUS_NEUTRAL:
        default:           col = t->text_secondary; break;
    }
    // Shadow
    gfx_fill_rounded_rect_aa(cx - r + 1, cy - r + 2, r * 2, r * 2,
                             t->shadow_soft, r);
    // Body
    gfx_fill_rounded_rect_aa(cx - r, cy - r, r * 2, r * 2, col, r);
    // Gloss
    gfx_fill_rounded_rect(cx - r + 2, cy - r + 1, r * 2 - 4, r - 1,
                          t->gloss_highlight, r - 1 > 0 ? r - 1 : 1);

    // Glyph
    int gx = cx - r;
    int gy = cy - r;
    int gw = r * 2;
    if (s == STATUS_OK) {
        // Checkmark
        gfx_draw_line(gx + gw * 3 / 10, gy + gw / 2,
                      gx + gw * 45 / 100, gy + gw * 65 / 100, 0xFFFFFFFF);
        gfx_draw_line(gx + gw * 45 / 100, gy + gw * 65 / 100,
                      gx + gw * 75 / 100, gy + gw * 30 / 100, 0xFFFFFFFF);
    } else if (s == STATUS_WARN || s == STATUS_ERROR) {
        // Exclamation
        gfx_fill_rect(cx - 1, cy - r / 2, 2, r, 0xFFFFFFFF);
        gfx_fill_rect(cx - 1, cy + r / 2 + 2, 2, 2, 0xFFFFFFFF);
    }
}
