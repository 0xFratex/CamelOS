// usr/libs/browser_js_bridge.c - JavaScript to Browser DOM Bridge
// This file connects the JS engine to the browser's DOM, enabling dynamic content
// Version 1.0 - Core DOM manipulation support

#include "browser_bridge.h"
#include "js_engine_v2.h"
#include "../../core/memory.h"
#include "../../core/string.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define MAX_DYNAMIC_NODES 128
#define MAX_PENDING_HTML 8192

// ============================================================================
// BRIDGE STATE
// ============================================================================
static js_v2_engine_t js_bridge_engine;
static int js_bridge_initialized = 0;

// Pending HTML from document.write() calls
static char pending_html[MAX_PENDING_HTML];
static int pending_html_len = 0;

// Reference to browser's DOM (set by browser)
static void* browser_document = 0;
static void* browser_current_url = 0;

// Callbacks to browser
static void (*browser_reparse_html_cb)(const char* html, int len) = 0;
static void (*browser_invalidate_cb)(void) = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

void js_bridge_init(void) {
    if (js_bridge_initialized) return;
    
    js_v2_init(&js_bridge_engine);
    pending_html[0] = 0;
    pending_html_len = 0;
    js_bridge_initialized = 1;
    
    // Set up log callback
    js_bridge_engine.log_callback = js_bridge_console_log_handler;
}

void js_bridge_set_browser_context(void* document, void* current_url) {
    browser_document = document;
    browser_current_url = current_url;
}

void js_bridge_set_callbacks(void (*reparse_cb)(const char*, int), void (*invalidate_cb)(void)) {
    browser_reparse_html_cb = reparse_cb;
    browser_invalidate_cb = invalidate_cb;
}

void js_bridge_reset(void) {
    pending_html[0] = 0;
    pending_html_len = 0;
    js_v2_clear_error(&js_bridge_engine);
}

// ============================================================================
// CONSOLE LOG HANDLER
// ============================================================================

void js_bridge_console_log_handler(const char* message) {
    // Forward to kernel debug output
    extern void s_printf(const char*);
    if (message) {
        s_printf("[JS] ");
        s_printf(message);
        s_printf("\n");
    }
}

// ============================================================================
// DOCUMENT.WRITE IMPLEMENTATION
// ============================================================================

js_v2_value_t* js_bridge_document_write(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1) return js_v2_new_undefined(engine);
    
    // Get the string to write
    js_v2_value_t* str_val = js_v2_to_string(engine, args[0]);
    const char* html = str_val->data.string;
    int html_len = strlen(html);
    
    // Append to pending HTML buffer
    if (pending_html_len + html_len < MAX_PENDING_HTML - 1) {
        strcpy(pending_html + pending_html_len, html);
        pending_html_len += html_len;
        pending_html[pending_html_len] = 0;
    }
    
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_bridge_document_writeln(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    // Call document.write then add newline
    js_v2_value_t* result = js_bridge_document_write(engine, argc, args);
    
    if (pending_html_len + 1 < MAX_PENDING_HTML - 1) {
        pending_html[pending_html_len++] = '\n';
        pending_html[pending_html_len] = 0;
    }
    
    return result;
}

// ============================================================================
// DOCUMENT.GETELEMENTBYID IMPLEMENTATION
// ============================================================================

js_v2_value_t* js_bridge_document_getElementById(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1) return js_v2_new_null(engine);
    
    // Get the ID to search for
    js_v2_value_t* str_val = js_v2_to_string(engine, args[0]);
    const char* id = str_val->data.string;
    
    // Search browser's DOM for element with this ID
    // For now, return a placeholder object
    js_v2_value_t* element = js_v2_new_object(engine);
    
    // Store reference to element (would need actual DOM search)
    js_v2_object_set(engine, element, "id", js_v2_new_string(engine, id));
    js_v2_object_set(engine, element, "_is_dom_element", js_v2_new_boolean(engine, 1));
    
    return element;
}

// ============================================================================
// DOCUMENT.CREATEELEMENT IMPLEMENTATION
// ============================================================================

js_v2_value_t* js_bridge_document_createElement(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1) return js_v2_new_null(engine);
    
    js_v2_value_t* str_val = js_v2_to_string(engine, args[0]);
    const char* tag_name = str_val->data.string;
    
    // Create a new element object
    js_v2_value_t* element = js_v2_new_object(engine);
    
    // Set element properties
    js_v2_object_set(engine, element, "tagName", js_v2_new_string(engine, tag_name));
    js_v2_object_set(engine, element, "innerHTML", js_v2_new_string(engine, ""));
    js_v2_object_set(engine, element, "id", js_v2_new_string(engine, ""));
    js_v2_object_set(engine, element, "className", js_v2_new_string(engine, ""));
    js_v2_object_set(engine, element, "_is_dom_element", js_v2_new_boolean(engine, 1));
    
    // Create empty style object
    js_v2_value_t* style = js_v2_new_object(engine);
    js_v2_object_set(engine, element, "style", style);
    
    return element;
}

