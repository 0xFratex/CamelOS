// core/appkit_extra.c - Additional AppKit Controls Implementation
// Implements: NSTableView, NSOutlineView, NSRunLoop, NSWorkspace,
// NSSearchField, NSProgressIndicator, NSToolbar, NSSlider,
// NSPopUpButton, NSCheckBox
//
// These controls bridge macOS AppKit API to CamelOS window_server/gfx_hal

#include "appkit_compat.h"
#include "foundation_stub.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/serial.h"
#include "../core/window_server.h"

extern kernel_api_t g_kernel_api;

// ============================================================================
// Class References
// ============================================================================
Class NSTableView_class = 0;
Class NSOutlineView_class = 0;
Class NSRunLoop_class = 0;
Class NSWorkspace_class = 0;
Class NSSearchField_class = 0;
Class NSProgressIndicator_class = 0;
Class NSToolbar_class = 0;
Class NSSlider_class = 0;
Class NSPopUpButton_class = 0;
Class NSCheckBox_class = 0;

// ============================================================================
// NSTableView Implementation
// ============================================================================

id NSTableView_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (!tv) return 0;
    
    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
    tv->data_source = 0;
    tv->delegate = 0;
    tv->row_height = 20;
    tv->number_of_rows = 0;
    tv->selected_row = -1;
    tv->header_height = 22;
    tv->uses_alternating_row_bg = 1;
    tv->selection_style = NSTableViewSelectionHighlightStyleRegular;
    tv->scroll_y = 0;
    tv->column_count = 0;
    
    return self;
}

void NSTableView_setDataSource(id self, SEL cmd, id source) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) tv->data_source = source;
}

void NSTableView_setDelegate(id self, SEL cmd, id delegate) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) tv->delegate = delegate;
}

int NSTableView_numberOfRows(id self, SEL cmd) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    return tv ? tv->number_of_rows : 0;
}

void NSTableView_setNumberOfRows(id self, SEL cmd, int count) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) tv->number_of_rows = count;
}

void NSTableView_addColumn(id self, SEL cmd, const char* title, int width) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (!tv || tv->column_count >= 8) return;
    
    strncpy(tv->column_titles[tv->column_count], title, 31);
    tv->column_titles[tv->column_count][31] = 0;
    tv->column_widths[tv->column_count] = width;
    tv->column_count++;
}

int NSTableView_selectedRow(id self, SEL cmd) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    return tv ? tv->selected_row : -1;
}

void NSTableView_selectRow(id self, SEL cmd, int row) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) {
        tv->selected_row = row;
        // Notify delegate
        if (tv->delegate) {
            SEL sel = sel_registerName("tableViewSelectionDidChange:");
            Method m = class_getInstanceMethod(object_getClass(tv->delegate), sel);
            if (m && m->imp) {
                typedef void (*sel_fn)(id, SEL, id);
                ((sel_fn)m->imp)(tv->delegate, sel, self);
            }
        }
    }
}

void NSTableView_reloadData(id self, SEL cmd) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (!tv) return;
    
    // Ask data source for number of rows
    if (tv->data_source) {
        SEL sel = sel_registerName("numberOfRowsInTableView:");
        Method m = class_getInstanceMethod(object_getClass(tv->data_source), sel);
        if (m && m->imp) {
            typedef int (*row_fn)(id, SEL, id);
            tv->number_of_rows = ((row_fn)m->imp)(tv->data_source, sel, self);
        }
    }
}

void NSTableView_setRowHeight(id self, SEL cmd, int height) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) tv->row_height = height;
}

void NSTableView_setUsesAlternatingRowBackgroundColors(id self, SEL cmd, int use) {
    (void)cmd;
    CamelOSTableView* tv = (CamelOSTableView*)self;
    if (tv) tv->uses_alternating_row_bg = use;
}

