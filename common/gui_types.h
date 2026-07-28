#ifndef GUI_TYPES_H
#define GUI_TYPES_H

#ifndef MAX_MENU_ITEMS
#define MAX_MENU_ITEMS 8
#endif
#ifndef MAX_MENUS
#define MAX_MENUS 6
#endif
#define MAX_SUBMENU_ITEMS 5

// MenuItem: a single item in a menu dropdown.
// For items with submenus (e.g., "View -> Sort By -> [Name, Size, Date]"),
// set has_submenu=1 and populate submenu_items/submenu_count.
typedef struct {
    char label[24];              // Display label
    char action_id[32];         // Action identifier or shortcut
    int is_separator;           // 1 = separator line, not clickable
    int has_submenu;            // 1 = this item has a submenu
    char submenu_labels[MAX_SUBMENU_ITEMS][24]; // Submenu item labels
    int submenu_action_ids[MAX_SUBMENU_ITEMS];  // Submenu action IDs
    int submenu_count;          // Number of submenu items
} MenuItem;

typedef struct {
    char name[16];
    MenuItem items[MAX_MENU_ITEMS];
    int item_count;
} MenuCategory;

#endif
