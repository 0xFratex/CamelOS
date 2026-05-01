// core/appkit_compat.c - AppKit Compatibility Layer Implementation
// Bridges macOS AppKit API calls to CamelOS window_server/compositor
// Provides NSApplication, NSWindow, NSView, NSMenu, NSTextField,
// NSButton, NSImageView, NSScrollView, NSColor, NSFont

#include "appkit_compat.h"
#include "foundation_stub.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../sys/cdl_defs.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/serial.h"
#include "../core/window_server.h"

// Kernel API
extern kernel_api_t g_kernel_api;

// --- Class References ---
Class NSApplication_class = 0;
Class NSWindow_class = 0;
Class NSView_class = 0;
Class NSMenu_class = 0;
Class NSMenuItem_class = 0;
Class NSControl_class = 0;
Class NSTextField_class = 0;
Class NSButton_class = 0;
Class NSImageView_class = 0;
Class NSScrollView_class = 0;
Class NSTextView_class = 0;
Class NSColor_class = 0;
Class NSFont_class = 0;

// ============================================================================
// Generic AppKit Window Paint / Mouse / Scroll Callbacks
// These bridge the window_server callback system to the AppKit view hierarchy.
// When an AppKit window is shown, these callbacks are registered so that
// the window_server can paint its content view tree and forward input events.
// ============================================================================

// Forward declarations
void appkit_window_paint(int x, int y, int w, int h);
void appkit_window_mouse(int lx, int ly, int btn);
void appkit_window_scroll(int delta);

// Helper: recursively compute absolute screen coordinates for a view
static void compute_view_abs_coords(CamelOSView* view, int parent_abs_x, int parent_abs_y) {
    if (!view) return;
    view->abs_x = parent_abs_x + view->x;
    view->abs_y = parent_abs_y + view->y;
    // Recurse into subviews
    if (view->subviews) {
        for (uint32_t i = 0; i < view->subviews->count; i++) {
            CamelOSView* sub = (CamelOSView*)view->subviews->objects[i];
            compute_view_abs_coords(sub, view->abs_x, view->abs_y);
        }
    }
}

// Helper: draw a single view (background + custom paint)
static void draw_view(CamelOSView* view, int win_x, int win_y) {
    if (!view || view->is_hidden) return;
    
    int vx = win_x + view->x;
    int vy = win_y + view->y;
    
    // Draw background (if opaque and not fully transparent)
    if (view->is_opaque && view->bg_color != 0x00000000) {
        gfx_fill_rect(vx, vy, view->w, view->h, view->bg_color);
    }
    
    // Custom paint callback
    if (view->paint_func) {
        typedef void (*vpcb)(int, int, int, int);
        ((vpcb)view->paint_func)(vx, vy, view->w, view->h);
    }
}

// Helper: draw an NSButton
static void draw_button(CamelOSButton* btn, int win_x, int win_y) {
    if (!btn) return;
    
    int bx = win_x + btn->x;
    int by = win_y + btn->y;
    
    uint32_t bg = btn->is_highlighted ? 0xFF0051D5 : btn->bg_color;
    if (btn->is_bordered) {
        gfx_fill_rounded_rect(bx, by, btn->w, btn->h, bg, 6);
        gfx_draw_string(bx + (btn->w - strlen(btn->title) * 8) / 2,
                        by + (btn->h - 14) / 2,
                        btn->title, btn->text_color);
    } else {
        gfx_draw_string(bx, by + (btn->h - 14) / 2, btn->title, btn->bg_color);
    }
}

// Helper: draw an NSTextField
static void draw_textfield(CamelOSTextField* field, int win_x, int win_y) {
    if (!field) return;
    
    int fx = win_x + field->x;
    int fy = win_y + field->y;
    
    if (field->bg_color != 0x00000000) {
        gfx_fill_rect(fx, fy, field->w > 0 ? field->w : strlen(field->text) * 8 + 8, 
                       field->h > 0 ? field->h : 20, field->bg_color);
    }
    gfx_draw_string(fx + 4, fy + 3, field->text, field->text_color);
}

// Helper: draw view tree recursively
static void draw_view_tree(id view_id, int win_x, int win_y) {
    if (!view_id) return;
    
    CamelOSView* view = (CamelOSView*)view_id;
    if (view->is_hidden) return;
    
    // Check if this view is actually an NSButton (check isa class)
    Class view_class = object_getClass(view_id);
    if (view_class == NSButton_class) {
        draw_button((CamelOSButton*)view, win_x, win_y);
    } else if (view_class == NSTextField_class) {
        draw_textfield((CamelOSTextField*)view, win_x, win_y);
    } else {
        // Generic NSView
        draw_view(view, win_x, win_y);
    }
    
    // Draw subviews (front to back)
    if (view->subviews) {
        for (uint32_t i = 0; i < view->subviews->count; i++) {
            draw_view_tree(view->subviews->objects[i], win_x + view->x, win_y + view->y);
        }
    }
}

