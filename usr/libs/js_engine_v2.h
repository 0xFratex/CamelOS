// usr/libs/js_engine_v2.h - Enhanced JavaScript Engine with ES6+ Support
// Version 2.0 - Modern JavaScript features for better web compatibility

#ifndef JS_ENGINE_V2_H
#define JS_ENGINE_V2_H

#include <types.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
#define JS_V2_MAX_VARIABLES     256
#define JS_V2_MAX_FUNCTIONS     64
#define JS_V2_MAX_OBJECTS       128
#define JS_V2_MAX_ARRAYS        64
#define JS_V2_MAX_STRING_LEN    512
#define JS_V2_MAX_STACK         256
#define JS_V2_MAX_SCOPES        16
#define JS_V2_MAX_PROMISES      32
#define JS_V2_MAX_CALLBACKS     64

// ============================================================================
// VALUE TYPES - Extended for ES6+
// ============================================================================
typedef enum {
    JS_V2_TYPE_UNDEFINED = 0,
    JS_V2_TYPE_NULL,
    JS_V2_TYPE_BOOLEAN,
    JS_V2_TYPE_NUMBER,
    JS_V2_TYPE_STRING,
    JS_V2_TYPE_OBJECT,
    JS_V2_TYPE_ARRAY,
    JS_V2_TYPE_FUNCTION,
    JS_V2_TYPE_SYMBOL,          // ES6 Symbol
    JS_V2_TYPE_BIGINT,          // ES2020 BigInt
    JS_V2_TYPE_PROMISE,         // ES6 Promise
    JS_V2_TYPE_MAP,             // ES6 Map
    JS_V2_TYPE_SET,             // ES6 Set
    JS_V2_TYPE_DATE,            // Date object
    JS_V2_TYPE_REGEXP,          // RegExp
    JS_V2_TYPE_ERROR,           // Error types
    JS_V2_TYPE_ARRAYBUFFER,     // ES6 ArrayBuffer
    JS_V2_TYPE_TYPEDARRAY,      // Typed arrays
    JS_V2_TYPE_PROXY,           // ES6 Proxy
    JS_V2_TYPE_REFLECT          // ES6 Reflect
} js_v2_type_t;

// ============================================================================
// VALUE STRUCTURE
// ============================================================================
typedef struct js_v2_value js_v2_value_t;
typedef struct js_v2_object js_v2_object_t;
typedef struct js_v2_array js_v2_array_t;
typedef struct js_v2_function js_v2_function_t;
typedef struct js_v2_promise js_v2_promise_t;

// Property descriptor (for getters/setters)
typedef struct {
    char key[64];
    js_v2_value_t* value;
    js_v2_value_t* getter;      // Getter function
    js_v2_value_t* setter;      // Setter function
    uint8_t writable;
    uint8_t enumerable;
    uint8_t configurable;
} js_v2_property_t;

// Object structure
struct js_v2_object {
    js_v2_property_t properties[32];
    int property_count;
    js_v2_object_t* prototype;   // Prototype chain
    char constructor_name[32];
    uint32_t flags;
};

// Array structure
struct js_v2_array {
    js_v2_value_t* elements[256];
    int length;
    uint32_t flags;
};

// Function structure
struct js_v2_function {
    char name[64];
    char params[16][32];
    int param_count;
    char* body;
    int body_len;
    js_v2_value_t* (*native_fn)(int argc, js_v2_value_t** args, void* engine);
    int is_native;
    int is_arrow;              // Arrow function
    int is_async;              // Async function
    int is_generator;          // Generator function
    void* closure;             // Closure scope
};

// Promise structure
struct js_v2_promise {
    int state;                 // 0=pending, 1=fulfilled, 2=rejected
    js_v2_value_t* result;
    js_v2_value_t* on_fulfilled[8];
    js_v2_value_t* on_rejected[8];
    int callback_count;
};

// Value union for different types
typedef union {
    int boolean;
    int64_t number;            // Extended for BigInt support
    char string[JS_V2_MAX_STRING_LEN];
    js_v2_object_t* object;
    js_v2_array_t* array;
    js_v2_function_t* function;
    js_v2_promise_t* promise;
    uint32_t symbol_id;
} js_v2_data_t;

// Main value structure
struct js_v2_value {
    js_v2_type_t type;
    js_v2_data_t data;
    int ref_count;
    uint32_t flags;
};

// ============================================================================
// SCOPE AND CLOSURE
// ============================================================================
typedef struct {
    char name[64];
    js_v2_value_t* value;
    int is_const;              // const vs let/var
    int is_let;                // let vs var (block scope)
} js_v2_variable_t;

typedef struct js_v2_scope js_v2_scope_t;

struct js_v2_scope {
    js_v2_variable_t variables[JS_V2_MAX_VARIABLES];
    int variable_count;
    js_v2_scope_t* parent;
    int scope_type;            // 0=global, 1=function, 2=block
};

