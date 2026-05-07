// usr/lib/selection_box.c - Reusable rubber-band selection box API
// Provides a macOS Finder/Desktop-style selection rectangle that multiple
// apps can share: desktop, files app, and any future apps.
#include "selection_box.h"
#include "../../core/string.h"
#include "../../core/memory.h"
#include "../../hal/drivers/serial.h"

// ============================================================================
// Internal helper: compute absolute value
// ============================================================================
static int _abs(int v) {
    return v < 0 ? -v : v;
}

// ============================================================================
// selbox_init - Initialize selection box with default values
// ============================================================================
void selbox_init(selection_box_t* sb) {
    if (!sb) return;

    // Zero out the entire struct
    memset(sb, 0, sizeof(selection_box_t));

    // Default colors: semi-transparent blue fill, opaque blue border
    sb->color        = 0x40007AFF;
    sb->border_color = 0xFF007AFF;

    // Minimum drag distance before the box visually activates
    sb->min_drag = 4;

    // State is inactive until selbox_start is called
    sb->state  = SELBOX_INACTIVE;
    sb->active = 1;
}

// ============================================================================
// selbox_start - Begin a selection drag at (x, y)
// ============================================================================
void selbox_start(selection_box_t* sb, int x, int y) {
    if (!sb || !sb->active) return;

    sb->start_x = x;
    sb->start_y = y;
    sb->cur_x   = x;
    sb->cur_y   = y;
    sb->state    = SELBOX_DRAGGING;

    s_printf("[selbox] start at (%d, %d)\n", x, y);
}

// ============================================================================
// selbox_update - Update the current mouse position during a drag
// ============================================================================
void selbox_update(selection_box_t* sb, int x, int y) {
    if (!sb || !sb->active) return;
    if (sb->state != SELBOX_DRAGGING) return;

    sb->cur_x = x;
    sb->cur_y = y;
}

// ============================================================================
// selbox_end - Finish the selection drag
// ============================================================================
void selbox_end(selection_box_t* sb) {
    if (!sb || !sb->active) return;
    if (sb->state != SELBOX_DRAGGING) return;

    // Set to INACTIVE so the rubber-band visual disappears on release.
    // Selected items remain highlighted via their own selection arrays
    // (desk_selected[] on desktop, is_selected[] in files app) which
    // are independent of the selbox state.
    sb->state = SELBOX_INACTIVE;

    s_printf("[selbox] end at (%d, %d)\n", sb->cur_x, sb->cur_y);
}

// ============================================================================
// selbox_cancel - Cancel the selection (e.g. right-click or ESC)
// ============================================================================
void selbox_cancel(selection_box_t* sb) {
    if (!sb) return;

    sb->state = SELBOX_INACTIVE;
}

// ============================================================================
// selbox_get_rect - Get the normalized selection rectangle
// Handles any drag direction (up-left, down-right, etc.)
// Returns 1 if selection is active, fills x/y/w/h with normalized rect
// ============================================================================
int selbox_get_rect(selection_box_t* sb, int* x, int* y, int* w, int* h) {
    if (!sb) return 0;
    if (sb->state != SELBOX_DRAGGING) return 0;

    // Normalize: find the top-left corner regardless of drag direction
    int left   = sb->start_x < sb->cur_x ? sb->start_x : sb->cur_x;
    int top    = sb->start_y < sb->cur_y ? sb->start_y : sb->cur_y;
    int width  = _abs(sb->cur_x - sb->start_x);
    int height = _abs(sb->cur_y - sb->start_y);

    if (x) *x = left;
    if (y) *y = top;
    if (w) *w = width;
    if (h) *h = height;

    return 1;
}

// ============================================================================
// selbox_contains_point - Check if a point is inside the selection
// Returns 1 if inside, 0 if not or if selection is inactive
// ============================================================================
int selbox_contains_point(selection_box_t* sb, int px, int py) {
    if (!sb) return 0;

    int rx, ry, rw, rh;
    if (!selbox_get_rect(sb, &rx, &ry, &rw, &rh)) return 0;

    // Point must be within the normalized rectangle bounds
    if (px >= rx && px < (rx + rw) && py >= ry && py < (ry + rh)) {
        return 1;
    }

    return 0;
}

// ============================================================================
// selbox_draw - Draw the selection rectangle if active
// Uses gfx_fill_rounded_rect for semi-transparent fill and gfx_draw_rect
// for the 1px border, giving a macOS Finder-style rubber-band appearance
// ============================================================================
void selbox_draw(selection_box_t* sb) {
    if (!sb || !sb->active) return;
    if (sb->state != SELBOX_DRAGGING) return;

    int rx, ry, rw, rh;
    if (!selbox_get_rect(sb, &rx, &ry, &rw, &rh)) return;

    // Only draw if the selection exceeds the minimum drag threshold in both
    // dimensions — this prevents tiny visual artifacts from accidental clicks
    if (rw < sb->min_drag || rh < sb->min_drag) return;

    // Draw filled semi-transparent rounded rect for the selection area
    // Corner radius of 4 gives a subtle macOS-style rounding
    gfx_fill_rounded_rect(rx, ry, rw, rh, sb->color, 4);

    // Draw 1px border around the selection
    gfx_draw_rect(rx, ry, rw, rh, sb->border_color);
}

// ============================================================================
// selbox_set_colors - Set custom fill and border colors
// ============================================================================
void selbox_set_colors(selection_box_t* sb, uint32_t fill, uint32_t border) {
    if (!sb) return;

    sb->color        = fill;
    sb->border_color = border;
}
