// core/notification_center.c - CamelOS Notification Center + Hub
// macOS-style notification system with slide-in banner and right-side panel
// Integrates with the existing core/notification.c toast system.

#include "notification_center.h"
#include "notification.h"
#include "theme.h"
#include "memory.h"
#include "../include/string.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/serial.h"

// ══════════════════════════════════════════════════════════════════
// Constants
// ══════════════════════════════════════════════════════════════════

#define BANNER_WIDTH      320
#define BANNER_HEIGHT     70
#define BANNER_MARGIN_X   16
#define BANNER_MARGIN_Y   36
#define BANNER_DURATION   180    // ~3 seconds at 60fps
#define BANNER_SLIDE_TICKS 15

#define CENTER_WIDTH      340
#define CENTER_MARGIN_X   0
#define CENTER_ITEM_H     72
#define CENTER_PADDING    16
#define CENTER_HEADER_H   44
#define CENTER_SLIDE_TICKS 12

// ══════════════════════════════════════════════════════════════════
// State
// ══════════════════════════════════════════════════════════════════

static notification_item_t notif_items[NOTIF_MAX_ITEMS];
static int notif_count = 0;

// Banner state
static notification_item_t active_banner;
static int banner_showing = 0;
static int banner_slide_in = 0;    // 0..BANNER_SLIDE_TICKS
static int banner_slide_out = 0;   // 0..BANNER_SLIDE_TICKS
static int banner_timer = 0;

// Center panel state
static int center_open = 0;
static int center_slide = 0;       // 0..CENTER_SLIDE_TICKS for animation

// ══════════════════════════════════════════════════════════════════
// Type → color mapping
// ══════════════════════════════════════════════════════════════════

static uint32_t notif_type_color(int type) {
    switch (type) {
        case NOTIF_TYPE_SUCCESS: return 0xFF34C759;  // Green
        case NOTIF_TYPE_WARNING: return 0xFFFF9500;  // Orange
        case NOTIF_TYPE_ERROR:   return 0xFFFF3B30;  // Red
        case NOTIF_TYPE_INFO:
        default:                 return 0xFF007AFF;  // Blue
    }
}

static const char* notif_type_icon(int type) {
    switch (type) {
        case NOTIF_TYPE_SUCCESS: return "OK";
        case NOTIF_TYPE_WARNING: return "!!";
        case NOTIF_TYPE_ERROR:   return "X";
        case NOTIF_TYPE_INFO:
        default:                 return "i";
    }
}

// ══════════════════════════════════════════════════════════════════
// Initialization
// ══════════════════════════════════════════════════════════════════

void notif_init(void) {
    memset(notif_items, 0, sizeof(notif_items));
    notif_count = 0;
    banner_showing = 0;
    banner_slide_in = 0;
    banner_slide_out = 0;
    banner_timer = 0;
    center_open = 0;
    center_slide = 0;
    s_printf("[NOTIF_CENTER] Initialized\n");
}

// ══════════════════════════════════════════════════════════════════
// Post a notification — shows banner + adds to history
// ══════════════════════════════════════════════════════════════════