static void register_NSTableView_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSTableView_initWithFrame, "@@:iiii");
    sel = sel_registerName("setDataSource:");
    class_addMethod(cls, sel, (void*)NSTableView_setDataSource, "v@:@");
    sel = sel_registerName("setDelegate:");
    class_addMethod(cls, sel, (void*)NSTableView_setDelegate, "v@:@");
    sel = sel_registerName("numberOfRows");
    class_addMethod(cls, sel, (void*)NSTableView_numberOfRows, "i@:");
    sel = sel_registerName("setNumberOfRows:");
    class_addMethod(cls, sel, (void*)NSTableView_setNumberOfRows, "v@:i");
    sel = sel_registerName("addColumn:width:");
    class_addMethod(cls, sel, (void*)NSTableView_addColumn, "v@:*i");
    sel = sel_registerName("selectedRow");
    class_addMethod(cls, sel, (void*)NSTableView_selectedRow, "i@:");
    sel = sel_registerName("selectRow:");
    class_addMethod(cls, sel, (void*)NSTableView_selectRow, "v@:i");
    sel = sel_registerName("reloadData");
    class_addMethod(cls, sel, (void*)NSTableView_reloadData, "v@:");
    sel = sel_registerName("setRowHeight:");
    class_addMethod(cls, sel, (void*)NSTableView_setRowHeight, "v@:i");
    sel = sel_registerName("setUsesAlternatingRowBackgroundColors:");
    class_addMethod(cls, sel, (void*)NSTableView_setUsesAlternatingRowBackgroundColors, "v@:i");
}

// ============================================================================
// NSRunLoop Implementation
// ============================================================================

id NSRunLoop_currentRunLoop(void) {
    static CamelOSRunLoop* loop = 0;
    if (!loop) {
        loop = (CamelOSRunLoop*)class_createInstance(NSRunLoop_class, 0);
        if (loop) {
            loop->running = 0;
            loop->accepting_input = 1;
            loop->timer_port = 0;
            loop->event_port = 0;
            track_object((id)loop);
        }
    }
    return (id)loop;
}

void NSRunLoop_run(id self, SEL cmd) {
    (void)cmd;
    CamelOSRunLoop* loop = (CamelOSRunLoop*)self;
    if (!loop) return;
    
    loop->running = 1;
    
    while (loop->running && loop->accepting_input) {
        // Process window server events
        g_kernel_api.process_events();
        
        // Process timers
        // (TODO: implement timer processing)
        
        // Small delay to prevent CPU spinning
        sys_delay(1);
    }
}

void NSRunLoop_stop(id self, SEL cmd) {
    (void)cmd;
    CamelOSRunLoop* loop = (CamelOSRunLoop*)self;
    if (loop) loop->running = 0;
}

static void register_NSRunLoop_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("run");
    class_addMethod(cls, sel, (void*)NSRunLoop_run, "v@:");
    sel = sel_registerName("stop");
    class_addMethod(cls, sel, (void*)NSRunLoop_stop, "v@:");
}

// ============================================================================
// NSWorkspace Implementation
// ============================================================================

id NSWorkspace_sharedWorkspace(void) {
    static CamelOSWorkspace* ws = 0;
    if (!ws) {
        ws = (CamelOSWorkspace*)class_createInstance(NSWorkspace_class, 0);
        if (ws) {
            ws->notify_delegate = 0;
            track_object((id)ws);
        }
    }
    return (id)ws;
}

int NSWorkspace_launchApp(id self, SEL cmd, const char* app_path) {
    (void)self; (void)cmd;
    // Launch an application using the CDL loader or app installer
    extern int cdl_load_and_run(const char* path);
    return cdl_load_and_run(app_path);
}

int NSWorkspace_openFile(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    // Open a file with the default application
    // For now, just try to launch the file as an app
    extern int sys_fs_exists(const char* path);
    if (sys_fs_exists(path)) {
        // TODO: Determine the appropriate app for the file type
        return 0;
    }
    return -1;
}

static void register_NSWorkspace_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("launchApp:");
    class_addMethod(cls, sel, (void*)NSWorkspace_launchApp, "i@:*");
    sel = sel_registerName("openFile:");
    class_addMethod(cls, sel, (void*)NSWorkspace_openFile, "i@:*");
}

// ============================================================================
// NSSearchField Implementation
// ============================================================================

