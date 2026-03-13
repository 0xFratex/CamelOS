// usr/libs/browser_bridge.h - Modern JavaScript to Browser DOM Bridge Header
// Version 3.0 - Interface for connecting JS engine to browser DOM
// Supports React/Vue/SPA frameworks with proper DOM manipulation APIs

#ifndef BROWSER_BRIDGE_H
#define BROWSER_BRIDGE_H

#include "js_engine_v2.h"

// ============================================================================
// INITIALIZATION
// ============================================================================

// Initialize the JS bridge
void js_bridge_init(void);

// Set browser context pointers
void js_bridge_set_browser_context(void* document, void* current_url);

// Set callbacks for DOM updates
void js_bridge_set_callbacks(void (*reparse_cb)(const char*, int), void (*invalidate_cb)(void));

// Reset bridge state for new page
void js_bridge_reset(void);

// ============================================================================
// SCRIPT EXECUTION
// ============================================================================

// Execute JavaScript code
// Returns 0 on success, -1 on error
int js_bridge_execute_script(const char* script);

// Register browser APIs with the JS engine
void js_bridge_register_browser_apis(void);

// ============================================================================
// PENDING HTML
// ============================================================================

// Get HTML accumulated from document.write() calls
const char* js_bridge_get_pending_html(void);
int js_bridge_get_pending_html_len(void);

// Clear pending HTML buffer
void js_bridge_clear_pending_html(void);

// ============================================================================
// TIMERS
// ============================================================================

// Process pending timers (call from main loop)
void js_bridge_process_timers(void);

// ============================================================================
// CALLBACKS
// ============================================================================

// Console log handler
void js_bridge_console_log_handler(const char* message);

// ============================================================================
// MODERN API IMPLEMENTATIONS (v3.0)
// ============================================================================

// window.fetch - Returns a Promise-like object
js_v2_value_t* js_bridge_window_fetch(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.appendChild
js_v2_value_t* js_bridge_element_appendChild(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.addEventListener
js_v2_value_t* js_bridge_element_addEventListener(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// console.log
js_v2_value_t* js_bridge_console_log(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// document.querySelector (enhanced)
js_v2_value_t* js_bridge_document_querySelector(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// ============================================================================
// UTILITIES
// ============================================================================

// Get the JS engine instance
js_v2_engine_t* js_bridge_get_engine(void);

// Helper: Convert value to string
js_v2_value_t* js_v2_to_string(js_v2_engine_t* engine, js_v2_value_t* val);

// Helper: Convert value to number
js_v2_value_t* js_v2_to_number(js_v2_engine_t* engine, js_v2_value_t* val);

// Helper: Create undefined value
js_v2_value_t* js_v2_new_undefined(js_v2_engine_t* engine);

// Helper: Create null value
js_v2_value_t* js_v2_new_null(js_v2_engine_t* engine);

// Helper: Create boolean value
js_v2_value_t* js_v2_new_boolean(js_v2_engine_t* engine, int val);

// Helper: Create array
js_v2_value_t* js_v2_new_array(js_v2_engine_t* engine);

// Helper: Get object property
js_v2_value_t* js_v2_object_get(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key);

// Helper: Call function
js_v2_value_t* js_v2_call(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val, 
                          int argc, js_v2_value_t** args);

#endif // BROWSER_BRIDGE_H