void notif_post(const char* app_name, const char* title, const char* body, int type) {
    if (!title || !title[0]) return;

    // Also post to the existing toast system for the priority-based renderer
    notify_post(title, body ? body : "", app_name ? app_name : "System",
                NOTIFY_PRIORITY_NORMAL, NOTIFY_CAT_APP);

    // Add to our notification center history
    if (notif_count >= NOTIF_MAX_ITEMS) {
        // Shift items down (drop oldest)
        for (int i = 0; i < NOTIF_MAX_ITEMS - 1; i++) {
            notif_items[i] = notif_items[i + 1];
        }
        notif_count = NOTIF_MAX_ITEMS - 1;
    }

    notification_item_t* item = &notif_items[notif_count++];
    memset(item, 0, sizeof(notification_item_t));

    if (title) {
        strncpy(item->title, title, NOTIF_MAX_TITLE - 1);
        item->title[NOTIF_MAX_TITLE - 1] = '\0';
    }
    if (body) {
        strncpy(item->body, body, NOTIF_MAX_BODY - 1);
        item->body[NOTIF_MAX_BODY - 1] = '\0';
    }
    if (app_name) {
        strncpy(item->app_name, app_name, NOTIF_MAX_APP - 1);
        item->app_name[NOTIF_MAX_APP - 1] = '\0';
    }
    item->type = type;
    item->read = 0;
    item->active = 0;
    item->banner_timer = 0;

    // Get timestamp
    extern volatile uint32_t ticks;
    item->timestamp = ticks;

    // Show as banner
    active_banner = *item;
    active_banner.active = 1;
    active_banner.banner_timer = BANNER_DURATION;
    banner_showing = 1;
    banner_slide_in = 0;
    banner_slide_out = 0;

    s_printf("[NOTIF_CENTER] Posted: ");
    s_printf(title);
    s_printf("\n");
}

// ══════════════════════════════════════════════════════════════════
// Dismiss active banner
// ══════════════════════════════════════════════════════════════════

void notif_dismiss(void) {
    if (banner_showing && banner_slide_out == 0) {
        banner_slide_out = 1;  // Start slide-out animation
    }
}

// ══════════════════════════════════════════════════════════════════
// Center panel toggle
// ══════════════════════════════════════════════════════════════════

void notif_center_toggle(void) {
    center_open = !center_open;
    if (center_open) {
        center_slide = 0;  // Start slide-in
        // Mark all as read when opening
        for (int i = 0; i < notif_count; i++) {
            notif_items[i].read = 1;
        }
    }
}

int notif_center_is_open(void) {
    return center_open;
}

// ══════════════════════════════════════════════════════════════════
// Clear all notifications
// ══════════════════════════════════════════════════════════════════

void notif_clear_all(void) {
    notif_count = 0;
    memset(notif_items, 0, sizeof(notif_items));
}

// ══════════════════════════════════════════════════════════════════
// Unread count
// ══════════════════════════════════════════════════════════════════

int notif_unread_count(void) {
    int count = 0;
    for (int i = 0; i < notif_count; i++) {
        if (!notif_items[i].read) count++;
    }
    // Also count if banner is currently showing
    if (banner_showing) count++;
    return count;
}

// ══════════════════════════════════════════════════════════════════
// Tick — called each frame to update animations
// ══════════════════════════════════════════════════════════════════

void notif_tick(void) {
    // Banner slide-in animation
    if (banner_showing && banner_slide_in < BANNER_SLIDE_TICKS) {
        banner_slide_in++;
    }

    // Banner slide-out animation
    if (banner_showing && banner_slide_out > 0) {
        banner_slide_out++;
        if (banner_slide_out >= BANNER_SLIDE_TICKS) {
            banner_showing = 0;
            banner_slide_out = 0;
            banner_slide_in = 0;
        }
    }

    // Banner auto-dismiss timer
    if (banner_showing && banner_slide_out == 0) {
        banner_timer--;
        if (banner_timer <= 0) {
            notif_dismiss();
        }
    }

    // Center panel slide animation
    if (center_open && center_slide < CENTER_SLIDE_TICKS) {
        center_slide++;
    }
    if (!center_open && center_slide > 0) {
        center_slide--;
    }

    // Also tick the existing toast notification system
    notify_animate();
}

// ══════════════════════════════════════════════════════════════════
// Render the notification banner (top-right, slide in/out)
// ══════════════════════════════════════════════════════════════════