id NSSearchField_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSSearchField* sf = (CamelOSSearchField*)self;
    if (!sf) return 0;
    
    sf->x = x; sf->y = y; sf->w = w; sf->h = h;
    sf->text[0] = 0;
    strcpy(sf->placeholder, "Search");
    sf->is_editable = 1;
    sf->text_color = 0xFF333333;
    sf->bg_color = 0xFFFFFFFF;
    sf->delegate = 0;
    sf->search_button = 0;
    sf->cancel_button = 0;
    
    return self;
}

void NSSearchField_setPlaceholder(id self, SEL cmd, const char* placeholder) {
    (void)cmd;
    CamelOSSearchField* sf = (CamelOSSearchField*)self;
    if (sf && placeholder) {
        strncpy(sf->placeholder, placeholder, 63);
        sf->placeholder[63] = 0;
    }
}

const char* NSSearchField_stringValue(id self, SEL cmd) {
    (void)cmd;
    CamelOSSearchField* sf = (CamelOSSearchField*)self;
    return sf ? sf->text : "";
}

static void register_NSSearchField_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSSearchField_initWithFrame, "@@:iiii");
    sel = sel_registerName("setPlaceholder:");
    class_addMethod(cls, sel, (void*)NSSearchField_setPlaceholder, "v@:*");
    sel = sel_registerName("stringValue");
    class_addMethod(cls, sel, (void*)NSSearchField_stringValue, "*@:");
}

// ============================================================================
// NSProgressIndicator Implementation
// ============================================================================

id NSProgressIndicator_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (!pi) return 0;
    
    pi->x = x; pi->y = y; pi->w = w; pi->h = h;
    pi->style = NSProgressIndicatorBarStyle;
    pi->min_value = 0.0;
    pi->max_value = 100.0;
    pi->double_value = 0.0;
    pi->is_indeterminate = 0;
    pi->is_animating = 0;
    pi->progress_color = 0xFF007AFF;
    pi->track_color = 0xFFE0E0E0;
    
    return self;
}

void NSProgressIndicator_setDoubleValue(id self, SEL cmd, double value) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->double_value = value;
}

void NSProgressIndicator_setMinValue(id self, SEL cmd, double value) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->min_value = value;
}

void NSProgressIndicator_setMaxValue(id self, SEL cmd, double value) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->max_value = value;
}

void NSProgressIndicator_startAnimation(id self, SEL cmd) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->is_animating = 1;
}

void NSProgressIndicator_stopAnimation(id self, SEL cmd) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->is_animating = 0;
}

void NSProgressIndicator_setStyle(id self, SEL cmd, int style) {
    (void)cmd;
    CamelOSProgressIndicator* pi = (CamelOSProgressIndicator*)self;
    if (pi) pi->style = style;
}

