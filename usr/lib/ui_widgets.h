// usr/lib/ui_widgets.h - Shared widget library for CamelOS surfaces.
//
// Classic Aqua (Mac OS X 10.4-10.6) inspired widget set, theme-aware,
// usable from BOTH the running system (usr/bubbleview.c, screenlock.c,
// welcome_setup.c, desktop.c, ...) AND the standalone installer
// (installer/installer_main.c).
//
// All widgets take mouse state explicitly so they don't depend on a
// specific input subsystem — the caller passes whatever it has.
//
// All widgets source their colors from core/theme.h so a single
// theme_set() flips the whole UI light/dark.

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "../../hal/video/gfx_hal.h"
#include "../../core/theme.h"

// ── Mouse state passed by caller ──
// (so the same widget code works in the system, where sys_mouse_read()
// is used, AND in the installer, which has its own poll_input() globals)
typedef struct {
    int x, y;           // pointer position
    int left_down;      // 1 while button is held
    int clicked;        // 1 on the rising edge of a click (consumed)
} widget_mouse_t;

// ── Button variants ──
typedef enum {
    BTN_PRIMARY,        // accent-color fill, white text
    BTN_SECONDARY,      // card-bg fill, dark text, subtle border
    BTN_DESTRUCTIVE,    // error-color fill, white text
    BTN_GHOST,          // no fill, accent text only (hover = light highlight)
    BTN_PILL_PRIMARY,   // pill-shaped primary (radius = h/2)
    BTN_PILL_SECONDARY, // pill-shaped secondary
} widget_button_style_t;

// ── Draw a button. Returns 1 on click (consumes the click). ──
int  widget_button(int x, int y, int w, int h,
                   const char* label,
                   widget_button_style_t style,
                   const widget_mouse_t* m);

// ── Draw a disabled button (no interaction). ──
void widget_button_disabled(int x, int y, int w, int h,
                            const char* label,
                            widget_button_style_t style);

// ── Card / panel surface (shadow + rounded white body + 1px border). ──
void widget_card(int x, int y, int w, int h);

// ── Pinstriped card (Aqua utility window surface). ──
void widget_card_pinstriped(int x, int y, int w, int h);

// ── Text field (input box). ──
// `text`        current value (may be empty)
// `placeholder` shown when text is empty (may be NULL)
// `focused`     1 if this field has keyboard focus (draws accent ring)
void widget_text_field(int x, int y, int w, int h,
                       const char* text, const char* placeholder,
                       int focused);

// ── Password field — shows dots instead of text. ──
void widget_password_field(int x, int y, int w, int h,
                           const char* text, const char* placeholder,
                           int focused);

// ── Progress bar with Aqua gloss. ──
// pct: 0..100
// color: fill color (use theme->accent_color, success_color, etc.)
void widget_progress_bar(int x, int y, int w, int h,
                         int pct, uint32_t color);

// ── Pill badge (small status indicator). ──
// e.g. "Good 92%" with success_color background.
void widget_pill_badge(int x, int y, const char* label, uint32_t color);

// ── Horizontal separator line. ──
void widget_separator(int x, int y, int w);

// ── Breadcrumb dots (wizard step indicator). ──
// steps[]: labels for each step (NULL = no label)
// current: 0-based index of the active step
// count:   total number of steps
void widget_breadcrumb(int y, const char* steps[], int count, int current);

// ── Modal overlay — dims the screen and draws a centered card. ──
// Returns (x, y, w, h) of the modal card via out_ params.
void widget_modal_begin(int card_w, int card_h,
                        int* out_x, int* out_y);

// ── Aqua-specific overlays (used internally + by callers that
//    want to add the gloss/pinstripe to their own custom drawings) ──
void widget_gloss_overlay(int x, int y, int w, int h, int radius);
void widget_pinstripe_overlay(int x, int y, int w, int h);

// ── Brushed-metal fill (Aqua 10.4 utility window background). ──
// Draws a vertical gradient from metal_light to metal_dark with
// faint horizontal streaks.
void widget_brushed_metal(int x, int y, int w, int h);

// ── Aqua "sphere" background (installer/welcome hero). ──
// Draws the classic Mac OS X installer streak — a soft circular
// highlight that suggests a 3D sphere over a pinstriped background.
void widget_aqua_sphere_bg(int x, int y, int w, int h);

// ── Drop shadow (multi-layer, themeable). ──
void widget_shadow(int x, int y, int w, int h, int radius);

// ── Section header label (small caps style, secondary color). ──
void widget_section_label(int x, int y, const char* label);

// ── Status icon (circle with optional checkmark / cross / ! ). ──
typedef enum {
    STATUS_NEUTRAL,    // grey circle
    STATUS_OK,         // green circle + checkmark
    STATUS_WARN,       // orange circle + !
    STATUS_ERROR,      // red circle + X
} widget_status_t;
void widget_status_icon(int cx, int cy, int r, widget_status_t s);

#endif // UI_WIDGETS_H