void notif_render_banner(void) {
    if (!banner_showing) return;

    int screen_w = gfx_get_width();
    const theme_t* theme = theme_get_current();

    // Calculate banner position
    int target_x = screen_w - BANNER_WIDTH - BANNER_MARGIN_X;
    int banner_x = target_x;
    int banner_y = BANNER_MARGIN_Y;

    // Slide-in animation offset
    if (banner_slide_in < BANNER_SLIDE_TICKS && banner_slide_out == 0) {
        int offset = (BANNER_SLIDE_TICKS - banner_slide_in) * BANNER_WIDTH / BANNER_SLIDE_TICKS;
        banner_x = target_x + offset;
    }
    // Slide-out animation offset
    if (banner_slide_out > 0) {
        int offset = banner_slide_out * BANNER_WIDTH / BANNER_SLIDE_TICKS;
        banner_x = target_x + offset;
    }

    uint32_t type_col = notif_type_color(active_banner.type);

    // Shadow
    gfx_fill_rounded_rect_aa(banner_x + 2, banner_y + 3,
                              BANNER_WIDTH, BANNER_HEIGHT,
                              0x20000000, 12);

    // Background (uses theme)
    uint32_t bg_col = theme->notification_bg;
    gfx_fill_rounded_rect_aa(banner_x, banner_y,
                              BANNER_WIDTH, BANNER_HEIGHT,
                              bg_col, 12);

    // Border
    gfx_draw_rect(banner_x, banner_y, BANNER_WIDTH, BANNER_HEIGHT, theme->notification_border);

    // Type indicator bar (left edge)
    gfx_fill_rounded_rect(banner_x + 6, banner_y + 8, 4, BANNER_HEIGHT - 16,
                           type_col, 2);

    // App name (small, muted)
    int text_x = banner_x + 18;
    gfx_draw_string(text_x, banner_y + 6, active_banner.app_name,
                    theme->text_secondary);

    // Title
    {
        int max_ch = (BANNER_WIDTH - 50) / 8;
        if (max_ch > 35) max_ch = 35;
        char tbuf[36];
        strncpy(tbuf, active_banner.title, max_ch);
        tbuf[max_ch] = '\0';
        gfx_draw_string(text_x, banner_y + 22, tbuf, theme->text_primary);
    }

    // Body (truncated)
    {
        int max_ch = (BANNER_WIDTH - 50) / 8;
        if (max_ch > 38) max_ch = 38;
        char bbuf[39];
        strncpy(bbuf, active_banner.body, max_ch);
        bbuf[max_ch] = '\0';
        gfx_draw_string(text_x, banner_y + 38, bbuf, theme->text_secondary);
    }

    // Close button (X)
    {
        int cx = banner_x + BANNER_WIDTH - 22;
        int cy = banner_y + 6;
        gfx_fill_rounded_rect(cx, cy, 16, 16, 0x40000000, 8);
        gfx_draw_line(cx + 4, cy + 4, cx + 11, cy + 11, theme->text_secondary);
        gfx_draw_line(cx + 11, cy + 4, cx + 4, cy + 11, theme->text_secondary);
    }

    // Glass highlight (top edge)
    gfx_draw_line(banner_x + 12, banner_y + 1,
                  banner_x + BANNER_WIDTH - 12, banner_y + 1,
                  0x18FFFFFF);
}

// ══════════════════════════════════════════════════════════════════
// Render the notification center panel (slides in from right)
// ══════════════════════════════════════════════════════════════════