static void register_NSProgressIndicator_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_initWithFrame, "@@:iiii");
    sel = sel_registerName("setDoubleValue:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_setDoubleValue, "v@:d");
    sel = sel_registerName("setMinValue:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_setMinValue, "v@:d");
    sel = sel_registerName("setMaxValue:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_setMaxValue, "v@:d");
    sel = sel_registerName("startAnimation:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_startAnimation, "v@:@");
    sel = sel_registerName("stopAnimation:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_stopAnimation, "v@:@");
    sel = sel_registerName("setStyle:");
    class_addMethod(cls, sel, (void*)NSProgressIndicator_setStyle, "v@:i");
}

// ============================================================================
// NSSlider Implementation
// ============================================================================

id NSSlider_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSSlider* sl = (CamelOSSlider*)self;
    if (!sl) return 0;
    
    sl->x = x; sl->y = y; sl->w = w; sl->h = h;
    sl->min_value = 0.0;
    sl->max_value = 100.0;
    sl->current_value = 50.0;
    sl->is_vertical = 0;
    sl->target = 0;
    sl->action = 0;
    sl->tick_mark_count = 0;
    sl->track_color = 0xFFE0E0E0;
    sl->knob_color = 0xFFFFFFFF;
    
    return self;
}

void NSSlider_setDoubleValue(id self, SEL cmd, double value) {
    (void)cmd;
    CamelOSSlider* sl = (CamelOSSlider*)self;
    if (sl) sl->current_value = value;
}

double NSSlider_doubleValue(id self, SEL cmd) {
    (void)cmd;
    CamelOSSlider* sl = (CamelOSSlider*)self;
    return sl ? sl->current_value : 0.0;
}

void NSSlider_setTarget(id self, SEL cmd, id target) {
    (void)cmd;
    CamelOSSlider* sl = (CamelOSSlider*)self;
    if (sl) sl->target = target;
}

void NSSlider_setAction(id self, SEL cmd, SEL action) {
    (void)cmd;
    CamelOSSlider* sl = (CamelOSSlider*)self;
    if (sl) sl->action = action;
}

static void register_NSSlider_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSSlider_initWithFrame, "@@:iiii");
    sel = sel_registerName("setDoubleValue:");
    class_addMethod(cls, sel, (void*)NSSlider_setDoubleValue, "v@:d");
    sel = sel_registerName("doubleValue");
    class_addMethod(cls, sel, (void*)NSSlider_doubleValue, "d@:");
    sel = sel_registerName("setTarget:");
    class_addMethod(cls, sel, (void*)NSSlider_setTarget, "v@:@");
    sel = sel_registerName("setAction:");
    class_addMethod(cls, sel, (void*)NSSlider_setAction, "v@::");
}

// ============================================================================
// NSPopUpButton Implementation
// ============================================================================

id NSPopUpButton_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSPopUpButton* pb = (CamelOSPopUpButton*)self;
    if (!pb) return 0;
    
    pb->x = x; pb->y = y; pb->w = w; pb->h = h;
    pb->title[0] = 0;
    pb->item_count = 0;
    pb->selected_index = 0;
    pb->target = 0;
    pb->action = 0;
    pb->pulls_down = 0;
    pb->bg_color = 0xFFFFFFFF;
    pb->text_color = 0xFF333333;
    
    return self;
}

void NSPopUpButton_addItem(id self, SEL cmd, const char* title) {
    (void)cmd;
    CamelOSPopUpButton* pb = (CamelOSPopUpButton*)self;
    if (!pb || pb->item_count >= 16 || !title) return;
    
    strncpy(pb->items[pb->item_count], title, 63);
    pb->items[pb->item_count][63] = 0;
    if (pb->item_count == 0) {
        strncpy(pb->title, title, 63);
        pb->title[63] = 0;
    }
    pb->item_count++;
}

void NSPopUpButton_selectItem(id self, SEL cmd, int index) {
    (void)cmd;
    CamelOSPopUpButton* pb = (CamelOSPopUpButton*)self;
    if (!pb || index < 0 || index >= pb->item_count) return;
    
    pb->selected_index = index;
    strncpy(pb->title, pb->items[index], 63);
    pb->title[63] = 0;
    
    // Fire action
    if (pb->target && pb->action) {
        typedef id (*msgSend_fn)(id, SEL);
        ((msgSend_fn)objc_msgSend)(pb->target, pb->action);
    }
}

int NSPopUpButton_selectedIndex(id self, SEL cmd) {
    (void)cmd;
    CamelOSPopUpButton* pb = (CamelOSPopUpButton*)self;
    return pb ? pb->selected_index : -1;
}

static void register_NSPopUpButton_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSPopUpButton_initWithFrame, "@@:iiii");
    sel = sel_registerName("addItem:");
    class_addMethod(cls, sel, (void*)NSPopUpButton_addItem, "v@:*");
    sel = sel_registerName("selectItem:");
    class_addMethod(cls, sel, (void*)NSPopUpButton_selectItem, "v@:i");
    sel = sel_registerName("selectedIndex");
    class_addMethod(cls, sel, (void*)NSPopUpButton_selectedIndex, "i@:");
}

// ============================================================================
// NSCheckBox Implementation
// ============================================================================

id NSCheckBox_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSCheckBox* cb = (CamelOSCheckBox*)self;
    if (!cb) return 0;
    
    cb->x = x; cb->y = y; cb->w = w; cb->h = h;
    cb->title[0] = 0;
    cb->state = 0;
    cb->target = 0;
    cb->action = 0;
    cb->text_color = 0xFF333333;
    
    return self;
}

void NSCheckBox_setTitle(id self, SEL cmd, const char* title) {
    (void)cmd;
    CamelOSCheckBox* cb = (CamelOSCheckBox*)self;
    if (cb && title) {
        strncpy(cb->title, title, 63);
        cb->title[63] = 0;
    }
}

