// usr/libs/browser_bridge.h - Modern JavaScript to Browser DOM Bridge Header
// Version 3.1 - Interface for connecting JS engine to browser DOM
// Fully wired to Native C DOM for SPA rendering

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
// PENDING HTML (from document.write)
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
// MODERN API IMPLEMENTATIONS (v3.1 - wired to native C DOM)
// ============================================================================

// document.getElementById
js_v2_value_t* js_bridge_document_getElementById(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// document.createElement - Creates REAL native C DOM node
js_v2_value_t* js_bridge_document_createElement(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// document.getElementsByTagName
js_v2_value_t* js_bridge_document_getElementsByTagName(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// document.querySelector (v3.1 - wired to native C)
js_v2_value_t* js_bridge_document_querySelector(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// document.write / writeln
js_v2_value_t* js_bridge_document_write(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_bridge_document_writeln(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.appendChild (v3.1 - wired to native C)
js_v2_value_t* js_bridge_element_appendChild(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.setAttribute (v3.1 - wired to native C)
js_v2_value_t* js_bridge_element_setAttribute(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.addEventListener
js_v2_value_t* js_bridge_element_addEventListener(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// element.innerHTML setter
void js_bridge_set_innerHTML(js_v2_engine_t* engine, js_v2_value_t* element, const char* html);

// window.fetch (v3.1 - Promise-based)
js_v2_value_t* js_bridge_window_fetch(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// window.setTimeout/setInterval
js_v2_value_t* js_bridge_window_setTimeout(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_bridge_window_setInterval(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
void js_bridge_window_clearTimeout(js_v2_engine_t* engine, int timer_id);
void js_bridge_window_clearInterval(js_v2_engine_t* engine, int timer_id);

// console.log
js_v2_value_t* js_bridge_console_log(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Get the JS engine instance
js_v2_engine_t* js_bridge_get_engine(void);

// Convert value to string
js_v2_value_t* js_v2_to_string(js_v2_engine_t* engine, js_v2_value_t* val);

// Convert value to number
js_v2_value_t* js_v2_to_number(js_v2_engine_t* engine, js_v2_value_t* val);

// Create undefined value
js_v2_value_t* js_v2_new_undefined(js_v2_engine_t* engine);

// Create null value
js_v2_value_t* js_v2_new_null(js_v2_engine_t* engine);

// Create boolean value
js_v2_value_t* js_v2_new_boolean(js_v2_engine_t* engine, int val);

// Create array
js_v2_value_t* js_v2_new_array(js_v2_engine_t* engine);

// Get object property
js_v2_value_t* js_v2_object_get(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key);

// Call function
js_v2_value_t* js_v2_call(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val, 
                          int argc, js_v2_value_t** args);

#endif // BROWSER_BRIDGE_H