// ============================================================================
// ENGINE STATE
// ============================================================================
typedef struct {
    // Value pool
    js_v2_value_t values[1024];
    int value_count;
    
    // Scope stack
    js_v2_scope_t scopes[JS_V2_MAX_SCOPES];
    int scope_count;
    js_v2_scope_t* current_scope;
    
    // Function registry
    js_v2_function_t functions[JS_V2_MAX_FUNCTIONS];
    int function_count;
    
    // Promise queue (for async)
    js_v2_promise_t promises[JS_V2_MAX_PROMISES];
    int promise_count;
    
    // Callback queue (for event loop)
    struct {
        js_v2_function_t* fn;
        js_v2_value_t* args[8];
        int argc;
        uint32_t scheduled_time;
    } callbacks[JS_V2_MAX_CALLBACKS];
    int callback_count;
    
    // Error state
    int has_error;
    char error_msg[256];
    js_v2_value_t* error_value;
    
    // Global objects
    js_v2_value_t* global_object;
    js_v2_value_t* console_object;
    js_v2_value_t* document_object;
    js_v2_value_t* window_object;
    js_v2_value_t* math_object;
    js_v2_value_t* json_object;
    js_v2_value_t* array_object;
    js_v2_value_t* object_object;
    js_v2_value_t* promise_object;
    
    // Symbol counter
    uint32_t next_symbol_id;
    
    // Browser integration
    void* browser_context;
    void (*log_callback)(const char* message);
    void (*dom_update_callback)(void* element, const char* property, js_v2_value_t* value);
    void (*dom_query_callback)(const char* selector, void* result);
    
} js_v2_engine_t;

// ============================================================================
// ENGINE API
// ============================================================================

// Initialization
void js_v2_init(js_v2_engine_t* engine);
void js_v2_destroy(js_v2_engine_t* engine);

// Execution
js_v2_value_t* js_v2_eval(js_v2_engine_t* engine, const char* code);
js_v2_value_t* js_v2_eval_module(js_v2_engine_t* engine, const char* code);

// Value creation
js_v2_value_t* js_v2_new_undefined(js_v2_engine_t* engine);
js_v2_value_t* js_v2_new_null(js_v2_engine_t* engine);
js_v2_value_t* js_v2_new_boolean(js_v2_engine_t* engine, int value);
js_v2_value_t* js_v2_new_number(js_v2_engine_t* engine, double value);
js_v2_value_t* js_v2_new_string(js_v2_engine_t* engine, const char* value);
js_v2_value_t* js_v2_new_object(js_v2_engine_t* engine);
js_v2_value_t* js_v2_new_array(js_v2_engine_t* engine);
js_v2_value_t* js_v2_new_function(js_v2_engine_t* engine, const char* name);
js_v2_value_t* js_v2_new_promise(js_v2_engine_t* engine);
js_v2_value_t* js_v2_new_symbol(js_v2_engine_t* engine, const char* description);
js_v2_value_t* js_v2_new_bigint(js_v2_engine_t* engine, int64_t value);
js_v2_value_t* js_v2_new_date(js_v2_engine_t* engine, int64_t timestamp);
js_v2_value_t* js_v2_new_error(js_v2_engine_t* engine, const char* message, const char* type);

