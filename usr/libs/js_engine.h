// usr/libs/js_engine.h - Lightweight JavaScript Engine for Camel OS
// A minimal JS interpreter designed for embedded systems
#ifndef JS_ENGINE_H
#define JS_ENGINE_H

#include "../../include/types.h"

// ============================================================================
// Configuration
// ============================================================================
#define JS_MAX_HEAP_SIZE 65536      // 64KB heap for JS objects
#define JS_MAX_STRING_LEN 256
#define JS_MAX_VARIABLES 128
#define JS_MAX_FUNCTIONS 32
#define JS_MAX_CALL_STACK 16
#define JS_MAX_OBJECTS 64
#define JS_MAX_ARRAY_SIZE 32

// ============================================================================
// JavaScript Types
// ============================================================================
typedef enum {
    JS_TYPE_UNDEFINED,
    JS_TYPE_NULL,
    JS_TYPE_BOOLEAN,
    JS_TYPE_NUMBER,
    JS_TYPE_STRING,
    JS_TYPE_OBJECT,
    JS_TYPE_ARRAY,
    JS_TYPE_FUNCTION,
    JS_TYPE_NATIVE_FUNCTION
} js_type_t;

// ============================================================================
// JavaScript Value
// ============================================================================
typedef struct js_value js_value_t;
typedef struct js_object js_object_t;
typedef struct js_array js_array_t;

typedef union {
    int boolean;
    int number;
    char string[256];  // Inline string storage
    js_object_t* object;
    js_array_t* array;
    int function_id;
    js_value_t* (*native_fn)(int argc, js_value_t** args);
} js_value_data_t;

struct js_value {
    js_type_t type;
    js_value_data_t data;
    int ref_count;
};

// ============================================================================
// JavaScript Object
// ============================================================================
typedef struct {
    char key[64];
    js_value_t* value;
} js_property_t;

struct js_object {
    js_property_t properties[16];
    int property_count;
    js_object_t* prototype;
};

// ============================================================================
// JavaScript Array
// ============================================================================
struct js_array {
    js_value_t* elements[JS_MAX_ARRAY_SIZE];
    int length;
};

// ============================================================================
// JavaScript Variable
// ============================================================================
typedef struct {
    char name[64];
    js_value_t* value;
    int scope_level;
} js_variable_t;

// ============================================================================
// JavaScript Function
// ============================================================================
typedef struct {
    char name[64];
    char* body;             // Function source code
    char params[8][32];     // Parameter names
    int param_count;
    int is_native;
    js_value_t* (*native_fn)(int argc, js_value_t** args);
} js_function_t;

// ============================================================================
// JavaScript Call Frame
// ============================================================================
typedef struct {
    js_function_t* function;
    js_variable_t locals[32];
    int local_count;
    int return_address;
    js_value_t* this_value;
} js_call_frame_t;

// ============================================================================
// JavaScript Engine State
// ============================================================================
typedef struct {
    // Heap management
    uint8_t heap[JS_MAX_HEAP_SIZE];
    int heap_used;
    
    // Global scope
    js_variable_t globals[JS_MAX_VARIABLES];
    int global_count;
    
    // Functions
    js_function_t functions[JS_MAX_FUNCTIONS];
    int function_count;
    
    // Call stack
    js_call_frame_t call_stack[JS_MAX_CALL_STACK];
    int call_stack_top;
    
    // Object pool
    js_value_t values[256];
    int value_count;
    
    // Error state
    char error_msg[256];
    int has_error;
    
    // DOM bindings (browser-specific)
    void* dom_document;
    void* dom_window;
    
    // Output callback
    void (*print_callback)(const char* str);
    
} js_engine_t;

// ============================================================================
// Engine API
// ============================================================================

// Initialize the JavaScript engine
void js_init(js_engine_t* engine);

// Execute JavaScript code
js_value_t* js_eval(js_engine_t* engine, const char* code);

// Execute JavaScript code from file
js_value_t* js_eval_file(js_engine_t* engine, const char* filename);

// Register a native function
int js_register_native(js_engine_t* engine, const char* name, 
                       js_value_t* (*fn)(int argc, js_value_t** args));

// Get a global variable
js_value_t* js_get_global(js_engine_t* engine, const char* name);

// Set a global variable
void js_set_global(js_engine_t* engine, const char* name, js_value_t* value);

// Create values
js_value_t* js_new_undefined(js_engine_t* engine);
js_value_t* js_new_null(js_engine_t* engine);
js_value_t* js_new_boolean(js_engine_t* engine, int value);
js_value_t* js_new_number(js_engine_t* engine, int value);
js_value_t* js_new_string(js_engine_t* engine, const char* value);
js_value_t* js_new_object(js_engine_t* engine);
js_value_t* js_new_array(js_engine_t* engine);
js_value_t* js_new_function(js_engine_t* engine, const char* name, const char* body);

// Value operations
js_value_t* js_to_string(js_engine_t* engine, js_value_t* value);
js_value_t* js_to_number(js_engine_t* engine, js_value_t* value);
js_value_t* js_to_boolean(js_engine_t* engine, js_value_t* value);

// Object operations
void js_object_set(js_engine_t* engine, js_value_t* obj, const char* key, js_value_t* value);
js_value_t* js_object_get(js_engine_t* engine, js_value_t* obj, const char* key);

// Array operations
void js_array_push(js_engine_t* engine, js_value_t* arr, js_value_t* value);
js_value_t* js_array_get(js_engine_t* engine, js_value_t* arr, int index);
int js_array_length(js_engine_t* engine, js_value_t* arr);

// Memory management
void js_value_ref(js_value_t* value);
void js_value_unref(js_value_t* value);

// Error handling
const char* js_get_error(js_engine_t* engine);
void js_clear_error(js_engine_t* engine);

// ============================================================================
// Browser DOM Bindings
// ============================================================================

// Register browser-specific APIs
void js_register_dom_api(js_engine_t* engine);

// console object
js_value_t* js_console_log(int argc, js_value_t** args);
js_value_t* js_console_error(int argc, js_value_t** args);
js_value_t* js_console_warn(int argc, js_value_t** args);

// document object
js_value_t* js_document_getElementById(int argc, js_value_t** args);
js_value_t* js_document_querySelector(int argc, js_value_t** args);
js_value_t* js_document_querySelectorAll(int argc, js_value_t** args);

// Element manipulation
js_value_t* js_element_setAttribute(int argc, js_value_t** args);
js_value_t* js_element_getAttribute(int argc, js_value_t** args);
js_value_t* js_element_setInnerHTML(int argc, js_value_t** args);
js_value_t* js_element_getInnerHTML(int argc, js_value_t** args);

// window object
js_value_t* js_window_alert(int argc, js_value_t** args);
js_value_t* js_window_setTimeout(int argc, js_value_t** args);
js_value_t* js_window_setInterval(int argc, js_value_t** args);

#endif // JS_ENGINE_H