// Generic AppKit window paint callback
// Called by window_server with the content area (x, y, w, h)
void appkit_window_paint(int x, int y, int w, int h) {
    // Find the CamelOSWindow that corresponds to this paint call
    // We search by matching the active window's paint_callback
    CamelOSWindow* app_win = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* ws_win = ws_get_window_at_index(i);
        if (ws_win && ws_win->paint_callback == (void*)appkit_window_paint) {
            // Search tracked objects for the CamelOSWindow with this ws_window
            for (int j = 0; j < g_object_tracking_count; j++) {
                CamelOSWindow* cw = (CamelOSWindow*)g_object_ptrs[j];
                if (cw && cw->ws_window == ws_win) {
                    app_win = cw;
                    break;
                }
            }
            if (app_win) {
                // Fill content area with the window's background
                gfx_fill_rect(x, y, w, h, 0xFFFFFFFF);
                
                // Draw the view hierarchy starting from content_view
                if (app_win->content_view) {
                    draw_view_tree(app_win->content_view, x, y);
                }
                return;
            }
        }
    }
}

// Helper: handle mouse hit-test on a view (returns 1 if consumed)
static int hit_test_view_tree(id view_id, int lx, int ly, int btn) {
    if (!view_id) return 0;
    
    CamelOSView* view = (CamelOSView*)view_id;
    if (view->is_hidden) return 0;
    
    // Check subviews first (front to back = reverse order for hit test)
    if (view->subviews && view->subviews->count > 0) {
        for (int i = (int)view->subviews->count - 1; i >= 0; i--) {
            CamelOSView* sub = (CamelOSView*)view->subviews->objects[i];
            // Check if click is within this subview's frame
            if (lx >= sub->x && lx < sub->x + sub->w &&
                ly >= sub->y && ly < sub->y + sub->h) {
                int sub_lx = lx - sub->x;
                int sub_ly = ly - sub->y;
                if (hit_test_view_tree((id)sub, sub_lx, sub_ly, btn)) {
                    return 1;
                }
            }
        }
    }
    
    // Check if this view itself handles clicks
    Class view_class = object_getClass(view_id);
    
    // NSButton: check for click and fire action
    if (view_class == NSButton_class) {
        CamelOSButton* btn = (CamelOSButton*)view;
        if (btn->target && btn->action && btn->is_bordered) {
            // Fire the action via objc_msgSend
            typedef id (*msgSend_fn)(id, SEL);
            ((msgSend_fn)objc_msgSend)(btn->target, btn->action);
            return 1;
        }
    }
    
    // Custom mouse handler on the view
    if (view->mouse_func) {
        typedef void (*vmcb)(int, int, int);
        ((vmcb)view->mouse_func)(lx, ly, btn);
        return 1;
    }
    
    return 0;
}

// Generic AppKit window mouse callback
void appkit_window_mouse(int lx, int ly, int btn) {
    // Find the AppKit window for the active window
    CamelOSWindow* app_win = 0;
    window_t* active = ws_get_active_window();
    if (!active || active->mouse_callback != (void*)appkit_window_mouse) return;
    
    for (int j = 0; j < g_object_tracking_count; j++) {
        CamelOSWindow* cw = (CamelOSWindow*)g_object_ptrs[j];
        if (cw && cw->ws_window == active) {
            app_win = cw;
            break;
        }
    }
    
    if (app_win && app_win->content_view && btn == 1) {
        hit_test_view_tree(app_win->content_view, lx, ly, btn);
    }
}

// Generic AppKit window scroll callback
void appkit_window_scroll(int delta) {
    // Forward scroll to the document view of any NSScrollView in the window
    CamelOSWindow* app_win = 0;
    window_t* active = ws_get_active_window();
    if (!active || active->scroll_callback != (void*)appkit_window_scroll) return;
    
    for (int j = 0; j < g_object_tracking_count; j++) {
        CamelOSWindow* cw = (CamelOSWindow*)g_object_ptrs[j];
        if (cw && cw->ws_window == active) {
            app_win = cw;
            break;
        }
    }
    
    if (app_win && app_win->content_view) {
        // Walk view tree looking for NSScrollView
        CamelOSView* view = (CamelOSView*)app_win->content_view;
        if (view->subviews) {
            for (uint32_t i = 0; i < view->subviews->count; i++) {
                CamelOSView* sub = (CamelOSView*)view->subviews->objects[i];
                Class sub_class = object_getClass((id)sub);
                if (sub_class == NSScrollView_class) {
                    CamelOSScrollView* scroll = (CamelOSScrollView*)sub;
                    scroll->scroll_y -= delta * 20;
                    if (scroll->scroll_y < 0) scroll->scroll_y = 0;
                }
            }
        }
    }
}

// ============================================================================
// NSApplication Implementation
// ============================================================================

id NSApplication_sharedApplication(void) {
    static CamelOSApplication* app = 0;
    if (!app) {
        app = (CamelOSApplication*)class_createInstance(NSApplication_class, 0);
        if (app) {
            strcpy(app->app_name, "CamelOS App");
            app->running = 0;
            app->finished = 0;
            app->delegate = 0;
            app->menu_bar = 0;
            track_object((id)app);
        }
    }
    return (id)app;
}