int NSCheckBox_state(id self, SEL cmd) {
    (void)cmd;
    CamelOSCheckBox* cb = (CamelOSCheckBox*)self;
    return cb ? cb->state : 0;
}

void NSCheckBox_setState(id self, SEL cmd, int state) {
    (void)cmd;
    CamelOSCheckBox* cb = (CamelOSCheckBox*)self;
    if (cb) cb->state = state;
}

static void register_NSCheckBox_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSCheckBox_initWithFrame, "@@:iiii");
    sel = sel_registerName("setTitle:");
    class_addMethod(cls, sel, (void*)NSCheckBox_setTitle, "v@:*");
    sel = sel_registerName("state");
    class_addMethod(cls, sel, (void*)NSCheckBox_state, "i@:");
    sel = sel_registerName("setState:");
    class_addMethod(cls, sel, (void*)NSCheckBox_setState, "v@:i");
}

// ============================================================================
// Additional AppKit Initialization
// ============================================================================

void appkit_extra_init(void) {
    s_printf("[AppKit] Initializing extra controls...\n");
    
    Class nsobj = objc_getClass("NSObject");
    
    // NSTableView
    NSTableView_class = objc_allocateClassPair(nsobj, "NSTableView",
        sizeof(CamelOSTableView) - sizeof(struct objc_object));
    if (NSTableView_class) {
        register_NSTableView_methods(NSTableView_class);
        objc_registerClassPair(NSTableView_class);
    }
    
    // NSRunLoop
    NSRunLoop_class = objc_allocateClassPair(nsobj, "NSRunLoop",
        sizeof(CamelOSRunLoop) - sizeof(struct objc_object));
    if (NSRunLoop_class) {
        register_NSRunLoop_methods(NSRunLoop_class);
        objc_registerClassPair(NSRunLoop_class);
    }
    
    // NSWorkspace
    NSWorkspace_class = objc_allocateClassPair(nsobj, "NSWorkspace",
        sizeof(CamelOSWorkspace) - sizeof(struct objc_object));
    if (NSWorkspace_class) {
        register_NSWorkspace_methods(NSWorkspace_class);
        objc_registerClassPair(NSWorkspace_class);
    }
    
    // NSSearchField
    NSSearchField_class = objc_allocateClassPair(nsobj, "NSSearchField",
        sizeof(CamelOSSearchField) - sizeof(struct objc_object));
    if (NSSearchField_class) {
        register_NSSearchField_methods(NSSearchField_class);
        objc_registerClassPair(NSSearchField_class);
    }
    
    // NSProgressIndicator
    NSProgressIndicator_class = objc_allocateClassPair(nsobj, "NSProgressIndicator",
        sizeof(CamelOSProgressIndicator) - sizeof(struct objc_object));
    if (NSProgressIndicator_class) {
        register_NSProgressIndicator_methods(NSProgressIndicator_class);
        objc_registerClassPair(NSProgressIndicator_class);
    }
    
    // NSSlider
    NSSlider_class = objc_allocateClassPair(nsobj, "NSSlider",
        sizeof(CamelOSSlider) - sizeof(struct objc_object));
    if (NSSlider_class) {
        register_NSSlider_methods(NSSlider_class);
        objc_registerClassPair(NSSlider_class);
    }
    
    // NSPopUpButton
    NSPopUpButton_class = objc_allocateClassPair(nsobj, "NSPopUpButton",
        sizeof(CamelOSPopUpButton) - sizeof(struct objc_object));
    if (NSPopUpButton_class) {
        register_NSPopUpButton_methods(NSPopUpButton_class);
        objc_registerClassPair(NSPopUpButton_class);
    }
    
    // NSCheckBox
    NSCheckBox_class = objc_allocateClassPair(nsobj, "NSCheckBox",
        sizeof(CamelOSCheckBox) - sizeof(struct objc_object));
    if (NSCheckBox_class) {
        register_NSCheckBox_methods(NSCheckBox_class);
        objc_registerClassPair(NSCheckBox_class);
    }
    
    s_printf("[AppKit] Extra controls initialized.\n");
}