void notif_render_center(void) {
    if (center_slide == 0 && !center_open) return;

    int screen_w = gfx_get_width();
    int screen_h = gfx_get_height();
    const theme_t* theme = theme_get_current();

    // Calculate slide position
    int target_x = screen_w - CENTER_WIDTH;
    int panel_x = target_x;
    if (center_slide < CENTER_SLIDE_TICKS) {
        int offset = (CENTER_SLIDE_TICKS - center_slide) * CENTER_WIDTH / CENTER_SLIDE_TICKS;
        panel_x = target_x + offset;
    }

    // Dim overlay (only when fully open or animating)
    int overlay_alpha = (center_slide * 0x40) / CENTER_SLIDE_TICKS;
    if (overlay_alpha > 0) {
        for (int y = 0; y < screen_h; y++) {
            if (y % 4 == 0) {
                for (int x = 0; x < panel_x; x++) {
                    if (x % 3 == 0) {
                        gfx_put_pixel(x, y, (overlay_alpha << 24));
                    }
                }
            }
        }
    }

    // Panel background
    gfx_fill_rect(panel_x, 0, CENTER_WIDTH, screen_h, theme->menubar_bg);
    // Right border
    gfx_draw_rect(panel_x, 0, 1, screen_h, theme->separator);

    // Header
    int cy = CENTER_PADDING;
    gfx_draw_string(panel_x + CENTER_PADDING, cy, "Notifications", theme->text_primary);
    cy += 18;

    // "Clear All" button
    {
        int btn_x = panel_x + CENTER_WIDTH - CENTER_PADDING - 80;
        int btn_y = CENTER_PADDING;
        gfx_fill_rounded_rect(btn_x, btn_y, 80, 22, theme->accent_color, 4);
        gfx_draw_string(btn_x + 10, btn_y + 5, "Clear All", 0xFFFFFFFF);
    }

    // Separator
    gfx_draw_line(panel_x + CENTER_PADDING, cy + 4,
                  panel_x + CENTER_WIDTH - CENTER_PADDING, cy + 4,
                  theme->separator);
    cy += 12;

    // Notification list
    if (notif_count == 0) {
        gfx_draw_string(panel_x + CENTER_PADDING, cy + 20,
                        "No Notifications", theme->text_secondary);
    } else {
        // Show most recent first (iterate backwards)
        int visible_count = notif_count;
        int max_visible = (screen_h - cy - 20) / (CENTER_ITEM_H + 8);
        if (visible_count > max_visible) visible_count = max_visible;

        for (int i = notif_count - 1; i >= notif_count - visible_count && i >= 0; i--) {
            notification_item_t* item = &notif_items[i];
            uint32_t type_col = notif_type_color(item->type);

            // Item background card
            gfx_fill_rounded_rect(panel_x + CENTER_PADDING, cy,
                                   CENTER_WIDTH - CENTER_PADDING * 2,
                                   CENTER_ITEM_H, theme->window_body, 8);
            gfx_draw_rect(panel_x + CENTER_PADDING, cy,
                          CENTER_WIDTH - CENTER_PADDING * 2,
                          CENTER_ITEM_H, theme->separator);

            // Type indicator (left bar)
            gfx_fill_rounded_rect(panel_x + CENTER_PADDING + 6, cy + 8, 4,
                                   CENTER_ITEM_H - 16, type_col, 2);

            int tx = panel_x + CENTER_PADDING + 18;

            // App name + timestamp
            gfx_draw_string(tx, cy + 6, item->app_name, theme->text_secondary);

            // Title (truncated)
            {
                int max_ch = (CENTER_WIDTH - 54) / 8;
                if (max_ch > 34) max_ch = 34;
                char tbuf[35];
                strncpy(tbuf, item->title, max_ch);
                tbuf[max_ch] = '\0';
                gfx_draw_string(tx, cy + 22, tbuf, theme->text_primary);
            }

            // Body (truncated)
            {
                int max_ch = (CENTER_WIDTH - 54) / 8;
                if (max_ch > 38) max_ch = 38;
                char bbuf[39];
                strncpy(bbuf, item->body, max_ch);
                bbuf[max_ch] = '\0';
                gfx_draw_string(tx, cy + 38, bbuf, theme->text_secondary);
            }

            // Dismiss (X) button for individual notification
            {
                int bx = panel_x + CENTER_WIDTH - CENTER_PADDING - 24;
                int by = cy + 6;
                gfx_fill_rounded_rect(bx, by, 16, 16, 0x20000000, 8);
                gfx_draw_line(bx + 4, by + 4, bx + 11, by + 11, theme->text_secondary);
                gfx_draw_line(bx + 11, by + 4, bx + 4, by + 11, theme->text_secondary);
            }

            // Unread indicator (blue dot)
            if (!item->read) {
                gfx_fill_rounded_rect(panel_x + CENTER_PADDING + 4, cy + 4,
                                       8, 8, theme->accent_color, 4);
            }

            cy += CENTER_ITEM_H + 8;
        }
    }
}

