#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include "../include/types.h"

/* Notification priority levels */
typedef enum {
    NOTIFY_PRIORITY_LOW    = 0,   /* Informational, subtle display */
    NOTIFY_PRIORITY_NORMAL = 1,   /* Default */
    NOTIFY_PRIORITY_HIGH   = 2,   /* Important, stays longer */
    NOTIFY_PRIORITY_URGENT = 3,   /* Critical, requires attention */
} notify_priority_t;

/* Notification category */
typedef enum {
    NOTIFY_CAT_SYSTEM     = 0,    /* System notifications */
    NOTIFY_CAT_NETWORK    = 1,    /* Network status changes */
    NOTIFY_CAT_APP        = 2,    /* Application notifications */
    NOTIFY_CAT_MESSAGE    = 3,    /* Messages/chat */
    NOTIFY_CAT_DOWNLOAD   = 4,    /* Download progress */
    NOTIFY_CAT_SECURITY   = 5,    /* Security alerts */
    NOTIFY_CAT_MEDIA      = 6,    /* Media playback */
} notify_category_t;

/* Notification action */
typedef struct notify_action {
    char label[32];                /* Action button label */
    void (*callback)(void* data);  /* Action callback */
    void* data;                    /* Callback data */
} notify_action_t;

/* Notification structure */
typedef struct notification {
    int id;                          /* Unique notification ID */
    char title[64];                  /* Notification title */
    char body[256];                  /* Notification body text */
    char source[32];                 /* Source app name */
    notify_priority_t priority;      /* Priority level */
    notify_category_t category;      /* Category */
    uint32_t timestamp;              /* Creation time (ticks) */
    uint32_t expire_time;            /* When to auto-dismiss (ticks) */
    int dismissed;                   /* 1 if dismissed */
    int action_count;                /* Number of actions */
    notify_action_t actions[3];      /* Up to 3 action buttons */
    struct notification* next;       /* Linked list next */
} notification_t;

/* Notification center state */
typedef struct {
    notification_t* active;         /* Currently displayed notifications */
    notification_t* history;        /* Dismissed/history list */
    int active_count;               /* Number of active notifications */
    int history_count;              /* Number in history */
    int dnd_mode;                   /* Do Not Disturb mode (0=off, 1=on) */
    uint32_t max_history;           /* Max history entries (default 50) */
    int next_id;                    /* Next notification ID */
} notify_center_t;

/* Display timing (in ticks, ~20ms per tick at 50Hz) */
#define NOTIFY_DISPLAY_TIME_NORMAL   250    /* ~5 seconds */
#define NOTIFY_DISPLAY_TIME_HIGH     500    /* ~10 seconds */
#define NOTIFY_DISPLAY_TIME_URGENT   0      /* Until dismissed */
#define NOTIFY_ANIM_IN_TICKS        10      /* Slide-in animation */
#define NOTIFY_ANIM_OUT_TICKS       8       /* Slide-out animation */

/* Notification display position */
#define NOTIFY_MARGIN_X             16
#define NOTIFY_MARGIN_Y             40
#define NOTIFY_WIDTH                300
#define NOTIFY_HEIGHT               80
#define NOTIFY_SPACING              8
#define NOTIFY_MAX_VISIBLE          5

/* === Initialization === */
void notify_init(void);

/* === Core API === */
int notify_post(const char* title, const char* body, const char* source,
                notify_priority_t priority, notify_category_t category);
int notify_post_with_action(const char* title, const char* body, const char* source,
                            notify_priority_t priority, notify_category_t category,
                            const char* action_label, void (*callback)(void*), void* data);
int notify_dismiss(int id);
int notify_dismiss_all(void);

/* === Rendering === */
void notify_render(void);               /* Called from compositor to draw active toasts */
void notify_animate(void);              /* Called from timer tick for animations */

/* === History/Query === */
notification_t* notify_get_active(int* count);
notification_t* notify_get_history(int* count);
int notify_get_by_id(int id, notification_t* out);

/* === Do Not Disturb === */
void notify_set_dnd(int enabled);
int notify_get_dnd(void);

/* === Action Handling === */
int notify_click(int x, int y);         /* Handle click at screen coordinates */
int notify_action_invoke(int notify_id, int action_idx);

/* === Configuration === */
void notify_set_max_history(uint32_t max);
void notify_clear_history(void);

#endif /* NOTIFICATION_H */