// Value operations
void js_v2_value_ref(js_v2_value_t* value);
void js_v2_value_unref(js_v2_value_t* value);
js_v2_value_t* js_v2_to_string(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_to_number(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_to_boolean(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_to_primitive(js_v2_engine_t* engine, js_v2_value_t* value);

// Object operations
void js_v2_object_set(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key, js_v2_value_t* value);
js_v2_value_t* js_v2_object_get(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key);
js_v2_value_t* js_v2_object_get_prototype(js_v2_engine_t* engine, js_v2_value_t* obj);
void js_v2_object_set_prototype(js_v2_engine_t* engine, js_v2_value_t* obj, js_v2_value_t* proto);
int js_v2_object_has_property(js_v2_value_t* obj, const char* key);
js_v2_value_t** js_v2_object_keys(js_v2_engine_t* engine, js_v2_value_t* obj, int* count);
js_v2_value_t** js_v2_object_values(js_v2_engine_t* engine, js_v2_value_t* obj, int* count);
js_v2_value_t** js_v2_object_entries(js_v2_engine_t* engine, js_v2_value_t* obj, int* count);

// Array operations
void js_v2_array_push(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value);
js_v2_value_t* js_v2_array_pop(js_v2_engine_t* engine, js_v2_value_t* arr);
js_v2_value_t* js_v2_array_shift(js_v2_engine_t* engine, js_v2_value_t* arr);
void js_v2_array_unshift(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value);
int js_v2_array_length(js_v2_value_t* arr);
js_v2_value_t* js_v2_array_map(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn);
js_v2_value_t* js_v2_array_filter(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn);
js_v2_value_t* js_v2_array_reduce(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn, js_v2_value_t* initial);
js_v2_value_t* js_v2_array_find(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn);
int js_v2_array_find_index(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn);
js_v2_value_t* js_v2_array_includes(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value);
js_v2_value_t* js_v2_array_slice(js_v2_engine_t* engine, js_v2_value_t* arr, int start, int end);
js_v2_value_t* js_v2_array_splice(js_v2_engine_t* engine, js_v2_value_t* arr, int start, int delete_count);
js_v2_value_t* js_v2_array_concat(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* other);
js_v2_value_t* js_v2_array_join(js_v2_engine_t* engine, js_v2_value_t* arr, const char* separator);
js_v2_value_t* js_v2_array_sort(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* compare_fn);
void js_v2_array_fill(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value, int start, int end);
js_v2_value_t* js_v2_array_flat(js_v2_engine_t* engine, js_v2_value_t* arr, int depth);
js_v2_value_t* js_v2_array_flatMap(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn);

// Function operations
int js_v2_register_native(js_v2_engine_t* engine, const char* name, 
                          js_v2_value_t* (*fn)(int argc, js_v2_value_t** args, void* engine));
js_v2_value_t* js_v2_call(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val,
                           int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_call_method(js_v2_engine_t* engine, js_v2_value_t* obj, const char* method,
                                  int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_apply(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val,
                            js_v2_value_t* args_array);
js_v2_value_t* js_v2_bind(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val,
                           int argc, js_v2_value_t** args);

// Promise operations
js_v2_value_t* js_v2_promise_resolve(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_promise_reject(js_v2_engine_t* engine, js_v2_value_t* reason);
js_v2_value_t* js_v2_promise_all(js_v2_engine_t* engine, js_v2_value_t* promises);
js_v2_value_t* js_v2_promise_all_settled(js_v2_engine_t* engine, js_v2_value_t* promises);
js_v2_value_t* js_v2_promise_race(js_v2_engine_t* engine, js_v2_value_t* promises);
js_v2_value_t* js_v2_promise_any(js_v2_engine_t* engine, js_v2_value_t* promises);
void js_v2_promise_then(js_v2_engine_t* engine, js_v2_value_t* promise, 
                        js_v2_value_t* on_fulfilled, js_v2_value_t* on_rejected);
js_v2_value_t* js_v2_promise_catch(js_v2_engine_t* engine, js_v2_value_t* promise, 
                                   js_v2_value_t* on_rejected);
js_v2_value_t* js_v2_promise_finally(js_v2_engine_t* engine, js_v2_value_t* promise, 
                                     js_v2_value_t* on_finally);

// Async/await support
js_v2_value_t* js_v2_await(js_v2_engine_t* engine, js_v2_value_t* promise);
void js_v2_run_microtasks(js_v2_engine_t* engine);

// Module support
int js_v2_register_module(js_v2_engine_t* engine, const char* name, js_v2_value_t* module_exports);
js_v2_value_t* js_v2_import(js_v2_engine_t* engine, const char* module_name);

// Scope management
void js_v2_push_scope(js_v2_engine_t* engine, int scope_type);
void js_v2_pop_scope(js_v2_engine_t* engine);
js_v2_variable_t* js_v2_find_variable(js_v2_engine_t* engine, const char* name);
void js_v2_declare_variable(js_v2_engine_t* engine, const char* name, int is_const, int is_let);

// Global registration
void js_v2_set_global(js_v2_engine_t* engine, const char* name, js_v2_value_t* value);
js_v2_value_t* js_v2_get_global(js_v2_engine_t* engine, const char* name);

// Error handling
const char* js_v2_get_error(js_v2_engine_t* engine);
void js_v2_clear_error(js_v2_engine_t* engine);
void js_v2_throw_error(js_v2_engine_t* engine, const char* message, const char* type);

// ============================================================================
// BUILT-IN OBJECTS AND METHODS
// ============================================================================

// console object methods
js_v2_value_t* js_v2_console_log(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_error(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_warn(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_info(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_table(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_time(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_console_timeEnd(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// JSON methods
js_v2_value_t* js_v2_json_parse(js_v2_engine_t* engine, const char* json_string);
char* js_v2_json_stringify(js_v2_engine_t* engine, js_v2_value_t* value, int indent);

// Math methods
js_v2_value_t* js_v2_math_random(js_v2_engine_t* engine);
js_v2_value_t* js_v2_math_floor(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_math_ceil(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_math_round(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_math_abs(js_v2_engine_t* engine, js_v2_value_t* value);
js_v2_value_t* js_v2_math_min(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_math_max(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_math_pow(js_v2_engine_t* engine, js_v2_value_t* base, js_v2_value_t* exp);
js_v2_value_t* js_v2_math_sqrt(js_v2_engine_t* engine, js_v2_value_t* value);

// String methods
js_v2_value_t* js_v2_string_split(js_v2_engine_t* engine, js_v2_value_t* str, const char* separator);
js_v2_value_t* js_v2_string_slice(js_v2_engine_t* engine, js_v2_value_t* str, int start, int end);
js_v2_value_t* js_v2_string_substring(js_v2_engine_t* engine, js_v2_value_t* str, int start, int end);
js_v2_value_t* js_v2_string_substr(js_v2_engine_t* engine, js_v2_value_t* str, int start, int length);
js_v2_value_t* js_v2_string_toUpperCase(js_v2_engine_t* engine, js_v2_value_t* str);
js_v2_value_t* js_v2_string_toLowerCase(js_v2_engine_t* engine, js_v2_value_t* str);
js_v2_value_t* js_v2_string_trim(js_v2_engine_t* engine, js_v2_value_t* str);
js_v2_value_t* js_v2_string_trimStart(js_v2_engine_t* engine, js_v2_value_t* str);
js_v2_value_t* js_v2_string_trimEnd(js_v2_engine_t* engine, js_v2_value_t* str);
js_v2_value_t* js_v2_string_includes(js_v2_engine_t* engine, js_v2_value_t* str, const char* search);
js_v2_value_t* js_v2_string_startsWith(js_v2_engine_t* engine, js_v2_value_t* str, const char* search);
js_v2_value_t* js_v2_string_endsWith(js_v2_engine_t* engine, js_v2_value_t* str, const char* search);
js_v2_value_t* js_v2_string_repeat(js_v2_engine_t* engine, js_v2_value_t* str, int count);
js_v2_value_t* js_v2_string_replace(js_v2_engine_t* engine, js_v2_value_t* str, const char* search, const char* replace);
js_v2_value_t* js_v2_string_replaceAll(js_v2_engine_t* engine, js_v2_value_t* str, const char* search, const char* replace);
js_v2_value_t* js_v2_string_padStart(js_v2_engine_t* engine, js_v2_value_t* str, int length, const char* pad);
js_v2_value_t* js_v2_string_padEnd(js_v2_engine_t* engine, js_v2_value_t* str, int length, const char* pad);
js_v2_value_t* js_v2_string_match(js_v2_engine_t* engine, js_v2_value_t* str, const char* pattern);
js_v2_value_t* js_v2_string_matchAll(js_v2_engine_t* engine, js_v2_value_t* str, const char* pattern);

// DOM methods
js_v2_value_t* js_v2_document_getElementById(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_document_querySelector(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_document_querySelectorAll(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_document_createElement(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_document_createTextNode(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_element_getAttribute(js_v2_engine_t* engine, js_v2_value_t* element, const char* attr);
void js_v2_element_setAttribute(js_v2_engine_t* engine, js_v2_value_t* element, const char* attr, const char* value);
js_v2_value_t* js_v2_element_getElementsByClassName(js_v2_engine_t* engine, js_v2_value_t* element, const char* class_name);
js_v2_value_t* js_v2_element_getElementsByTagName(js_v2_engine_t* engine, js_v2_value_t* element, const char* tag);
void js_v2_element_appendChild(js_v2_engine_t* engine, js_v2_value_t* parent, js_v2_value_t* child);
void js_v2_element_removeChild(js_v2_engine_t* engine, js_v2_value_t* parent, js_v2_value_t* child);
void js_v2_element_addEventListener(js_v2_engine_t* engine, js_v2_value_t* element, const char* event, js_v2_value_t* handler);

// Window methods
js_v2_value_t* js_v2_window_setTimeout(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
js_v2_value_t* js_v2_window_setInterval(js_v2_engine_t* engine, int argc, js_v2_value_t** args);
void js_v2_window_clearTimeout(js_v2_engine_t* engine, int timeout_id);
void js_v2_window_clearInterval(js_v2_engine_t* engine, int interval_id);
js_v2_value_t* js_v2_window_fetch(js_v2_engine_t* engine, int argc, js_v2_value_t** args);

// Fetch API
typedef struct {
    int status;
    char status_text[32];
    js_v2_value_t* headers;
    js_v2_value_t* body;
    int ok;
    int redirected;
    char url[256];
} js_v2_response_t;

js_v2_value_t* js_v2_fetch(js_v2_engine_t* engine, const char* url, js_v2_value_t* options);
js_v2_value_t* js_v2_response_json(js_v2_engine_t* engine, js_v2_value_t* response);
js_v2_value_t* js_v2_response_text(js_v2_engine_t* engine, js_v2_value_t* response);

// Register all built-ins
void js_v2_register_builtins(js_v2_engine_t* engine);

#endif // JS_ENGINE_V2_H