// ============================================================================
// ELEMENT.INNERHTML SETTER
// ============================================================================

void js_bridge_set_innerHTML(js_v2_engine_t* engine, js_v2_value_t* element, const char* html) {
    if (!element || element->type != JS_V2_TYPE_OBJECT) return;
    
    // Update the element's innerHTML property
    js_v2_object_set(engine, element, "innerHTML", js_v2_new_string(engine, html));
    
    // If this is the document.body, append to pending HTML
    js_v2_value_t* tag = js_v2_object_get(engine, element, "tagName");
    if (tag && tag->type == JS_V2_TYPE_STRING) {
        if (strcmp(tag->data.string, "BODY") == 0 || strcmp(tag->data.string, "body") == 0) {
            int html_len = strlen(html);
            if (pending_html_len + html_len < MAX_PENDING_HTML - 1) {
                strcpy(pending_html + pending_html_len, html);
                pending_html_len += html_len;
                pending_html[pending_html_len] = 0;
            }
        }
    }
}

// ============================================================================
// DOCUMENT.GETELEMENTS BY TAG NAME
// ============================================================================

js_v2_value_t* js_bridge_document_getElementsByTagName(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1) return js_v2_new_array(engine);
    
    js_v2_value_t* str_val = js_v2_to_string(engine, args[0]);
    const char* tag_name = str_val->data.string;
    
    // Return an array-like object (HTMLCollection)
    js_v2_value_t* collection = js_v2_new_object(engine);
    js_v2_object_set(engine, collection, "length", js_v2_new_number(engine, 0));
    
    return collection;
}

// ============================================================================
// WINDOW.SETTIMEOUT/SETINTERVAL
// ============================================================================

#define MAX_TIMERS 16

typedef struct {
    js_v2_value_t* callback;
    uint32_t target_time;
    int interval;
    int active;
} js_timer_t;

static js_timer_t js_timers[MAX_TIMERS];
static int js_timer_count = 0;

js_v2_value_t* js_bridge_window_setTimeout(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 2) return js_v2_new_number(engine, -1);
    
    // Find free timer slot
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!js_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return js_v2_new_number(engine, -1);
    
    // Get callback and delay
    js_v2_value_t* callback = args[0];
    js_v2_value_t* delay_val = js_v2_to_number(engine, args[1]);
    int delay = (int)delay_val->data.number;
    
    // Set up timer
    extern uint32_t get_tick_count(void);
    js_timers[slot].callback = callback;
    js_timers[slot].target_time = get_tick_count() + delay;
    js_timers[slot].interval = 0;
    js_timers[slot].active = 1;
    
    if (slot >= js_timer_count) js_timer_count = slot + 1;
    
    return js_v2_new_number(engine, slot);
}

js_v2_value_t* js_bridge_window_setInterval(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 2) return js_v2_new_number(engine, -1);
    
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!js_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return js_v2_new_number(engine, -1);
    
    js_v2_value_t* callback = args[0];
    js_v2_value_t* delay_val = js_v2_to_number(engine, args[1]);
    int delay = (int)delay_val->data.number;
    
    extern uint32_t get_tick_count(void);
    js_timers[slot].callback = callback;
    js_timers[slot].target_time = get_tick_count() + delay;
    js_timers[slot].interval = delay;
    js_timers[slot].active = 1;
    
    if (slot >= js_timer_count) js_timer_count = slot + 1;
    
    return js_v2_new_number(engine, slot);
}

void js_bridge_window_clearTimeout(js_v2_engine_t* engine, int timer_id) {
    if (timer_id >= 0 && timer_id < MAX_TIMERS) {
        js_timers[timer_id].active = 0;
    }
}

void js_bridge_window_clearInterval(js_v2_engine_t* engine, int timer_id) {
    if (timer_id >= 0 && timer_id < MAX_TIMERS) {
        js_timers[timer_id].active = 0;
    }
}

// ============================================================================
// TIMER PROCESSING
// ============================================================================

void js_bridge_process_timers(void) {
    extern uint32_t get_tick_count(void);
    uint32_t now = get_tick_count();
    
    for (int i = 0; i < js_timer_count; i++) {
        if (js_timers[i].active && now >= js_timers[i].target_time) {
            // Execute callback
            if (js_timers[i].callback && js_timers[i].callback->type == JS_V2_TYPE_FUNCTION) {
                js_v2_call(&js_bridge_engine, js_timers[i].callback, NULL, 0, NULL);
            }
            
            if (js_timers[i].interval > 0) {
                // Reschedule for interval
                js_timers[i].target_time = now + js_timers[i].interval;
            } else {
                // One-shot timer
                js_timers[i].active = 0;
            }
        }
    }
}

// ============================================================================
// SCRIPT EXECUTION
// ============================================================================

