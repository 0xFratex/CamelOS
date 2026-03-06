#ifndef GUI_TYPES_H
#define GUI_TYPES_H

#define MAX_MENU_ITEMS 5
#define MAX_MENUS 4

typedef struct {
    char label[16];
    char action_id[32];
} MenuItem;

typedef struct {
    char name[12];
    MenuItem items[MAX_MENU_ITEMS];
    int item_count;
} MenuCategory;

#endif