void NSApplication_setDelegate(id self, SEL cmd, id delegate) {
    (void)cmd;
    CamelOSApplication* app = (CamelOSApplication*)self;
    if (app) app->delegate = delegate;
}

void NSApplication_run(id self, SEL cmd) {
    (void)cmd;
    CamelOSApplication* app = (CamelOSApplication*)self;
    if (!app) return;

    app->running = 1;

    // Send applicationDidFinishLaunching: to delegate
    if (app->delegate) {
        SEL sel = sel_registerName("applicationDidFinishLaunching:");
        Method m = class_getInstanceMethod(object_getClass(app->delegate), sel);
        if (m && m->imp) {
            typedef void (*app_did_launch_fn)(id, SEL, id);
            ((app_did_launch_fn)m->imp)(app->delegate, sel, 0);
        }
    }

    // Main event loop
    while (app->running && !app->finished) {
        g_kernel_api.process_events();
        sys_delay(1);
    }

    // Send applicationWillTerminate: to delegate
    if (app->delegate) {
        SEL sel = sel_registerName("applicationWillTerminate:");
        Method m = class_getInstanceMethod(object_getClass(app->delegate), sel);
        if (m && m->imp) {
            typedef void (*app_will_term_fn)(id, SEL, id);
            ((app_will_term_fn)m->imp)(app->delegate, sel, 0);
        }
    }
}

void NSApplication_terminate(id self, SEL cmd, id sender) {
    (void)cmd; (void)sender;
    CamelOSApplication* app = (CamelOSApplication*)self;
    if (app) app->finished = 1;
}

void NSApplication_setMainMenu(id self, SEL cmd, id menu) {
    (void)cmd;
    CamelOSApplication* app = (CamelOSApplication*)self;
    if (app) app->menu_bar = (void*)menu;
}

id NSApplication_keyWindow(id self, SEL cmd) {
    (void)cmd;
    (void)self;
    // Return the current key window (simplified)
    window_t* active = ws_get_active_window();
    if (!active) return 0;
    // Look for the CamelOSWindow that corresponds to this window_t
    return 0;  // TODO: Maintain a mapping
}

static void register_NSApplication_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("setDelegate:");
    class_addMethod(cls, sel, (void*)NSApplication_setDelegate, "v@:@");
    sel = sel_registerName("run");
    class_addMethod(cls, sel, (void*)NSApplication_run, "v@:");
    sel = sel_registerName("terminate:");
    class_addMethod(cls, sel, (void*)NSApplication_terminate, "v@:@");
    sel = sel_registerName("setMainMenu:");
    class_addMethod(cls, sel, (void*)NSApplication_setMainMenu, "v@:@");
    sel = sel_registerName("keyWindow");
    class_addMethod(cls, sel, (void*)NSApplication_keyWindow, "@@:");
}

// ============================================================================
// NSWindow Implementation
// ============================================================================

id NSWindow_initWithContentRect(id self, SEL cmd,
    int x, int y, int w, int h, int style, void* backing, int defer) {
    (void)cmd; (void)backing; (void)defer;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win) return 0;

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->style_mask = style;
    win->content_view = 0;
    win->delegate = 0;
    win->visible = 0;
    win->key_window = 0;
    win->opacity = 1.0f;

    // Create the window_server window
    // Convert AppKit coords (origin bottom-left) to CamelOS coords (origin top-left)
    // For simplicity, we just pass through
    int ws_style = 0;
    if (style & NSWindowStyleMaskTitled) ws_style |= 0x00;  // STANDARD
    if (style & NSWindowStyleMaskBorderless) ws_style |= 0x02;
    if (style & NSWindowStyleMaskResizable) ws_style |= 0x00;  // Default is resizable

    win->ws_window = (void*)ws_create_window(win->title, w, h, 0, 0, 0);
    if (win->ws_window) {
        win->ws_window_id = ((window_t*)win->ws_window)->id;
    }

    return self;
}

void NSWindow_setTitle(id self, SEL cmd, const char* title) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win) return;
    strncpy(win->title, title, 63);
    win->title[63] = 0;
    if (win->ws_window) {
        ws_set_title((window_t*)win->ws_window, title);
    }
}

void NSWindow_setContentView(id self, SEL cmd, id view) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (win) win->content_view = view;
}

id NSWindow_contentView(id self, SEL cmd) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    return win ? win->content_view : 0;
}

void NSWindow_makeKeyAndOrderFront(id self, SEL cmd, id sender) {
    (void)cmd; (void)sender;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win || !win->ws_window) return;

    win->visible = 1;
    win->key_window = 1;
    window_t* ws_win = (window_t*)win->ws_window;
    ws_win->is_visible = 1;
    ws_win->is_focused = 1;
    
    // Set up generic AppKit paint and mouse callbacks so the window
    // can draw its view hierarchy and forward mouse events to views.
    ws_win->paint_callback = (void*)appkit_window_paint;
    ws_win->mouse_callback = (void*)appkit_window_mouse;
    ws_win->scroll_callback = (void*)appkit_window_scroll;
    
    ws_bring_to_front(ws_win);
}

