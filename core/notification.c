// core/notification.c - CamelOS Notification System
// Toast-style notifications with priority-based display, categories,
// action buttons, slide-in/out animations, and Do Not Disturb mode.

#include "notification.h"
#include "memory.h"
#include "../include/string.h"
#include "../hal/video/gfx_hal.h"
#include "../common/serial.h"

/* ── Timer ticks reference ── */
extern volatile uint32_t ticks;

/* ── Internal animation states (stored in notification.dismissed) ── */
#define DISMISS_ACTIVE     0   /* Normal active display */
#define DISMISS_DONE       1   /* Moved to history */
#define DISMISS_SLIDE_OUT  2   /* Currently animating out */

/* ── Notification Center Singleton ── */
static notify_center_t center;

/* ══════════════════════════════════════════════════════════════════
 * Internal Helpers
 * ══════════════════════════════════════════════════════════════════ */

/* Category → accent color mapping (ARGB) */
static uint32_t category_color(notify_category_t cat) {
    switch (cat) {
        case NOTIFY_CAT_SYSTEM:   return 0xFF4A90D9; /* Blue        */
        case NOTIFY_CAT_NETWORK:  return 0xFF50C878; /* Green       */
        case NOTIFY_CAT_APP:      return 0xFF9B59B6; /* Purple      */
        case NOTIFY_CAT_MESSAGE:  return 0xFF3498DB; /* Light blue  */
        case NOTIFY_CAT_DOWNLOAD: return 0xFFF39C12; /* Orange      */
        case NOTIFY_CAT_SECURITY: return 0xFFE74C3C; /* Red         */
        case NOTIFY_CAT_MEDIA:    return 0xFFE91E63; /* Pink        */
        default:                  return 0xFF4A90D9;
    }
}

/* Compute the top-left screen position for the i-th visible notification */
static void notify_calc_pos(int index, int screen_w, int* out_x, int* out_y) {
    *out_x = screen_w - NOTIFY_WIDTH - NOTIFY_MARGIN_X;
    *out_y = NOTIFY_MARGIN_Y + index * (NOTIFY_HEIGHT + NOTIFY_SPACING);
}

/* Compute the current animation X-offset (pixels) for a notification.
 * Positive offset means the notification is shifted right (partially off-screen). */
static int compute_anim_offset(notification_t* n) {
    uint32_t elapsed;

    if (n->dismissed == DISMISS_SLIDE_OUT) {
        /* expire_time was repurposed as the slide-out animation start tick */
        if (ticks >= n->expire_time) {
            elapsed = ticks - n->expire_time;
        } else {
            elapsed = 0; /* ticks wrapped – treat as start */
        }
        if (elapsed >= NOTIFY_ANIM_OUT_TICKS) return NOTIFY_WIDTH;
        return (int)((uint32_t)NOTIFY_WIDTH * elapsed / NOTIFY_ANIM_OUT_TICKS);
    }

    /* Slide-in: use creation timestamp */
    if (ticks >= n->timestamp) {
        elapsed = ticks - n->timestamp;
    } else {
        elapsed = NOTIFY_ANIM_IN_TICKS; /* ticks wrapped – assume done */
    }
    if (elapsed >= NOTIFY_ANIM_IN_TICKS) return 0;
    return (int)((uint32_t)NOTIFY_WIDTH * (NOTIFY_ANIM_IN_TICKS - elapsed) / NOTIFY_ANIM_IN_TICKS);
}

/* Remove a notification from the active linked list by ID.
 * Returns the removed node (still allocated) or NULL. */
