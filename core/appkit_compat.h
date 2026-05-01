// core/appkit_compat.h - AppKit Compatibility Layer for CamelOS
// Maps NSApplication, NSWindow, NSView, NSMenu, NSTextField, NSButton,
// NSImageView, NSScrollView, NSTableView to CamelOS window_server/compositor
// This is the bridge between macOS AppKit API calls and CamelOS's native GUI
#ifndef APPKIT_COMPAT_H
#define APPKIT_COMPAT_H

#include "objc_runtime.h"
#include "foundation_stub.h"
#include "foundation_extra.h"
#include "../include/types.h"
#include "../common/gui_types.h"

// ============================================================================
// NSApplication
// ============================================================================
typedef struct {
    struct objc_object isa;
    int running;
    int finished;
    id delegate;
    char app_name[64];
    void* menu_bar;
} CamelOSApplication;
extern Class NSApplication_class;

// ============================================================================
// NSWindow (maps to window_t in window_server)
// ============================================================================
typedef struct {
    struct objc_object isa;
    int ws_window_id;        // ID in window_server
    void* ws_window;         // Pointer to window_t
    char title[64];
    int x, y, w, h;
    int style_mask;
    id content_view;         // NSView*
    id delegate;
    int visible;
    int key_window;          // Is this the key window?
    float opacity;
} CamelOSWindow;
extern Class NSWindow_class;

// Window style masks
#define NSWindowStyleMaskTitled      0x01
#define NSWindowStyleMaskClosable    0x02
#define NSWindowStyleMaskMiniaturizable 0x04
#define NSWindowStyleMaskResizable   0x08
#define NSWindowStyleMaskFullScreen  0x10
#define NSWindowStyleMaskBorderless  0x20

// ============================================================================
// NSView (maps to a rectangular paint region)
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;          // Frame rect relative to superview
    int abs_x, abs_y;        // Absolute screen coordinates (computed)
    id superview;
    CamelOSArray* subviews;
    int needs_display;
    uint32_t bg_color;
    void* paint_func;        // Custom draw callback
    void* mouse_func;        // Custom mouse callback
    void* key_func;          // Custom key callback
    int is_opaque;
    int is_hidden;
} CamelOSView;
extern Class NSView_class;

// ============================================================================
// NSMenu
// ============================================================================
typedef struct {
    struct objc_object isa;
    char title[32];
    CamelOSArray* items;
} CamelOSMenu;
extern Class NSMenu_class;

typedef struct {
    struct objc_object isa;
    char title[32];
    char key_equivalent[8];
    id target;
    SEL action;
    int is_separator;
    int is_enabled;
} CamelOSMenuItem;
extern Class NSMenuItem_class;

// ============================================================================
// NSControl (base for interactive controls)
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    id target;
    SEL action;
    int enabled;
    int state;  // For checkboxes: 0=off, 1=on
    uint32_t bg_color;
    uint32_t text_color;
} CamelOSControl;
extern Class NSControl_class;

// ============================================================================
// NSTextField
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char text[256];
    int is_editable;
    int is_selectable;
    int alignment;  // 0=left, 1=center, 2=right
    uint32_t text_color;
    uint32_t bg_color;
    id delegate;
    int font_size;
} CamelOSTextField;
extern Class NSTextField_class;

// ============================================================================
// NSButton
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char title[64];
    id target;
    SEL action;
    int button_type;  // 0=push, 1=checkbox, 2=radio
    int state;
    uint32_t bg_color;
    uint32_t text_color;
    int is_bordered;
    int is_highlighted;
} CamelOSButton;
extern Class NSButton_class;

// ============================================================================
// NSImageView
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char image_path[256];
    uint32_t* image_data;
    int image_w, image_h;
    int scaling;  // 0=none, 1=proportional, 2=fit
} CamelOSImageView;
extern Class NSImageView_class;

// ============================================================================
// NSScrollView
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    id content_view;       // The clip view
    id document_view;      // The scrollable content
    int has_vertical_scroller;
    int has_horizontal_scroller;
    int scroll_x, scroll_y;
    int content_size_w, content_size_h;
} CamelOSScrollView;
extern Class NSScrollView_class;

// ============================================================================
// NSTextView (rich text, maps to textedit)
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char* text;
    uint32_t text_length;
    uint32_t text_capacity;
    int is_editable;
    int is_rich_text;
    uint32_t text_color;
    uint32_t bg_color;
    int font_size;
    int cursor_pos;
    int scroll_offset;
} CamelOSTextView;
extern Class NSTextView_class;

// ============================================================================
// NSColor (simplified - just stores RGBA)
// ============================================================================
typedef struct {
    struct objc_object isa;
    float r, g, b, a;
    uint32_t rgba;  // Packed color
} CamelOSColor;
extern Class NSColor_class;