void NSWindow_orderFront(id self, SEL cmd, id sender) {
    (void)cmd; (void)sender;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win || !win->ws_window) return;
    win->visible = 1;
    window_t* ws_win = (window_t*)win->ws_window;
    ws_win->is_visible = 1;
}

void NSWindow_orderOut(id self, SEL cmd, id sender) {
    (void)cmd; (void)sender;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win || !win->ws_window) return;
    win->visible = 0;
    window_t* ws_win = (window_t*)win->ws_window;
    ws_win->is_visible = 0;
}

void NSWindow_close(id self, SEL cmd) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win || !win->ws_window) return;
    ws_close((window_t*)win->ws_window);
    win->visible = 0;
    win->ws_window = 0;
}

void NSWindow_setDelegate(id self, SEL cmd, id delegate) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (win) win->delegate = delegate;
}

void NSWindow_center(id self, SEL cmd) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win) return;
    // Center on screen (assuming 800x600 default)
    win->x = (800 - win->w) / 2;
    win->y = (600 - win->h) / 2;
    if (win->ws_window) {
        ws_set_geometry((window_t*)win->ws_window, win->x, win->y, win->w, win->h);
    }
}

void NSWindow_setFrame(id self, SEL cmd, int x, int y, int w, int h, int display) {
    (void)cmd; (void)display;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win) return;
    win->x = x; win->y = y; win->w = w; win->h = h;
    if (win->ws_window) {
        ws_set_geometry((window_t*)win->ws_window, x, y, w, h);
    }
}

void NSWindow_setOpacity(id self, SEL cmd, float opacity) {
    (void)cmd;
    CamelOSWindow* win = (CamelOSWindow*)self;
    if (!win) return;
    win->opacity = opacity;
    if (win->ws_window) {
        ws_set_opacity((window_t*)win->ws_window, opacity);
    }
}

static void register_NSWindow_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithContentRect:styleMask:backing:defer:");
    class_addMethod(cls, sel, (void*)NSWindow_initWithContentRect, "@@:iiiiiii");
    sel = sel_registerName("setTitle:");
    class_addMethod(cls, sel, (void*)NSWindow_setTitle, "v@:*");
    sel = sel_registerName("setContentView:");
    class_addMethod(cls, sel, (void*)NSWindow_setContentView, "v@:@");
    sel = sel_registerName("contentView");
    class_addMethod(cls, sel, (void*)NSWindow_contentView, "@@:");
    sel = sel_registerName("makeKeyAndOrderFront:");
    class_addMethod(cls, sel, (void*)NSWindow_makeKeyAndOrderFront, "v@:@");
    sel = sel_registerName("orderFront:");
    class_addMethod(cls, sel, (void*)NSWindow_orderFront, "v@:@");
    sel = sel_registerName("orderOut:");
    class_addMethod(cls, sel, (void*)NSWindow_orderOut, "v@:@");
    sel = sel_registerName("close");
    class_addMethod(cls, sel, (void*)NSWindow_close, "v@:");
    sel = sel_registerName("setDelegate:");
    class_addMethod(cls, sel, (void*)NSWindow_setDelegate, "v@:@");
    sel = sel_registerName("center");
    class_addMethod(cls, sel, (void*)NSWindow_center, "v@:");
    sel = sel_registerName("setFrame:display:");
    class_addMethod(cls, sel, (void*)NSWindow_setFrame, "v@:iiiii");
    sel = sel_registerName("setOpacity:");
    class_addMethod(cls, sel, (void*)NSWindow_setOpacity, "v@:f");
}

// ============================================================================
// NSView Implementation
// ============================================================================

id NSView_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    if (!view) return 0;

    view->x = x; view->y = y; view->w = w; view->h = h;
    view->abs_x = x; view->abs_y = y;
    view->superview = 0;
    view->subviews = 0;
    view->needs_display = 1;
    view->bg_color = 0xFFFFFFFF;  // White
    view->paint_func = 0;
    view->mouse_func = 0;
    view->key_func = 0;
    view->is_opaque = 1;
    view->is_hidden = 0;

    return self;
}

void NSView_setNeedsDisplay(id self, SEL cmd, int needs) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    if (view) view->needs_display = needs;
}

void NSView_addSubview(id self, SEL cmd, id subview) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    CamelOSView* sub = (CamelOSView*)subview;
    if (!view || !sub) return;

    // Create subviews array if needed
    if (!view->subviews) {
        view->subviews = (CamelOSArray*)class_createInstance(NSArray_class, 0);
        if (view->subviews) {
            view->subviews->capacity = 8;
            view->subviews->objects = (id*)kmalloc(8 * sizeof(id));
            view->subviews->count = 0;
        }
    }

    if (view->subviews && view->subviews->count < view->subviews->capacity) {
        view->subviews->objects[view->subviews->count++] = subview;
    }

    sub->superview = self;
    view->needs_display = 1;
}

id NSView_superview(id self, SEL cmd) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    return view ? view->superview : 0;
}