static notification_t* remove_from_active(int id) {
    notification_t** pp = &center.active;
    while (*pp) {
        if ((*pp)->id == id) {
            notification_t* n = *pp;
            *pp = n->next;
            n->next = NULL;
            center.active_count--;
            return n;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Add a notification to the front of the history list.
 * Trims oldest entries that exceed max_history. */
static void add_to_history(notification_t* n) {
    n->dismissed = DISMISS_DONE;
    n->next = center.history;
    center.history = n;
    center.history_count++;

    /* Trim the tail of the history list */
    while (center.history_count > (int)center.max_history) {
        notification_t** pp = &center.history;
        /* Walk to the second-to-last node */
        while ((*pp)->next) {
            pp = &(*pp)->next;
        }
        kfree(*pp);
        *pp = NULL;
        center.history_count--;
    }
}

/* Find the notification that should be evicted when we exceed the
 * visible limit: lowest priority first, oldest among those. */
static notification_t* find_eviction_candidate(void) {
    notification_t* best = NULL;
    notification_t* n = center.active;

    while (n) {
        if (n->dismissed == DISMISS_SLIDE_OUT) {
            /* Don't evict notifications already sliding out */
            n = n->next;
            continue;
        }
        if (!best ||
            n->priority < best->priority ||
            (n->priority == best->priority && n->id < best->id)) {
            best = n;
        }
        n = n->next;
    }
    return best;
}

/* Debug helper: write a small integer to serial */
static void debug_int(const char* prefix, int value, const char* suffix) {
    char buf[16];
    s_printf(prefix);
    int_to_str(value, buf);
    s_printf(buf);
    s_printf(suffix);
}

/* ══════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════ */

void notify_init(void) {
    memset(&center, 0, sizeof(center));
    center.dnd_mode    = 0;
    center.max_history = 50;
    center.next_id     = 1;
    center.active      = NULL;
    center.history     = NULL;
    center.active_count  = 0;
    center.history_count = 0;
    s_printf("[notify] Notification system initialized\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Core API – Post
 * ══════════════════════════════════════════════════════════════════ */

int notify_post(const char* title, const char* body, const char* source,
                notify_priority_t priority, notify_category_t category) {
    return notify_post_with_action(title, body, source, priority, category,
                                   NULL, NULL, NULL);
}

int notify_post_with_action(const char* title, const char* body, const char* source,
                            notify_priority_t priority, notify_category_t category,
                            const char* action_label, void (*callback)(void*), void* data) {
    if (!title) return -1;

    /* ── Allocate ── */
    notification_t* n = (notification_t*)kzalloc(sizeof(notification_t));
    if (!n) {
        s_printf("[notify] ERROR: kmalloc failed\n");
        return -1;
    }

    /* ── Fill fields ── */
    n->id = center.next_id++;
    strncpy(n->title, title, sizeof(n->title) - 1);
    n->title[sizeof(n->title) - 1] = '\0';

    if (body) {
        strncpy(n->body, body, sizeof(n->body) - 1);
        n->body[sizeof(n->body) - 1] = '\0';
    }

    if (source) {
        strncpy(n->source, source, sizeof(n->source) - 1);
        n->source[sizeof(n->source) - 1] = '\0';
    }

    n->priority    = priority;
    n->category    = category;
    n->timestamp   = ticks;
    n->dismissed   = DISMISS_ACTIVE;
    n->action_count = 0;

    /* Expiry time based on priority */
    switch (priority) {
        case NOTIFY_PRIORITY_LOW:
            n->expire_time = ticks + 150;                      /* ~3 s  */
            break;
        case NOTIFY_PRIORITY_NORMAL:
            n->expire_time = ticks + NOTIFY_DISPLAY_TIME_NORMAL; /* ~5 s  */
            break;
        case NOTIFY_PRIORITY_HIGH:
            n->expire_time = ticks + NOTIFY_DISPLAY_TIME_HIGH;   /* ~10 s */
            break;
        case NOTIFY_PRIORITY_URGENT:
            n->expire_time = 0;                                  /* never */
            break;
        default:
            n->expire_time = ticks + NOTIFY_DISPLAY_TIME_NORMAL;
            break;
    }

    /* Optional action button */
    if (action_label && callback) {
        strncpy(n->actions[0].label, action_label, sizeof(n->actions[0].label) - 1);
        n->actions[0].label[sizeof(n->actions[0].label) - 1] = '\0';
        n->actions[0].callback = callback;
        n->actions[0].data     = data;
        n->action_count = 1;
    }

    /* ── Do Not Disturb ── */
    if (center.dnd_mode) {
        if (priority == NOTIFY_PRIORITY_URGENT) {
            /* URGENT still gets recorded in history */
            add_to_history(n);
            s_printf("[notify] URGENT in DND -> history\n");
            return n->id;
        } else {
            kfree(n);
            s_printf("[notify] Suppressed (DND)\n");
            return -1;
        }
    }

    /* ── Insert into active list sorted by priority (highest first).
     *    Among equal priority, newer notifications go first. ── */
    {
        notification_t** pp = &center.active;
        while (*pp) {
            if (n->priority > (*pp)->priority) break;
            if (n->priority == (*pp)->priority) break; /* newer before older */
            pp = &(*pp)->next;
        }
        n->next = *pp;
        *pp = n;
        center.active_count++;
    }

    /* ── Enforce visible cap ── */
    while (center.active_count > NOTIFY_MAX_VISIBLE) {
        notification_t* victim = find_eviction_candidate();
        if (!victim) break;
        notification_t* removed = remove_from_active(victim->id);
        if (removed) add_to_history(removed);
    }

    debug_int("[notify] Posted #", n->id, "\n");
    return n->id;
}

/* ══════════════════════════════════════════════════════════════════
 * Core API – Dismiss
 * ══════════════════════════════════════════════════════════════════ */

int notify_dismiss(int id) {
    notification_t* n = remove_from_active(id);
    if (!n) return -1;

    /* Unblock any animation – move straight to history */
    add_to_history(n);

    debug_int("[notify] Dismissed #", id, "\n");
    return 0;
}

int notify_dismiss_all(void) {
    int count = 0;
    while (center.active) {
        notification_t* n = center.active;
        center.active = n->next;
        n->next = NULL;
        center.active_count--;
        add_to_history(n);
        count++;
    }

    debug_int("[notify] Dismissed all (", count, ")\n");
    return count;
}

/* ══════════════════════════════════════════════════════════════════
 * Rendering – called from the compositor paint loop
 * ══════════════════════════════════════════════════════════════════ */

void notify_render(void) {
    if (!center.active) return;

    int screen_w = gfx_get_width();
    int screen_h = gfx_get_height();
    (void)screen_h; /* available for bounds checking */

    int idx = 0;
    notification_t* n = center.active;

    while (n && idx < NOTIFY_MAX_VISIBLE) {
        /* ── Position ── */
        int nx, ny;
        notify_calc_pos(idx, screen_w, &nx, &ny);

        int anim_off = compute_anim_offset(n);
        nx += anim_off;

        /* If slide-out is fully off-screen, still advance the index
         * so lower notifications stay in their lane until the
         * sliding-out one is removed by notify_animate(). */
        int skip_draw = (anim_off >= NOTIFY_WIDTH);

        if (!skip_draw) {
            uint32_t cat_col = category_color(n->category);

            /* 1 ── Subtle shadow (single layer, offset down-right) */
            gfx_fill_rounded_rect_aa(nx + 2, ny + 3,
                                      NOTIFY_WIDTH, NOTIFY_HEIGHT,
                                      0x20000000, 10);

            /* 2 ── Background (semi-transparent dark) */
            gfx_fill_rounded_rect_aa(nx, ny,
                                      NOTIFY_WIDTH, NOTIFY_HEIGHT,
                                      0xE0303030, 10);

            /* 3 ── Category accent bar (left edge) */
            gfx_fill_rounded_rect(nx + 3, ny + 6, 4, NOTIFY_HEIGHT - 12,
                                   cat_col, 2);

            /* 4 ── Title text (white, up to ~33 chars) */
            {
                int max_ch = (NOTIFY_WIDTH - 42) / 8;
                if (max_ch > 38) max_ch = 38;
                char tbuf[40];
                strncpy(tbuf, n->title, max_ch);
                tbuf[max_ch] = '\0';
                gfx_draw_string(nx + 14, ny + 8, tbuf, 0xFFFFFFFF);
            }

            /* 5 ── Close button (X) in top-right corner */
            {
                int cx = nx + NOTIFY_WIDTH - 22;
                int cy = ny + 6;
                gfx_fill_rounded_rect(cx, cy, 16, 16, 0x40FFFFFF, 8);
                /* X strokes */
                gfx_draw_line(cx + 4, cy + 4, cx + 11, cy + 11, 0xFFAAAAAA);
                gfx_draw_line(cx + 11, cy + 4, cx + 4, cy + 11, 0xFFAAAAAA);
            }

            /* 6 ── Body text (light gray, up to ~34 chars) */
            {
                int max_ch = (NOTIFY_WIDTH - 28) / 8;
                if (max_ch > 38) max_ch = 38;
                char bbuf[40];
                strncpy(bbuf, n->body, max_ch);
                bbuf[max_ch] = '\0';
                gfx_draw_string(nx + 14, ny + 24, bbuf, 0xFFCCCCCC);
            }

            /* 7 ── Source label (category accent color) */
            gfx_draw_string(nx + 14, ny + 40, n->source, cat_col);

            /* 8 ── Priority indicator (colored dot for HIGH / URGENT) */
            if (n->priority >= NOTIFY_PRIORITY_HIGH) {
                uint32_t dot_col = (n->priority == NOTIFY_PRIORITY_URGENT)
                                    ? 0xFFFF4444 : 0xFFFFAA00;
                gfx_fill_rounded_rect(nx + NOTIFY_WIDTH - 40, ny + 9,
                                       8, 8, dot_col, 4);
            }

            /* 9 ── Action buttons */
            {
                int ac = n->action_count;
                if (ac > 3) ac = 3;
                for (int a = ac - 1; a >= 0; a--) {
                    int bw = (int)(strlen(n->actions[a].label) * 8) + 16;
                    if (bw < 40) bw = 40;
                    int bx = nx + NOTIFY_WIDTH - bw - 8;
                    int by = ny + NOTIFY_HEIGHT - 24;

                    /* Button pill */
                    gfx_fill_rounded_rect(bx, by, bw, 18, cat_col, 4);
                    /* Label */
                    gfx_draw_string(bx + 8, by + 4,
                                     n->actions[a].label, 0xFFFFFFFF);
                }
            }

            /* 10 ── Thin top highlight (glass-like) */
            gfx_draw_line(nx + 10, ny + 1,
                          nx + NOTIFY_WIDTH - 10, ny + 1,
                          0x18FFFFFF);
        }

        n = n->next;
        idx++;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * Animation – called from the timer tick handler
 * ══════════════════════════════════════════════════════════════════ */

void notify_animate(void) {
    if (!center.active) return;

    notification_t** pp = &center.active;

    while (*pp) {
        notification_t* n = *pp;

        /* ── Complete any finished slide-out animations ── */
        if (n->dismissed == DISMISS_SLIDE_OUT) {
            uint32_t elapsed = (ticks >= n->expire_time)
                               ? ticks - n->expire_time : NOTIFY_ANIM_OUT_TICKS;
            if (elapsed >= NOTIFY_ANIM_OUT_TICKS) {
                *pp = n->next;
                n->next = NULL;
                center.active_count--;
                add_to_history(n);
                /* Don't advance pp – it already points to the next node */
                continue;
            }
            pp = &n->next;
            continue;
        }

        /* ── Check auto-expire ── */
        if (n->expire_time != 0 && ticks >= n->expire_time) {
            /* Begin slide-out animation */
            n->dismissed   = DISMISS_SLIDE_OUT;
            n->expire_time = ticks; /* repurpose as anim start tick */
            s_printf("[notify] Auto-expiring\n");
        }

        pp = &n->next;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * Click Handling
 * ══════════════════════════════════════════════════════════════════ */

int notify_click(int x, int y) {
    int screen_w = gfx_get_width();
    int idx = 0;
    notification_t* n = center.active;

    while (n && idx < NOTIFY_MAX_VISIBLE) {
        /* Skip notifications that are fully off-screen (sliding out) */
        if (n->dismissed == DISMISS_SLIDE_OUT &&
            compute_anim_offset(n) >= NOTIFY_WIDTH) {
            n = n->next;
            idx++;
            continue;
        }

        int nx, ny;
        notify_calc_pos(idx, screen_w, &nx, &ny);
        nx += compute_anim_offset(n);

        /* ── Bounds check ── */
        if (x >= nx && x < nx + NOTIFY_WIDTH &&
            y >= ny && y < ny + NOTIFY_HEIGHT) {

            /* Close button hit test */
            {
                int cx = nx + NOTIFY_WIDTH - 22;
                int cy = ny + 6;
                if (x >= cx && x < cx + 16 && y >= cy && y < cy + 16) {
                    notify_dismiss(n->id);
                    return n->id;
                }
            }

            /* Action button hit test */
            {
                int ac = n->action_count;
                if (ac > 3) ac = 3;
                for (int a = ac - 1; a >= 0; a--) {
                    int bw = (int)(strlen(n->actions[a].label) * 8) + 16;
                    if (bw < 40) bw = 40;
                    int bx = nx + NOTIFY_WIDTH - bw - 8;
                    int by = ny + NOTIFY_HEIGHT - 24;

                    if (x >= bx && x < bx + bw && y >= by && y < by + 18) {
                        notify_action_invoke(n->id, a);
                        return n->id;
                    }
                }
            }

            /* Clicked somewhere on the notification body */
            return n->id;
        }

        n = n->next;
        idx++;
    }

    return 0; /* No notification hit */
}

int notify_action_invoke(int notify_id, int action_idx) {
    /* Find the notification in the active list */
    notification_t* n = center.active;
    while (n) {
        if (n->id == notify_id) break;
        n = n->next;
    }
    if (!n) return -1;
    if (action_idx < 0 || action_idx >= n->action_count) return -1;
    if (!n->actions[action_idx].callback) return -1;

    /* Invoke the callback */
    n->actions[action_idx].callback(n->actions[action_idx].data);

    debug_int("[notify] Action #", notify_id, " invoked\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * History / Query
 * ══════════════════════════════════════════════════════════════════ */

notification_t* notify_get_active(int* count) {
    if (count) *count = center.active_count;
    return center.active;
}

notification_t* notify_get_history(int* count) {
    if (count) *count = center.history_count;
    return center.history;
}

int notify_get_by_id(int id, notification_t* out) {
    if (!out) return -1;

    /* Search active */
    notification_t* n = center.active;
    while (n) {
        if (n->id == id) {
            memcpy(out, n, sizeof(notification_t));
            out->next = NULL;
            return 0;
        }
        n = n->next;
    }

    /* Search history */
    n = center.history;
    while (n) {
        if (n->id == id) {
            memcpy(out, n, sizeof(notification_t));
            out->next = NULL;
            return 0;
        }
        n = n->next;
    }

    return -1; /* Not found */
}

/* ══════════════════════════════════════════════════════════════════
 * Do Not Disturb
 * ══════════════════════════════════════════════════════════════════ */

void notify_set_dnd(int enabled) {
    center.dnd_mode = enabled ? 1 : 0;
    s_printf(enabled ? "[notify] DND on\n" : "[notify] DND off\n");
}

int notify_get_dnd(void) {
    return center.dnd_mode;
}

/* ══════════════════════════════════════════════════════════════════
 * Configuration
 * ══════════════════════════════════════════════════════════════════ */

void notify_set_max_history(uint32_t max) {
    center.max_history = max;

    /* Trim existing history from the tail */
    while (center.history_count > (int)center.max_history) {
        notification_t** pp = &center.history;
        while ((*pp)->next) pp = &(*pp)->next;
        kfree(*pp);
        *pp = NULL;
        center.history_count--;
    }
}

void notify_clear_history(void) {
    while (center.history) {
        notification_t* n = center.history;
        center.history = n->next;
        kfree(n);
    }
    center.history_count = 0;
    s_printf("[notify] History cleared\n");
}
