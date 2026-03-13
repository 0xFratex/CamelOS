// usr/libs/browser_bridge.h - JavaScript to Browser DOM Bridge Header
// Interface for connecting JS engine to browser DOM

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
// UTILITIES
// ============================================================================

// Get the JS engine instance
js_v2_engine_t* js_bridge_get_engine(void);

#endif // BROWSER_BRIDGE_H