void NSView_removeFromSuperview(id self, SEL cmd) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    if (!view || !view->superview) return;

    CamelOSView* super_view = (CamelOSView*)view->superview;
    if (super_view->subviews) {
        for (uint32_t i = 0; i < super_view->subviews->count; i++) {
            if (super_view->subviews->objects[i] == (id)self) {
                super_view->subviews->objects[i] = super_view->subviews->objects[super_view->subviews->count - 1];
                super_view->subviews->count--;
                break;
            }
        }
    }
    view->superview = 0;
}

void NSView_setFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    if (!view) return;
    view->x = x; view->y = y; view->w = w; view->h = h;
    view->needs_display = 1;
}

void NSView_setBackgroundColor(id self, SEL cmd, uint32_t color) {
    (void)cmd;
    CamelOSView* view = (CamelOSView*)self;
    if (view) {
        view->bg_color = color;
        view->needs_display = 1;
    }
}

static void register_NSView_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSView_initWithFrame, "@@:iiii");
    sel = sel_registerName("setNeedsDisplay:");
    class_addMethod(cls, sel, (void*)NSView_setNeedsDisplay, "v@:i");
    sel = sel_registerName("addSubview:");
    class_addMethod(cls, sel, (void*)NSView_addSubview, "v@:@");
    sel = sel_registerName("superview");
    class_addMethod(cls, sel, (void*)NSView_superview, "@@:");
    sel = sel_registerName("removeFromSuperview");
    class_addMethod(cls, sel, (void*)NSView_removeFromSuperview, "v@:");
    sel = sel_registerName("setFrame:");
    class_addMethod(cls, sel, (void*)NSView_setFrame, "v@:iiii");
    sel = sel_registerName("setBackgroundColor:");
    class_addMethod(cls, sel, (void*)NSView_setBackgroundColor, "v@:I");
}

// ============================================================================
// NSButton Implementation
// ============================================================================

id NSButton_buttonWithTitle(const char* title, id target, SEL action) {
    CamelOSButton* btn = (CamelOSButton*)class_createInstance(NSButton_class, 0);
    if (btn) {
        if (title) strncpy(btn->title, title, 63);
        btn->title[63] = 0;
        btn->target = target;
        btn->action = action;
        btn->button_type = 0;  // Push button
        btn->state = 0;
        btn->x = 0; btn->y = 0;
        btn->w = strlen(title) * 8 + 24;  // Auto-size based on title
        btn->h = 28;
        btn->bg_color = 0xFF007AFF;  // Blue
        btn->text_color = 0xFFFFFFFF;  // White
        btn->is_bordered = 1;
        btn->is_highlighted = 0;
        track_object((id)btn);
    }
    return (id)btn;
}

void NSButton_setTitle(id self, SEL cmd, const char* title) {
    (void)cmd;
    CamelOSButton* btn = (CamelOSButton*)self;
    if (btn && title) {
        strncpy(btn->title, title, 63);
        btn->title[63] = 0;
    }
}

void NSButton_setTarget(id self, SEL cmd, id target) {
    (void)cmd;
    CamelOSButton* btn = (CamelOSButton*)self;
    if (btn) btn->target = target;
}

void NSButton_setAction(id self, SEL cmd, SEL action) {
    (void)cmd;
    CamelOSButton* btn = (CamelOSButton*)self;
    if (btn) btn->action = action;
}

int NSButton_state(id self, SEL cmd) {
    (void)cmd;
    CamelOSButton* btn = (CamelOSButton*)self;
    return btn ? btn->state : 0;
}

void NSButton_setState(id self, SEL cmd, int state) {
    (void)cmd;
    CamelOSButton* btn = (CamelOSButton*)self;
    if (btn) btn->state = state;
}

static void register_NSButton_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("setTitle:");
    class_addMethod(cls, sel, (void*)NSButton_setTitle, "v@:*");
    sel = sel_registerName("setTarget:");
    class_addMethod(cls, sel, (void*)NSButton_setTarget, "v@:@");
    sel = sel_registerName("setAction:");
    class_addMethod(cls, sel, (void*)NSButton_setAction, "v@::");
    sel = sel_registerName("state");
    class_addMethod(cls, sel, (void*)NSButton_state, "i@:");
    sel = sel_registerName("setState:");
    class_addMethod(cls, sel, (void*)NSButton_setState, "v@:i");
    sel = sel_registerName("setFrame:");
    class_addMethod(cls, sel, (void*)NSView_setFrame, "v@:iiii");
}

// ============================================================================
// NSTextField Implementation
// ============================================================================

id NSTextField_labelWithString(const char* text) {
    CamelOSTextField* field = (CamelOSTextField*)class_createInstance(NSTextField_class, 0);
    if (field) {
        if (text) strncpy(field->text, text, 255);
        field->text[255] = 0;
        field->is_editable = 0;
        field->is_selectable = 0;
        field->alignment = 0;  // Left
        field->text_color = 0xFF333333;
        field->bg_color = 0x00000000;  // Transparent
        field->delegate = 0;
        field->font_size = 13;
        field->x = 0; field->y = 0;
        field->w = strlen(text) * 8 + 8;
        field->h = 20;
        track_object((id)field);
    }
    return (id)field;
}

