#ifndef NOTIFICATION_CENTER_H
#define NOTIFICATION_CENTER_H

#include "../include/types.h"

#define NOTIF_MAX_ITEMS 32
#define NOTIF_MAX_TITLE 64
#define NOTIF_MAX_BODY 128
#define NOTIF_MAX_APP  32

// Notification types
#define NOTIF_TYPE_INFO     0
#define NOTIF_TYPE_SUCCESS  1
#define NOTIF_TYPE_WARNING  2
#define NOTIF_TYPE_ERROR    3

typedef struct {
    char title[NOTIF_MAX_TITLE];
    char body[NOTIF_MAX_BODY];
    char app_name[NOTIF_MAX_APP];
    int type;
    uint32_t timestamp;
    int read;
    int active;       // 1 = currently showing as banner
    int banner_timer; // Frames remaining for banner display
} notification_item_t;

// Initialize notification system
void notif_init(void);

// Post a notification (shows banner + adds to center)
void notif_post(const char* app_name, const char* title, const char* body, int type);

// Dismiss active banner
void notif_dismiss(void);

// Open/close notification center panel
void notif_center_toggle(void);
int notif_center_is_open(void);

// Clear all notifications
void notif_clear_all(void);

// Get unread count
int notif_unread_count(void);

// Render the notification banner (call from main loop)
void notif_render_banner(void);

// Render the notification center panel (call from main loop)
void notif_render_center(void);

// Handle input in notification center
int notif_handle_click(int x, int y);

// Tick for banner animation
void notif_tick(void);

#endif
