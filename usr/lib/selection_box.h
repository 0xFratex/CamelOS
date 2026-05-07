#ifndef SELECTION_BOX_H
#define SELECTION_BOX_H

#include "../../sys/api.h"
#include "../../hal/video/gfx_hal.h"

// Selection box states
#define SELBOX_INACTIVE  0
#define SELBOX_DRAGGING  1
#define SELBOX_COMPLETED 2

typedef struct {
    int state;              // SELBOX_*
    int start_x, start_y;  // Screen coords where drag started
    int cur_x, cur_y;      // Current mouse position
    uint32_t color;        // Selection rectangle fill color (default: 0x40007AFF)
    uint32_t border_color; // Border color (default: 0xFF007AFF)
    int min_drag;          // Minimum pixels before activating (default: 4)
    int active;            // 1 if the selection box has been initialized
} selection_box_t;

// Initialize selection box with default values
void selbox_init(selection_box_t* sb);

// Begin a selection drag at the given screen coordinates
void selbox_start(selection_box_t* sb, int x, int y);

// Update the current mouse position during a drag
void selbox_update(selection_box_t* sb, int x, int y);

// Finish the selection drag (user released mouse button)
void selbox_end(selection_box_t* sb);

// Cancel the selection (e.g. right-click or ESC)
void selbox_cancel(selection_box_t* sb);

// Draw the selection rectangle if active
void selbox_draw(selection_box_t* sb);

// Get the normalized selection rect (handles any drag direction)
// Returns 1 if selection is active, fills x/y/w/h with the normalized rect
int selbox_get_rect(selection_box_t* sb, int* x, int* y, int* w, int* h);

// Check if a point is inside the current selection rectangle
// Returns 1 if inside, 0 if not or if selection is inactive
int selbox_contains_point(selection_box_t* sb, int px, int py);

// Set custom fill and border colors
void selbox_set_colors(selection_box_t* sb, uint32_t fill, uint32_t border);

#endif