void NSTextField_setStringValue(id self, SEL cmd, const char* text) {
    (void)cmd;
    CamelOSTextField* field = (CamelOSTextField*)self;
    if (field && text) {
        strncpy(field->text, text, 255);
        field->text[255] = 0;
    }
}

const char* NSTextField_stringValue(id self, SEL cmd) {
    (void)cmd;
    CamelOSTextField* field = (CamelOSTextField*)self;
    return field ? field->text : "";
}

void NSTextField_setEditable(id self, SEL cmd, int editable) {
    (void)cmd;
    CamelOSTextField* field = (CamelOSTextField*)self;
    if (field) field->is_editable = editable;
}

void NSTextField_setAlignment(id self, SEL cmd, int alignment) {
    (void)cmd;
    CamelOSTextField* field = (CamelOSTextField*)self;
    if (field) field->alignment = alignment;
}

static void register_NSTextField_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("setStringValue:");
    class_addMethod(cls, sel, (void*)NSTextField_setStringValue, "v@:*");
    sel = sel_registerName("stringValue");
    class_addMethod(cls, sel, (void*)NSTextField_stringValue, "*@:");
    sel = sel_registerName("setEditable:");
    class_addMethod(cls, sel, (void*)NSTextField_setEditable, "v@:i");
    sel = sel_registerName("setAlignment:");
    class_addMethod(cls, sel, (void*)NSTextField_setAlignment, "v@:i");
    sel = sel_registerName("setFrame:");
    class_addMethod(cls, sel, (void*)NSView_setFrame, "v@:iiii");
}

// ============================================================================
// NSScrollView Implementation
// ============================================================================

id NSScrollView_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSScrollView* scroll = (CamelOSScrollView*)self;
    if (!scroll) return 0;

    scroll->x = x; scroll->y = y; scroll->w = w; scroll->h = h;
    scroll->content_view = 0;
    scroll->document_view = 0;
    scroll->has_vertical_scroller = 1;
    scroll->has_horizontal_scroller = 0;
    scroll->scroll_x = 0;
    scroll->scroll_y = 0;
    scroll->content_size_w = w;
    scroll->content_size_h = h;

    return self;
}

void NSScrollView_setDocumentView(id self, SEL cmd, id view) {
    (void)cmd;
    CamelOSScrollView* scroll = (CamelOSScrollView*)self;
    if (scroll) scroll->document_view = view;
}

id NSScrollView_documentView(id self, SEL cmd) {
    (void)cmd;
    CamelOSScrollView* scroll = (CamelOSScrollView*)self;
    return scroll ? scroll->document_view : 0;
}

void NSScrollView_setHasVerticalScroller(id self, SEL cmd, int has) {
    (void)cmd;
    CamelOSScrollView* scroll = (CamelOSScrollView*)self;
    if (scroll) scroll->has_vertical_scroller = has;
}

static void register_NSScrollView_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSScrollView_initWithFrame, "@@:iiii");
    sel = sel_registerName("setDocumentView:");
    class_addMethod(cls, sel, (void*)NSScrollView_setDocumentView, "v@:@");
    sel = sel_registerName("documentView");
    class_addMethod(cls, sel, (void*)NSScrollView_documentView, "@@:");
    sel = sel_registerName("setHasVerticalScroller:");
    class_addMethod(cls, sel, (void*)NSScrollView_setHasVerticalScroller, "v@:i");
}

// ============================================================================
// NSColor Implementation
// ============================================================================

id NSColor_colorWithRGB(float r, float g, float b, float a) {
    CamelOSColor* color = (CamelOSColor*)class_createInstance(NSColor_class, 0);
    if (color) {
        color->r = r; color->g = g; color->b = b; color->a = a;
        // Pack to ABGR for CamelOS gfx
        color->rgba = ((uint32_t)(a * 255) << 24) |
                      ((uint32_t)(b * 255) << 16) |
                      ((uint32_t)(g * 255) << 8) |
                       (uint32_t)(r * 255);
        track_object((id)color);
    }
    return (id)color;
}

uint32_t NSColor_rgbaValue(id self, SEL cmd) {
    (void)cmd;
    CamelOSColor* color = (CamelOSColor*)self;
    return color ? color->rgba : 0xFF000000;
}

static void register_NSColor_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("rgbaValue");
    class_addMethod(cls, sel, (void*)NSColor_rgbaValue, "I@:");
}

// ============================================================================
// NSFont Implementation
// ============================================================================

id NSFont_systemFontOfSize(int size) {
    CamelOSFont* font = (CamelOSFont*)class_createInstance(NSFont_class, 0);
    if (font) {
        strcpy(font->font_name, "System");
        font->size = size;
        font->is_bold = 0;
        font->is_italic = 0;
        track_object((id)font);
    }
    return (id)font;
}