int js_bridge_execute_script(const char* script) {
    if (!js_bridge_initialized) {
        js_bridge_init();
    }
    
    // Execute the script
    js_v2_value_t* result = js_v2_eval(&js_bridge_engine, script);
    
    if (js_bridge_engine.has_error) {
        js_bridge_console_log_handler(js_bridge_engine.error_msg);
        return -1;
    }
    
    return 0;
}

// ============================================================================
// GET PENDING HTML
// ============================================================================

const char* js_bridge_get_pending_html(void) {
    return pending_html;
}

int js_bridge_get_pending_html_len(void) {
    return pending_html_len;
}

void js_bridge_clear_pending_html(void) {
    pending_html[0] = 0;
    pending_html_len = 0;
}

// ============================================================================
// REGISTER BROWSER APIS
// ============================================================================

void js_bridge_register_browser_apis(void) {
    if (!js_bridge_initialized) {
        js_bridge_init();
    }
    
    // Register document methods
    js_v2_value_t* document = js_bridge_engine.document_object;
    
    // document.write
    js_v2_value_t* write_fn = js_v2_new_function(&js_bridge_engine, "write");
    if (write_fn && write_fn->data.function) {
        write_fn->data.function->native_fn = js_bridge_document_write;
        write_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, document, "write", write_fn);
    
    // document.writeln
    js_v2_value_t* writeln_fn = js_v2_new_function(&js_bridge_engine, "writeln");
    if (writeln_fn && writeln_fn->data.function) {
        writeln_fn->data.function->native_fn = js_bridge_document_writeln;
        writeln_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, document, "writeln", writeln_fn);
    
    // document.getElementById
    js_v2_value_t* getElementById_fn = js_v2_new_function(&js_bridge_engine, "getElementById");
    if (getElementById_fn && getElementById_fn->data.function) {
        getElementById_fn->data.function->native_fn = js_bridge_document_getElementById;
        getElementById_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, document, "getElementById", getElementById_fn);
    
    // document.createElement
    js_v2_value_t* createElement_fn = js_v2_new_function(&js_bridge_engine, "createElement");
    if (createElement_fn && createElement_fn->data.function) {
        createElement_fn->data.function->native_fn = js_bridge_document_createElement;
        createElement_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, document, "createElement", createElement_fn);
    
    // document.getElementsByTagName
    js_v2_value_t* getElementsByTagName_fn = js_v2_new_function(&js_bridge_engine, "getElementsByTagName");
    if (getElementsByTagName_fn && getElementsByTagName_fn->data.function) {
        getElementsByTagName_fn->data.function->native_fn = js_bridge_document_getElementsByTagName;
        getElementsByTagName_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, document, "getElementsByTagName", getElementsByTagName_fn);
    
    // Register window methods
    js_v2_value_t* window = js_bridge_engine.window_object;
    
    // window.setTimeout
    js_v2_value_t* setTimeout_fn = js_v2_new_function(&js_bridge_engine, "setTimeout");
    if (setTimeout_fn && setTimeout_fn->data.function) {
        setTimeout_fn->data.function->native_fn = js_bridge_window_setTimeout;
        setTimeout_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, window, "setTimeout", setTimeout_fn);
    
    // window.setInterval
    js_v2_value_t* setInterval_fn = js_v2_new_function(&js_bridge_engine, "setInterval");
    if (setInterval_fn && setInterval_fn->data.function) {
        setInterval_fn->data.function->native_fn = js_bridge_window_setInterval;
        setInterval_fn->data.function->is_native = 1;
    }
    js_v2_object_set(&js_bridge_engine, window, "setInterval", setInterval_fn);
    
    // Add document.body placeholder
    js_v2_value_t* body = js_v2_new_object(&js_bridge_engine);
    js_v2_object_set(&js_bridge_engine, body, "tagName", js_v2_new_string(&js_bridge_engine, "BODY"));
    js_v2_object_set(&js_bridge_engine, body, "innerHTML", js_v2_new_string(&js_bridge_engine, ""));
    js_v2_object_set(&js_bridge_engine, document, "body", body);
    
    // Add document.location
    js_v2_value_t* location = js_v2_new_object(&js_bridge_engine);
    js_v2_object_set(&js_bridge_engine, location, "href", js_v2_new_string(&js_bridge_engine, "about:blank"));
    js_v2_object_set(&js_bridge_engine, document, "location", location);
    js_v2_object_set(&js_bridge_engine, window, "location", location);
    
    // Add common globals
    js_v2_set_global(&js_bridge_engine, "alert", js_v2_new_function(&js_bridge_engine, "alert"));
    js_v2_set_global(&js_bridge_engine, "parseInt", js_v2_new_function(&js_bridge_engine, "parseInt"));
    js_v2_set_global(&js_bridge_engine, "parseFloat", js_v2_new_function(&js_bridge_engine, "parseFloat"));
    js_v2_set_global(&js_bridge_engine, "isNaN", js_v2_new_function(&js_bridge_engine, "isNaN"));
}

// ============================================================================
// GET ENGINE INSTANCE
// ============================================================================

js_v2_engine_t* js_bridge_get_engine(void) {
    return &js_bridge_engine;
}