// Common colors
#define NSColor_whiteColor     0xFFFFFFFF
#define NSColor_blackColor     0xFF000000
#define NSColor_grayColor      0xFF808080
#define NSColor_redColor       0xFFFF3B30
#define NSColor_greenColor     0xFF34C759
#define NSColor_blueColor      0xFF007AFF
#define NSColor_yellowColor    0xFFFFCC00
#define NSColor_orangeColor    0xFFFF9500
#define NSColor_windowBgColor  0xFFF6F6F6
#define NSColor_controlColor   0xFFFFFFFF
#define NSColor_textColor      0xFF333333
#define NSColor_secondaryTextColor 0xFF888888
#define NSColor_separatorColor 0xFFD4D4D4

// ============================================================================
// NSFont (simplified)
// ============================================================================
typedef struct {
    struct objc_object isa;
    char font_name[64];
    int size;
    int is_bold;
    int is_italic;
} CamelOSFont;
extern Class NSFont_class;

// ============================================================================
// NSTableView - Table view with rows and columns
// ============================================================================
#define NSTableViewSelectionHighlightStyleRegular  0
#define NSTableViewSelectionHighlightStyleSourceList 1

typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    id data_source;         // DataSource object
    id delegate;            // Delegate object
    int row_height;
    int number_of_rows;
    int selected_row;
    int header_height;
    int uses_alternating_row_bg;
    int selection_style;
    int scroll_y;
    // Column definitions (simplified: max 8 columns)
    int column_count;
    char column_titles[8][32];
    int column_widths[8];
} CamelOSTableView;
extern Class NSTableView_class;

// ============================================================================
// NSOutlineView - Outline/tree view (extends NSTableView)
// ============================================================================
typedef struct {
    CamelOSTableView table;  // Inherits from NSTableView
    int auto_expand_items;
    int indent_per_level;
    int is_expanded[256];    // Expansion state per item
} CamelOSOutlineView;
extern Class NSOutlineView_class;

// NSRunLoop is defined in foundation_extra.h (with timer support + AppKit ports)
// CamelOSRunLoop and NSRunLoop_class are available from that header

// ============================================================================
// NSWorkspace - Application and file management
// ============================================================================
typedef struct {
    struct objc_object isa;
    int notify_delegate;
} CamelOSWorkspace;
extern Class NSWorkspace_class;

// ============================================================================
// NSSearchField - Search text field with magnifying glass icon
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char text[256];
    char placeholder[64];
    int is_editable;
    uint32_t text_color;
    uint32_t bg_color;
    id delegate;
    id search_button;
    id cancel_button;
    int recents_autosave_name[64];
} CamelOSSearchField;
extern Class NSSearchField_class;

// ============================================================================
// NSProgressIndicator - Spinning/loading indicator
// ============================================================================
#define NSProgressIndicatorBarStyle    0
#define NSProgressIndicatorSpinningStyle 1

typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    int style;              // Bar or spinning
    double min_value;
    double max_value;
    double double_value;    // Current value
    int is_indeterminate;
    int is_animating;
    uint32_t progress_color;
    uint32_t track_color;
} CamelOSProgressIndicator;
extern Class NSProgressIndicator_class;

// ============================================================================
// NSToolbar - Window toolbar (below title bar)
// ============================================================================
#define NSToolbarItemTypeGeneral  0
#define NSToolbarItemTypeFlexibleSpace 1

typedef struct {
    struct objc_object isa;
    char identifier[64];
    id delegate;
    int allows_user_customization;
    int display_mode;       // Default, icon, label, iconAndLabel
    int visible;
    // Items
    int item_count;
    char item_labels[16][32];
    int item_types[16];
} CamelOSToolbar;
extern Class NSToolbar_class;

// ============================================================================
// NSSlider - Horizontal/vertical slider control
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    double min_value;
    double max_value;
    double current_value;
    int is_vertical;
    id target;
    SEL action;
    int tick_mark_count;
    uint32_t track_color;
    uint32_t knob_color;
} CamelOSSlider;
extern Class NSSlider_class;

// ============================================================================
// NSPopUpButton - Dropdown/popup menu button
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char title[64];
    char items[16][64];    // Menu items
    int item_count;
    int selected_index;
    id target;
    SEL action;
    int pulls_down;        // 0=popup, 1=pulldown
    uint32_t bg_color;
    uint32_t text_color;
} CamelOSPopUpButton;
extern Class NSPopUpButton_class;

// ============================================================================
// NSCheckBox - Checkbox control
// ============================================================================
typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    char title[64];
    int state;              // 0=off, 1=on, 2=mixed
    id target;
    SEL action;
    uint32_t text_color;
} CamelOSCheckBox;
extern Class NSCheckBox_class;

// ============================================================================
// AppKit Initialization
// ============================================================================

// Initialize the AppKit compatibility layer (call after Foundation init)
void appkit_init(void);

// Get the shared NSApplication
id NSApplication_sharedApplication(void);

// Run the application event loop
void NSApplication_run(id self, SEL cmd);

#endif // APPKIT_COMPAT_H