id NSFont_boldSystemFontOfSize(int size) {
    CamelOSFont* font = (CamelOSFont*)class_createInstance(NSFont_class, 0);
    if (font) {
        strcpy(font->font_name, "System");
        font->size = size;
        font->is_bold = 1;
        font->is_italic = 0;
        track_object((id)font);
    }
    return (id)font;
}

static void register_NSFont_methods(Class cls) {
    (void)cls;
    // No instance methods needed beyond NSObject
}

// ============================================================================
// NSMenu Implementation
// ============================================================================

id NSMenu_initWithTitle(id self, SEL cmd, const char* title) {
    (void)cmd;
    CamelOSMenu* menu = (CamelOSMenu*)self;
    if (menu && title) {
        strncpy(menu->title, title, 31);
        menu->title[31] = 0;
    }
    return self;
}

void NSMenu_addItem(id self, SEL cmd, id item) {
    (void)cmd;
    CamelOSMenu* menu = (CamelOSMenu*)self;
    if (!menu) return;

    if (!menu->items) {
        menu->items = (CamelOSArray*)class_createInstance(NSArray_class, 0);
        if (menu->items) {
            menu->items->capacity = 16;
            menu->items->objects = (id*)kmalloc(16 * sizeof(id));
            menu->items->count = 0;
        }
    }
    if (menu->items && menu->items->count < menu->items->capacity) {
        menu->items->objects[menu->items->count++] = item;
    }
}

static void register_NSMenu_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithTitle:");
    class_addMethod(cls, sel, (void*)NSMenu_initWithTitle, "@@:*");
    sel = sel_registerName("addItem:");
    class_addMethod(cls, sel, (void*)NSMenu_addItem, "v@:@");
}

// ============================================================================
// NSMenuItem Implementation
// ============================================================================

id NSMenuItem_initWithTitleActionKeyEquivalent(id self, SEL cmd,
    const char* title, SEL action, const char* key) {
    (void)cmd;
    CamelOSMenuItem* item = (CamelOSMenuItem*)self;
    if (item) {
        if (title) strncpy(item->title, title, 31);
        item->title[31] = 0;
        item->action = action;
        if (key) strncpy(item->key_equivalent, key, 7);
        item->key_equivalent[7] = 0;
        item->is_separator = 0;
        item->is_enabled = 1;
        item->target = 0;
    }
    return self;
}

id NSMenuItem_separatorItem(void) {
    CamelOSMenuItem* item = (CamelOSMenuItem*)class_createInstance(NSMenuItem_class, 0);
    if (item) {
        item->is_separator = 1;
        item->is_enabled = 0;
        track_object((id)item);
    }
    return (id)item;
}

static void register_NSMenuItem_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithTitle:action:keyEquivalent:");
    class_addMethod(cls, sel, (void*)NSMenuItem_initWithTitleActionKeyEquivalent, "@@:*:*");
}

// ============================================================================
// NSTextView Implementation
// ============================================================================

id NSTextView_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSTextView* tv = (CamelOSTextView*)self;
    if (!tv) return 0;

    tv->x = x; tv->y = y; tv->w = w; tv->h = h;
    tv->text_capacity = 4096;
    tv->text = (char*)kmalloc(tv->text_capacity);
    if (tv->text) tv->text[0] = 0;
    tv->text_length = 0;
    tv->is_editable = 1;
    tv->is_rich_text = 0;
    tv->text_color = 0xFF333333;
    tv->bg_color = 0xFFFFFFFF;
    tv->font_size = 13;
    tv->cursor_pos = 0;
    tv->scroll_offset = 0;

    return self;
}

void NSTextView_setString(id self, SEL cmd, const char* text) {
    (void)cmd;
    CamelOSTextView* tv = (CamelOSTextView*)self;
    if (!tv || !text) return;
    uint32_t len = strlen(text);
    if (len >= tv->text_capacity) len = tv->text_capacity - 1;
    memcpy(tv->text, text, len);
    tv->text[len] = 0;
    tv->text_length = len;
}

const char* NSTextView_string(id self, SEL cmd) {
    (void)cmd;
    CamelOSTextView* tv = (CamelOSTextView*)self;
    return tv ? tv->text : "";
}

void NSTextView_setEditable(id self, SEL cmd, int editable) {
    (void)cmd;
    CamelOSTextView* tv = (CamelOSTextView*)self;
    if (tv) tv->is_editable = editable;
}

static void register_NSTextView_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSTextView_initWithFrame, "@@:iiii");
    sel = sel_registerName("setString:");
    class_addMethod(cls, sel, (void*)NSTextView_setString, "v@:*");
    sel = sel_registerName("string");
    class_addMethod(cls, sel, (void*)NSTextView_string, "*@:");
    sel = sel_registerName("setEditable:");
    class_addMethod(cls, sel, (void*)NSTextView_setEditable, "v@:i");
}

// ============================================================================
// NSImageView Implementation
// ============================================================================

id NSImageView_initWithFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSImageView* iv = (CamelOSImageView*)self;
    if (!iv) return 0;
    iv->x = x; iv->y = y; iv->w = w; iv->h = h;
    iv->image_path[0] = 0;
    iv->image_data = 0;
    iv->image_w = 0;
    iv->image_h = 0;
    iv->scaling = 1;  // Proportional
    return self;
}