// ══════════════════════════════════════════════════════════════════
// Handle click in notification center / banner
// Returns 1 if click was consumed
// ══════════════════════════════════════════════════════════════════

int notif_handle_click(int x, int y) {
    int screen_w = gfx_get_width();

    // Check banner click first
    if (banner_showing) {
        int target_x = screen_w - BANNER_WIDTH - BANNER_MARGIN_X;
        int banner_y = BANNER_MARGIN_Y;

        // Adjust for slide animation
        int bx = target_x;
        if (banner_slide_in < BANNER_SLIDE_TICKS && banner_slide_out == 0) {
            int offset = (BANNER_SLIDE_TICKS - banner_slide_in) * BANNER_WIDTH / BANNER_SLIDE_TICKS;
            bx = target_x + offset;
        }
        if (banner_slide_out > 0) {
            int offset = banner_slide_out * BANNER_WIDTH / BANNER_SLIDE_TICKS;
            bx = target_x + offset;
        }

        if (x >= bx && x < bx + BANNER_WIDTH &&
            y >= banner_y && y < banner_y + BANNER_HEIGHT) {
            // Close button
            int cx = bx + BANNER_WIDTH - 22;
            int cy_pos = banner_y + 6;
            if (x >= cx && x < cx + 16 && y >= cy_pos && y < cy_pos + 16) {
                notif_dismiss();
                return 1;
            }
            // Click on banner body — could launch associated app
            return 1;
        }
    }

    // Check notification center panel click
    if (center_open && center_slide > 0) {
        int target_x = screen_w - CENTER_WIDTH;
        int panel_x = target_x;
        if (center_slide < CENTER_SLIDE_TICKS) {
            int offset = (CENTER_SLIDE_TICKS - center_slide) * CENTER_WIDTH / CENTER_SLIDE_TICKS;
            panel_x = target_x + offset;
        }

        if (x >= panel_x) {
            // Click inside the panel

            // "Clear All" button
            int btn_x = panel_x + CENTER_WIDTH - CENTER_PADDING - 80;
            int btn_y = CENTER_PADDING;
            if (x >= btn_x && x < btn_x + 80 && y >= btn_y && y < btn_y + 22) {
                notif_clear_all();
                return 1;
            }

            // Individual notification items
            int cy = CENTER_PADDING + 18 + 12;
            int max_visible = (gfx_get_height() - cy - 20) / (CENTER_ITEM_H + 8);
            int visible_count = notif_count;
            if (visible_count > max_visible) visible_count = max_visible;

            for (int i = notif_count - 1; i >= notif_count - visible_count && i >= 0; i--) {
                // X button for individual notification
                int bx = panel_x + CENTER_WIDTH - CENTER_PADDING - 24;
                int by = cy + 6;
                if (x >= bx && x < bx + 16 && y >= by && y < by + 16) {
                    // Remove this notification by shifting
                    for (int j = i; j < notif_count - 1; j++) {
                        notif_items[j] = notif_items[j + 1];
                    }
                    notif_count--;
                    return 1;
                }

                // Click on notification body — could launch associated app
                if (x >= panel_x + CENTER_PADDING && x < panel_x + CENTER_WIDTH - CENTER_PADDING &&
                    y >= cy && y < cy + CENTER_ITEM_H) {
                    // Mark as read
                    notif_items[i].read = 1;
                    return 1;
                }

                cy += CENTER_ITEM_H + 8;
            }

            return 1;  // Consumed click inside panel
        }

        // Click outside the panel — close it
        if (center_slide >= CENTER_SLIDE_TICKS) {
            center_open = 0;
            return 1;
        }
    }

    return 0;  // Not consumed
}