void NSImageView_setImagePath(id self, SEL cmd, const char* path) {
    (void)cmd;
    CamelOSImageView* iv = (CamelOSImageView*)self;
    if (iv && path) {
        strncpy(iv->image_path, path, 255);
        iv->image_path[255] = 0;
    }
}

static void register_NSImageView_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithFrame:");
    class_addMethod(cls, sel, (void*)NSImageView_initWithFrame, "@@:iiii");
    sel = sel_registerName("setImagePath:");
    class_addMethod(cls, sel, (void*)NSImageView_setImagePath, "v@:*");
}

// ============================================================================
// AppKit Initialization
// ============================================================================

void appkit_init(void) {
    s_printf("[AppKit] Initializing AppKit compatibility layer...\n");

    Class nsobj = objc_getClass("NSObject");

    // NSApplication
    NSApplication_class = objc_allocateClassPair(nsobj, "NSApplication",
        sizeof(CamelOSApplication) - sizeof(struct objc_object));
    if (NSApplication_class) {
        register_NSApplication_methods(NSApplication_class);
        objc_registerClassPair(NSApplication_class);
    }

    // NSWindow
    NSWindow_class = objc_allocateClassPair(nsobj, "NSWindow",
        sizeof(CamelOSWindow) - sizeof(struct objc_object));
    if (NSWindow_class) {
        register_NSWindow_methods(NSWindow_class);
        objc_registerClassPair(NSWindow_class);
    }

    // NSView
    NSView_class = objc_allocateClassPair(nsobj, "NSView",
        sizeof(CamelOSView) - sizeof(struct objc_object));
    if (NSView_class) {
        register_NSView_methods(NSView_class);
        objc_registerClassPair(NSView_class);
    }

    // NSButton
    NSButton_class = objc_allocateClassPair(nsobj, "NSButton",
        sizeof(CamelOSButton) - sizeof(struct objc_object));
    if (NSButton_class) {
        register_NSButton_methods(NSButton_class);
        objc_registerClassPair(NSButton_class);
    }

    // NSTextField
    NSTextField_class = objc_allocateClassPair(nsobj, "NSTextField",
        sizeof(CamelOSTextField) - sizeof(struct objc_object));
    if (NSTextField_class) {
        register_NSTextField_methods(NSTextField_class);
        objc_registerClassPair(NSTextField_class);
    }

    // NSScrollView
    NSScrollView_class = objc_allocateClassPair(nsobj, "NSScrollView",
        sizeof(CamelOSScrollView) - sizeof(struct objc_object));
    if (NSScrollView_class) {
        register_NSScrollView_methods(NSScrollView_class);
        objc_registerClassPair(NSScrollView_class);
    }

    // NSColor
    NSColor_class = objc_allocateClassPair(nsobj, "NSColor",
        sizeof(CamelOSColor) - sizeof(struct objc_object));
    if (NSColor_class) {
        register_NSColor_methods(NSColor_class);
        objc_registerClassPair(NSColor_class);
    }

    // NSFont
    NSFont_class = objc_allocateClassPair(nsobj, "NSFont",
        sizeof(CamelOSFont) - sizeof(struct objc_object));
    if (NSFont_class) {
        register_NSFont_methods(NSFont_class);
        objc_registerClassPair(NSFont_class);
    }

    // NSMenu
    NSMenu_class = objc_allocateClassPair(nsobj, "NSMenu",
        sizeof(CamelOSMenu) - sizeof(struct objc_object));
    if (NSMenu_class) {
        register_NSMenu_methods(NSMenu_class);
        objc_registerClassPair(NSMenu_class);
    }

    // NSMenuItem
    NSMenuItem_class = objc_allocateClassPair(nsobj, "NSMenuItem",
        sizeof(CamelOSMenuItem) - sizeof(struct objc_object));
    if (NSMenuItem_class) {
        register_NSMenuItem_methods(NSMenuItem_class);
        objc_registerClassPair(NSMenuItem_class);
    }

    // NSTextView
    NSTextView_class = objc_allocateClassPair(nsobj, "NSTextView",
        sizeof(CamelOSTextView) - sizeof(struct objc_object));
    if (NSTextView_class) {
        register_NSTextView_methods(NSTextView_class);
        objc_registerClassPair(NSTextView_class);
    }

    // NSImageView
    NSImageView_class = objc_allocateClassPair(nsobj, "NSImageView",
        sizeof(CamelOSImageView) - sizeof(struct objc_object));
    if (NSImageView_class) {
        register_NSImageView_methods(NSImageView_class);
        objc_registerClassPair(NSImageView_class);
    }

    // NSControl (base class)
    NSControl_class = objc_allocateClassPair(nsobj, "NSControl",
        sizeof(CamelOSControl) - sizeof(struct objc_object));
    if (NSControl_class) {
        objc_registerClassPair(NSControl_class);
    }

    s_printf("[AppKit] AppKit compatibility layer initialized\n");
}
