// usr/libs/js_engine_v2.c - Enhanced JavaScript Engine Implementation
// Version 2.0 - ES6+ features for modern web compatibility

#include "js_engine_v2.h"
#include "../../core/string.h"

// ============================================================================
// INLINE STRING FUNCTIONS (for CDL compilation without libc)
// ============================================================================
#undef strlen
#undef strcpy
#undef strncpy
#undef strcmp
#undef strncmp
#undef strcat
#undef strchr
#undef strstr
#undef memset
#undef memcpy
#undef memmove

static inline int _js_strlen(const char* s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static inline char* _js_strcpy(char* dest, const char* src) {
    char* d = dest;
    if (src) while ((*d++ = *src++));
    else *d = 0;
    return dest;
}

static inline char* _js_strncpy(char* dest, const char* src, unsigned long n) {
    char* d = dest;
    unsigned long i = 0;
    if (src) while (i < n && (*d++ = *src++)) i++;
    while (i++ < n) *d++ = 0;
    return dest;
}

static inline int _js_strcmp(const char* s1, const char* s2) {
    if (!s1) return s2 ? -1 : 0;
    if (!s2) return 1;
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static inline int _js_strncmp(const char* s1, const char* s2, unsigned long n) {
    if (!s1) return s2 ? -1 : 0;
    if (!s2) return 1;
    unsigned long i = 0;
    while (i < n && *s1 && *s1 == *s2) { s1++; s2++; i++; }
    return (i >= n) ? 0 : (*(unsigned char*)s1 - *(unsigned char*)s2);
}

static inline char* _js_strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    if (src) while ((*d++ = *src++));
    return dest;
}

static inline char* _js_strchr(const char* s, int c) {
    if (!s) return 0;
    while (*s && *s != (char)c) s++;
    return (*s == (char)c) ? (char*)s : 0;
}

static inline char* _js_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return (char*)haystack;
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
        haystack++;
    }
    return 0;
}

static inline void* _js_memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void* _js_memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static inline void* _js_memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

#define strlen _js_strlen
#define strcpy _js_strcpy
#define strncpy _js_strncpy
#define strcmp _js_strcmp
#define strncmp _js_strncmp
#define strcat _js_strcat
#define strchr _js_strchr
#define strstr _js_strstr
#define memset _js_memset
#define memcpy _js_memcpy
#define memmove _js_memmove

// Simple integer-only sprintf replacement for %lld and %d formats
static inline int _js_sprintf(char* buf, const char* fmt, ...) {
    (void)fmt;
    // Simple implementation for %lld format
    char* p = buf;
    const char* f = fmt;
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    
    while (*f) {
        if (*f == '%' && *(f+1) == 'l' && *(f+2) == 'l' && *(f+3) == 'd') {
            int64_t val = __builtin_va_arg(args, int64_t);
            char tmp[24];
            int i = 0;
            int neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            if (val == 0) tmp[i++] = '0';
            else while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
            if (neg) *p++ = '-';
            while (i > 0) *p++ = tmp[--i];
            f += 4;
        } else if (*f == '%' && *(f+1) == 'd') {
            int val = __builtin_va_arg(args, int);
            char tmp[12];
            int i = 0;
            int neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            if (val == 0) tmp[i++] = '0';
            else while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
            if (neg) *p++ = '-';
            while (i > 0) *p++ = tmp[--i];
            f += 2;
        } else {
            *p++ = *f++;
        }
    }
    *p = '\0';
    __builtin_va_end(args);
    return (int)(p - buf);
}
#define sprintf _js_sprintf

// ============================================================================
// VALUE POOL MANAGEMENT
// ============================================================================

static js_v2_value_t* alloc_value(js_v2_engine_t* engine) {
    if (engine->value_count >= 1024) {
        engine->has_error = 1;
        strcpy(engine->error_msg, "Out of value slots");
        return NULL;
    }
    
    js_v2_value_t* val = &engine->values[engine->value_count++];
    memset(val, 0, sizeof(js_v2_value_t));
    val->type = JS_V2_TYPE_UNDEFINED;
    val->ref_count = 1;
    return val;
}

// ============================================================================
// ENGINE INITIALIZATION
// ============================================================================

void js_v2_init(js_v2_engine_t* engine) {
    memset(engine, 0, sizeof(js_v2_engine_t));
    
    // Initialize global scope
    engine->current_scope = &engine->scopes[0];
    engine->scope_count = 1;
    engine->current_scope->scope_type = 0; // global
    
    // Create global objects
    engine->global_object = js_v2_new_object(engine);
    engine->console_object = js_v2_new_object(engine);
    engine->document_object = js_v2_new_object(engine);
    engine->window_object = js_v2_new_object(engine);
    engine->math_object = js_v2_new_object(engine);
    engine->json_object = js_v2_new_object(engine);
    engine->array_object = js_v2_new_object(engine);
    engine->object_object = js_v2_new_object(engine);
    engine->promise_object = js_v2_new_object(engine);
    
    // Set global references
    js_v2_object_set(engine, engine->global_object, "console", engine->console_object);
    js_v2_object_set(engine, engine->global_object, "document", engine->document_object);
    js_v2_object_set(engine, engine->global_object, "window", engine->window_object);
    js_v2_object_set(engine, engine->global_object, "Math", engine->math_object);
    js_v2_object_set(engine, engine->global_object, "JSON", engine->json_object);
    js_v2_object_set(engine, engine->global_object, "Array", engine->array_object);
    js_v2_object_set(engine, engine->global_object, "Object", engine->object_object);
    js_v2_object_set(engine, engine->global_object, "Promise", engine->promise_object);
    
    // Register built-in functions
    js_v2_register_builtins(engine);
}

void js_v2_destroy(js_v2_engine_t* engine) {
    // Clean up resources
    if (engine) {
        // Free any allocated memory
    }
}

// ============================================================================
// VALUE CREATION
// ============================================================================

js_v2_value_t* js_v2_new_undefined(js_v2_engine_t* engine) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) val->type = JS_V2_TYPE_UNDEFINED;
    return val;
}

js_v2_value_t* js_v2_new_null(js_v2_engine_t* engine) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) val->type = JS_V2_TYPE_NULL;
    return val;
}

js_v2_value_t* js_v2_new_boolean(js_v2_engine_t* engine, int value) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_BOOLEAN;
        val->data.boolean = value ? 1 : 0;
    }
    return val;
}

js_v2_value_t* js_v2_new_number(js_v2_engine_t* engine, int64_t value) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_NUMBER;
        val->data.number = (int64_t)value;
    }
    return val;
}

js_v2_value_t* js_v2_new_string(js_v2_engine_t* engine, const char* value) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_STRING;
        strncpy(val->data.string, value, JS_V2_MAX_STRING_LEN - 1);
        val->data.string[JS_V2_MAX_STRING_LEN - 1] = '\0';
    }
    return val;
}

js_v2_value_t* js_v2_new_object(js_v2_engine_t* engine) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_OBJECT;
        // Allocate on the stack instead of using static storage
        static js_v2_object_t static_objects[128];
        static int obj_idx = 0;
        if (obj_idx < 128) {
            val->data.object = &static_objects[obj_idx++];
            // Use kernel memset
            extern void* memset(void* ptr, int value, size_t num);
            memset(val->data.object, 0, sizeof(js_v2_object_t));
        }
    }
    return val;
}

js_v2_value_t* js_v2_new_array(js_v2_engine_t* engine) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_ARRAY;
        static js_v2_array_t static_arrays[64];
        static int arr_idx = 0;
        if (arr_idx < 64) {
            val->data.array = &static_arrays[arr_idx++];
            memset(val->data.array, 0, sizeof(js_v2_array_t));
        }
    }
    return val;
}

js_v2_value_t* js_v2_new_function(js_v2_engine_t* engine, const char* name) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_FUNCTION;
        static js_v2_function_t static_functions[64];
        static int fn_idx = 0;
        if (fn_idx < 64) {
            val->data.function = &static_functions[fn_idx++];
            memset(val->data.function, 0, sizeof(js_v2_function_t));
            if (name) {
                strncpy(val->data.function->name, name, 63);
            }
        }
    }
    return val;
}

js_v2_value_t* js_v2_new_promise(js_v2_engine_t* engine) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_PROMISE;
        static js_v2_promise_t static_promises[32];
        static int prom_idx = 0;
        if (prom_idx < 32) {
            val->data.promise = &static_promises[prom_idx++];
            memset(val->data.promise, 0, sizeof(js_v2_promise_t));
        }
    }
    return val;
}

js_v2_value_t* js_v2_new_symbol(js_v2_engine_t* engine, const char* description) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_SYMBOL;
        val->data.symbol_id = engine->next_symbol_id++;
        // Description could be stored in a separate table
    }
    return val;
}

js_v2_value_t* js_v2_new_bigint(js_v2_engine_t* engine, int64_t value) {
    js_v2_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_V2_TYPE_BIGINT;
        val->data.number = value;
    }
    return val;
}

js_v2_value_t* js_v2_new_date(js_v2_engine_t* engine, int64_t timestamp) {
    js_v2_value_t* val = js_v2_new_object(engine);
    if (val) {
        val->type = JS_V2_TYPE_DATE;
        val->data.number = timestamp;
    }
    return val;
}

js_v2_value_t* js_v2_new_error(js_v2_engine_t* engine, const char* message, const char* type) {
    js_v2_value_t* val = js_v2_new_object(engine);
    if (val) {
        val->type = JS_V2_TYPE_ERROR;
        js_v2_object_set(engine, val, "message", js_v2_new_string(engine, message));
        js_v2_object_set(engine, val, "name", js_v2_new_string(engine, type ? type : "Error"));
    }
    return val;
}

// ============================================================================
// VALUE OPERATIONS
// ============================================================================

void js_v2_value_ref(js_v2_value_t* value) {
    if (value) value->ref_count++;
}

void js_v2_value_unref(js_v2_value_t* value) {
    if (value && --value->ref_count <= 0) {
        value->type = JS_V2_TYPE_UNDEFINED;
    }
}

js_v2_value_t* js_v2_to_string(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value) return js_v2_new_string(engine, "undefined");
    
    char buf[64];
    
    switch (value->type) {
        case JS_V2_TYPE_UNDEFINED:
            return js_v2_new_string(engine, "undefined");
        case JS_V2_TYPE_NULL:
            return js_v2_new_string(engine, "null");
        case JS_V2_TYPE_BOOLEAN:
            return js_v2_new_string(engine, value->data.boolean ? "true" : "false");
        case JS_V2_TYPE_NUMBER:
            // Convert number to string
            sprintf(buf, "%lld", (long long)value->data.number);
            return js_v2_new_string(engine, buf);
        case JS_V2_TYPE_STRING:
            js_v2_value_ref(value);
            return value;
        case JS_V2_TYPE_OBJECT:
        case JS_V2_TYPE_ARRAY:
            return js_v2_new_string(engine, "[object Object]");
        case JS_V2_TYPE_FUNCTION:
            return js_v2_new_string(engine, "[Function]");
        default:
            return js_v2_new_string(engine, "undefined");
    }
}

js_v2_value_t* js_v2_to_number(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value) return js_v2_new_number(engine, 0);
    
    switch (value->type) {
        case JS_V2_TYPE_UNDEFINED:
            return js_v2_new_number(engine, 0);
        case JS_V2_TYPE_NULL:
            return js_v2_new_number(engine, 0);
        case JS_V2_TYPE_BOOLEAN:
            return js_v2_new_number(engine, value->data.boolean ? 1 : 0);
        case JS_V2_TYPE_NUMBER:
            js_v2_value_ref(value);
            return value;
        case JS_V2_TYPE_STRING: {
            // Parse string to number
            int num = 0;
            int neg = 0;
            const char* p = value->data.string;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            return js_v2_new_number(engine, neg ? -num : num);
        }
        default:
            return js_v2_new_number(engine, 0);
    }
}

js_v2_value_t* js_v2_to_boolean(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value) return js_v2_new_boolean(engine, 0);
    
    switch (value->type) {
        case JS_V2_TYPE_UNDEFINED:
        case JS_V2_TYPE_NULL:
            return js_v2_new_boolean(engine, 0);
        case JS_V2_TYPE_BOOLEAN:
            js_v2_value_ref(value);
            return value;
        case JS_V2_TYPE_NUMBER:
            return js_v2_new_boolean(engine, value->data.number != 0);
        case JS_V2_TYPE_STRING:
            return js_v2_new_boolean(engine, value->data.string[0] != '\0');
        case JS_V2_TYPE_OBJECT:
        case JS_V2_TYPE_ARRAY:
            return js_v2_new_boolean(engine, 1);
        default:
            return js_v2_new_boolean(engine, 0);
    }
}

js_v2_value_t* js_v2_to_primitive(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value) return js_v2_new_undefined(engine);
    
    switch (value->type) {
        case JS_V2_TYPE_OBJECT:
        case JS_V2_TYPE_ARRAY:
            // Try valueOf() then toString()
            return js_v2_to_string(engine, value);
        default:
            js_v2_value_ref(value);
            return value;
    }
}

// ============================================================================
// OBJECT OPERATIONS
// ============================================================================

void js_v2_object_set(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key, js_v2_value_t* value) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) return;
    
    js_v2_object_t* object = obj->data.object;
    
    // Check for existing property
    for (int i = 0; i < object->property_count; i++) {
        if (strcmp(object->properties[i].key, key) == 0) {
            object->properties[i].value = value;
            return;
        }
    }
    
    // Add new property
    if (object->property_count < 32) {
        strncpy(object->properties[object->property_count].key, key, 63);
        object->properties[object->property_count].value = value;
        object->properties[object->property_count].writable = 1;
        object->properties[object->property_count].enumerable = 1;
        object->properties[object->property_count].configurable = 1;
        object->property_count++;
    }
}

js_v2_value_t* js_v2_object_get(js_v2_engine_t* engine, js_v2_value_t* obj, const char* key) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) {
        return js_v2_new_undefined(engine);
    }
    
    js_v2_object_t* object = obj->data.object;
    
    for (int i = 0; i < object->property_count; i++) {
        if (strcmp(object->properties[i].key, key) == 0) {
            if (object->properties[i].getter) {
                // Call getter
                return js_v2_call(engine, object->properties[i].getter, obj, 0, NULL);
            }
            return object->properties[i].value;
        }
    }
    
    // Check prototype chain
    if (object->prototype) {
        js_v2_value_t proto_val;
        proto_val.type = JS_V2_TYPE_OBJECT;
        proto_val.data.object = object->prototype;
        return js_v2_object_get(engine, &proto_val, key);
    }
    
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_object_get_prototype(js_v2_engine_t* engine, js_v2_value_t* obj) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) {
        return js_v2_new_null(engine);
    }
    
    if (obj->data.object->prototype) {
        js_v2_value_t* proto = js_v2_new_object(engine);
        proto->data.object = obj->data.object->prototype;
        return proto;
    }
    
    return js_v2_new_null(engine);
}

void js_v2_object_set_prototype(js_v2_engine_t* engine, js_v2_value_t* obj, js_v2_value_t* proto) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) return;
    
    if (proto && proto->type == JS_V2_TYPE_OBJECT && proto->data.object) {
        obj->data.object->prototype = proto->data.object;
    } else {
        obj->data.object->prototype = NULL;
    }
}

int js_v2_object_has_property(js_v2_value_t* obj, const char* key) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) return 0;
    
    js_v2_object_t* object = obj->data.object;
    
    for (int i = 0; i < object->property_count; i++) {
        if (strcmp(object->properties[i].key, key) == 0) {
            return 1;
        }
    }
    
    return 0;
}

js_v2_value_t** js_v2_object_keys(js_v2_engine_t* engine, js_v2_value_t* obj, int* count) {
    if (!obj || obj->type != JS_V2_TYPE_OBJECT || !obj->data.object) {
        *count = 0;
        return NULL;
    }
    
    static js_v2_value_t* keys[32];
    js_v2_object_t* object = obj->data.object;
    int key_count = 0;
    
    for (int i = 0; i < object->property_count; i++) {
        if (object->properties[i].enumerable) {
            keys[key_count++] = js_v2_new_string(engine, object->properties[i].key);
        }
    }
    
    *count = key_count;
    return keys;
}

// ============================================================================
// ARRAY OPERATIONS
// ============================================================================

void js_v2_array_push(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) return;
    
    js_v2_array_t* array = arr->data.array;
    if (array->length < 256) {
        array->elements[array->length++] = value;
    }
}

js_v2_value_t* js_v2_array_pop(js_v2_engine_t* engine, js_v2_value_t* arr) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_undefined(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    if (array->length > 0) {
        return array->elements[--array->length];
    }
    
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_array_shift(js_v2_engine_t* engine, js_v2_value_t* arr) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_undefined(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    if (array->length > 0) {
        js_v2_value_t* first = array->elements[0];
        for (int i = 1; i < array->length; i++) {
            array->elements[i - 1] = array->elements[i];
        }
        array->length--;
        return first;
    }
    
    return js_v2_new_undefined(engine);
}

void js_v2_array_unshift(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) return;
    
    js_v2_array_t* array = arr->data.array;
    if (array->length < 256) {
        for (int i = array->length; i > 0; i--) {
            array->elements[i] = array->elements[i - 1];
        }
        array->elements[0] = value;
        array->length++;
    }
}

int js_v2_array_length(js_v2_value_t* arr) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) return 0;
    return arr->data.array->length;
}

js_v2_value_t* js_v2_array_map(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_array(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    js_v2_value_t* result = js_v2_new_array(engine);
    
    for (int i = 0; i < array->length; i++) {
        js_v2_value_t* args[3];
        args[0] = array->elements[i];
        args[1] = js_v2_new_number(engine, i);
        args[2] = arr;
        
        js_v2_value_t* mapped = js_v2_call(engine, (js_v2_value_t*)fn, NULL, 3, args);
        js_v2_array_push(engine, result, mapped);
    }
    
    return result;
}

js_v2_value_t* js_v2_array_filter(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_array(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    js_v2_value_t* result = js_v2_new_array(engine);
    
    for (int i = 0; i < array->length; i++) {
        js_v2_value_t* args[3];
        args[0] = array->elements[i];
        args[1] = js_v2_new_number(engine, i);
        args[2] = arr;
        
        js_v2_value_t* test = js_v2_call(engine, (js_v2_value_t*)fn, NULL, 3, args);
        js_v2_value_t* bool_val = js_v2_to_boolean(engine, test);
        
        if (bool_val->data.boolean) {
            js_v2_array_push(engine, result, array->elements[i]);
        }
    }
    
    return result;
}

js_v2_value_t* js_v2_array_reduce(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn, js_v2_value_t* initial) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return initial ? initial : js_v2_new_undefined(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    js_v2_value_t* accumulator = initial ? initial : 
                                  (array->length > 0 ? array->elements[0] : js_v2_new_undefined(engine));
    
    int start = initial ? 0 : 1;
    
    for (int i = start; i < array->length; i++) {
        js_v2_value_t* args[4];
        args[0] = accumulator;
        args[1] = array->elements[i];
        args[2] = js_v2_new_number(engine, i);
        args[3] = arr;
        
        accumulator = js_v2_call(engine, (js_v2_value_t*)fn, NULL, 4, args);
    }
    
    return accumulator;
}

js_v2_value_t* js_v2_array_find(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_undefined(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    
    for (int i = 0; i < array->length; i++) {
        js_v2_value_t* args[3];
        args[0] = array->elements[i];
        args[1] = js_v2_new_number(engine, i);
        args[2] = arr;
        
        js_v2_value_t* test = js_v2_call(engine, (js_v2_value_t*)fn, NULL, 3, args);
        js_v2_value_t* bool_val = js_v2_to_boolean(engine, test);
        
        if (bool_val->data.boolean) {
            return array->elements[i];
        }
    }
    
    return js_v2_new_undefined(engine);
}

int js_v2_array_find_index(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_function_t* fn) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) return -1;
    
    js_v2_array_t* array = arr->data.array;
    
    for (int i = 0; i < array->length; i++) {
        js_v2_value_t* args[3];
        args[0] = array->elements[i];
        args[1] = js_v2_new_number(engine, i);
        args[2] = arr;
        
        js_v2_value_t* test = js_v2_call(engine, (js_v2_value_t*)fn, NULL, 3, args);
        js_v2_value_t* bool_val = js_v2_to_boolean(engine, test);
        
        if (bool_val->data.boolean) {
            return i;
        }
    }
    
    return -1;
}

js_v2_value_t* js_v2_array_includes(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* value) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_boolean(engine, 0);
    }
    
    js_v2_array_t* array = arr->data.array;
    
    for (int i = 0; i < array->length; i++) {
        // Simple equality check
        if (array->elements[i] == value) {
            return js_v2_new_boolean(engine, 1);
        }
    }
    
    return js_v2_new_boolean(engine, 0);
}

js_v2_value_t* js_v2_array_slice(js_v2_engine_t* engine, js_v2_value_t* arr, int start, int end) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_array(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    js_v2_value_t* result = js_v2_new_array(engine);
    
    if (start < 0) start = array->length + start;
    if (start < 0) start = 0;
    if (end < 0) end = array->length + end;
    if (end > array->length) end = array->length;
    
    for (int i = start; i < end; i++) {
        js_v2_array_push(engine, result, array->elements[i]);
    }
    
    return result;
}

js_v2_value_t* js_v2_array_concat(js_v2_engine_t* engine, js_v2_value_t* arr, js_v2_value_t* other) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_array(engine);
    }
    
    js_v2_array_t* array = arr->data.array;
    js_v2_value_t* result = js_v2_new_array(engine);
    
    // Copy first array
    for (int i = 0; i < array->length; i++) {
        js_v2_array_push(engine, result, array->elements[i]);
    }
    
    // Copy second array if provided
    if (other && other->type == JS_V2_TYPE_ARRAY && other->data.array) {
        js_v2_array_t* other_array = other->data.array;
        for (int i = 0; i < other_array->length; i++) {
            js_v2_array_push(engine, result, other_array->elements[i]);
        }
    }
    
    return result;
}

js_v2_value_t* js_v2_array_join(js_v2_engine_t* engine, js_v2_value_t* arr, const char* separator) {
    if (!arr || arr->type != JS_V2_TYPE_ARRAY || !arr->data.array) {
        return js_v2_new_string(engine, "");
    }
    
    js_v2_array_t* array = arr->data.array;
    char result[1024] = {0};
    
    for (int i = 0; i < array->length; i++) {
        if (i > 0 && separator) {
            strcat(result, separator);
        } else if (i > 0) {
            strcat(result, ",");
        }
        
        js_v2_value_t* str = js_v2_to_string(engine, array->elements[i]);
        strcat(result, str->data.string);
    }
    
    return js_v2_new_string(engine, result);
}

// ============================================================================
// PROMISE OPERATIONS
// ============================================================================

js_v2_value_t* js_v2_promise_resolve(js_v2_engine_t* engine, js_v2_value_t* value) {
    js_v2_value_t* promise = js_v2_new_promise(engine);
    if (promise && promise->data.promise) {
        promise->data.promise->state = 1; // fulfilled
        promise->data.promise->result = value;
    }
    return promise;
}

js_v2_value_t* js_v2_promise_reject(js_v2_engine_t* engine, js_v2_value_t* reason) {
    js_v2_value_t* promise = js_v2_new_promise(engine);
    if (promise && promise->data.promise) {
        promise->data.promise->state = 2; // rejected
        promise->data.promise->result = reason;
    }
    return promise;
}

void js_v2_promise_then(js_v2_engine_t* engine, js_v2_value_t* promise,
                        js_v2_value_t* on_fulfilled, js_v2_value_t* on_rejected) {
    if (!promise || promise->type != JS_V2_TYPE_PROMISE || !promise->data.promise) return;
    
    js_v2_promise_t* p = promise->data.promise;
    
    if (p->callback_count < 8) {
        if (on_fulfilled) {
            p->on_fulfilled[p->callback_count] = on_fulfilled;
        }
        if (on_rejected) {
            p->on_rejected[p->callback_count] = on_rejected;
        }
        p->callback_count++;
    }
    
    // If already settled, execute immediately
    if (p->state == 1 && on_fulfilled) {
        js_v2_call(engine, on_fulfilled, NULL, 1, &p->result);
    } else if (p->state == 2 && on_rejected) {
        js_v2_call(engine, on_rejected, NULL, 1, &p->result);
    }
}

js_v2_value_t* js_v2_promise_all(js_v2_engine_t* engine, js_v2_value_t* promises) {
    if (!promises || promises->type != JS_V2_TYPE_ARRAY || !promises->data.array) {
        return js_v2_promise_resolve(engine, js_v2_new_array(engine));
    }
    
    js_v2_value_t* result_promise = js_v2_new_promise(engine);
    js_v2_value_t* results = js_v2_new_array(engine);
    
    js_v2_array_t* arr = promises->data.array;
    int resolved_count = 0;
    
    for (int i = 0; i < arr->length; i++) {
        js_v2_value_t* p = arr->elements[i];
        if (p && p->type == JS_V2_TYPE_PROMISE && p->data.promise) {
            if (p->data.promise->state == 1) {
                js_v2_array_push(engine, results, p->data.promise->result);
                resolved_count++;
            }
        } else {
            js_v2_array_push(engine, results, p);
            resolved_count++;
        }
    }
    
    if (resolved_count == arr->length) {
        result_promise->data.promise->state = 1;
        result_promise->data.promise->result = results;
    }
    
    return result_promise;
}

js_v2_value_t* js_v2_promise_race(js_v2_engine_t* engine, js_v2_value_t* promises) {
    if (!promises || promises->type != JS_V2_TYPE_ARRAY || !promises->data.array) {
        return js_v2_new_promise(engine);
    }
    
    js_v2_value_t* result_promise = js_v2_new_promise(engine);
    js_v2_array_t* arr = promises->data.array;
    
    for (int i = 0; i < arr->length; i++) {
        js_v2_value_t* p = arr->elements[i];
        if (p && p->type == JS_V2_TYPE_PROMISE && p->data.promise) {
            if (p->data.promise->state != 0) {
                result_promise->data.promise->state = p->data.promise->state;
                result_promise->data.promise->result = p->data.promise->result;
                return result_promise;
            }
        }
    }
    
    return result_promise;
}

// ============================================================================
// SCOPE MANAGEMENT
// ============================================================================

void js_v2_push_scope(js_v2_engine_t* engine, int scope_type) {
    if (engine->scope_count >= JS_V2_MAX_SCOPES) return;
    
    js_v2_scope_t* new_scope = &engine->scopes[engine->scope_count++];
    memset(new_scope, 0, sizeof(js_v2_scope_t));
    new_scope->parent = engine->current_scope;
    new_scope->scope_type = scope_type;
    engine->current_scope = new_scope;
}

void js_v2_pop_scope(js_v2_engine_t* engine) {
    if (engine->current_scope && engine->current_scope->parent) {
        engine->current_scope = engine->current_scope->parent;
    }
}

js_v2_variable_t* js_v2_find_variable(js_v2_engine_t* engine, const char* name) {
    js_v2_scope_t* scope = engine->current_scope;
    
    while (scope) {
        for (int i = 0; i < scope->variable_count; i++) {
            if (strcmp(scope->variables[i].name, name) == 0) {
                return &scope->variables[i];
            }
        }
        scope = scope->parent;
    }
    
    return NULL;
}

void js_v2_declare_variable(js_v2_engine_t* engine, const char* name, int is_const, int is_let) {
    if (!engine->current_scope) return;
    
    js_v2_scope_t* scope = engine->current_scope;
    if (scope->variable_count >= JS_V2_MAX_VARIABLES) return;
    
    js_v2_variable_t* var = &scope->variables[scope->variable_count++];
    strncpy(var->name, name, 63);
    var->value = js_v2_new_undefined(engine);
    var->is_const = is_const;
    var->is_let = is_let;
}

// ============================================================================
// GLOBAL REGISTRATION
// ============================================================================

void js_v2_set_global(js_v2_engine_t* engine, const char* name, js_v2_value_t* value) {
    js_v2_object_set(engine, engine->global_object, name, value);
}

js_v2_value_t* js_v2_get_global(js_v2_engine_t* engine, const char* name) {
    return js_v2_object_get(engine, engine->global_object, name);
}

// ============================================================================
// ERROR HANDLING
// ============================================================================

const char* js_v2_get_error(js_v2_engine_t* engine) {
    return engine->error_msg;
}

void js_v2_clear_error(js_v2_engine_t* engine) {
    engine->has_error = 0;
    engine->error_msg[0] = '\0';
}

void js_v2_throw_error(js_v2_engine_t* engine, const char* message, const char* type) {
    engine->has_error = 1;
    strncpy(engine->error_msg, message, 255);
    engine->error_value = js_v2_new_error(engine, message, type);
}

// ============================================================================
// BUILT-IN FUNCTIONS
// ============================================================================

js_v2_value_t* js_v2_console_log(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    for (int i = 0; i < argc; i++) {
        js_v2_value_t* str = js_v2_to_string(engine, args[i]);
        if (engine->log_callback) {
            engine->log_callback(str->data.string);
        }
    }
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_console_error(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    return js_v2_console_log(engine, argc, args);
}

js_v2_value_t* js_v2_console_warn(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    return js_v2_console_log(engine, argc, args);
}

js_v2_value_t* js_v2_console_info(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    return js_v2_console_log(engine, argc, args);
}

js_v2_value_t* js_v2_math_random(js_v2_engine_t* engine) {
    // Simple random number
    static uint32_t rand_state = 1;
    rand_state = rand_state * 1103515245 + 12345;
    int64_t random_value = (int64_t)((rand_state >> 16) & 0x7FFF);
    return js_v2_new_number(engine, random_value);
}

js_v2_value_t* js_v2_math_floor(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value || value->type != JS_V2_TYPE_NUMBER) {
        return js_v2_new_number(engine, 0);
    }
    return js_v2_new_number(engine, (int64_t)value->data.number);
}

js_v2_value_t* js_v2_math_ceil(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value || value->type != JS_V2_TYPE_NUMBER) {
        return js_v2_new_number(engine, 0);
    }
    int64_t n = (int64_t)value->data.number;
    if (value->data.number > n) n++;
    return js_v2_new_number(engine, n);
}

js_v2_value_t* js_v2_math_round(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value || value->type != JS_V2_TYPE_NUMBER) {
        return js_v2_new_number(engine, 0);
    }
    int64_t n = value->data.number; return js_v2_new_number(engine, (n >= 0) ? n + ((n & 1) ? 1 : 0) : n);
}

js_v2_value_t* js_v2_math_abs(js_v2_engine_t* engine, js_v2_value_t* value) {
    if (!value || value->type != JS_V2_TYPE_NUMBER) {
        return js_v2_new_number(engine, 0);
    }
    int64_t n = value->data.number;
    return js_v2_new_number(engine, n < 0 ? -n : n);
}

js_v2_value_t* js_v2_math_min(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc == 0) return js_v2_new_number(engine, 0);
    
    int64_t min = args[0]->type == JS_V2_TYPE_NUMBER ? args[0]->data.number : 0;
    
    for (int i = 1; i < argc; i++) {
        if (args[i]->type == JS_V2_TYPE_NUMBER && args[i]->data.number < min) {
            min = args[i]->data.number;
        }
    }
    
    return js_v2_new_number(engine, min);
}

js_v2_value_t* js_v2_math_max(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc == 0) return js_v2_new_number(engine, 0);
    
    int64_t max = args[0]->type == JS_V2_TYPE_NUMBER ? args[0]->data.number : 0;
    
    for (int i = 1; i < argc; i++) {
        if (args[i]->type == JS_V2_TYPE_NUMBER && args[i]->data.number > max) {
            max = args[i]->data.number;
        }
    }
    
    return js_v2_new_number(engine, max);
}

// ============================================================================
// JSON OPERATIONS
// ============================================================================

js_v2_value_t* js_v2_json_parse(js_v2_engine_t* engine, const char* json_string) {
    // Simple JSON parser
    // Skip whitespace
    while (*json_string == ' ' || *json_string == '\t' || 
           *json_string == '\n' || *json_string == '\r') {
        json_string++;
    }
    
    if (*json_string == '{') {
        return js_v2_new_object(engine);
    } else if (*json_string == '[') {
        return js_v2_new_array(engine);
    } else if (*json_string == '"') {
        // Parse string
        char str[256];
        int i = 0;
        json_string++;
        while (*json_string != '"' && *json_string != '\0' && i < 255) {
            str[i++] = *json_string++;
        }
        str[i] = '\0';
        return js_v2_new_string(engine, str);
    } else if (*json_string >= '0' && *json_string <= '9') {
        // Parse number
        int num = 0;
        while (*json_string >= '0' && *json_string <= '9') {
            num = num * 10 + (*json_string - '0');
            json_string++;
        }
        return js_v2_new_number(engine, num);
    } else if (strncmp(json_string, "true", 4) == 0) {
        return js_v2_new_boolean(engine, 1);
    } else if (strncmp(json_string, "false", 5) == 0) {
        return js_v2_new_boolean(engine, 0);
    } else if (strncmp(json_string, "null", 4) == 0) {
        return js_v2_new_null(engine);
    }
    
    return js_v2_new_undefined(engine);
}

char* js_v2_json_stringify(js_v2_engine_t* engine, js_v2_value_t* value, int indent) {
    static char buffer[4096];
    buffer[0] = '\0';
    
    if (!value) {
        strcpy(buffer, "null");
        return buffer;
    }
    
    switch (value->type) {
        case JS_V2_TYPE_UNDEFINED:
            strcpy(buffer, "undefined");
            break;
        case JS_V2_TYPE_NULL:
            strcpy(buffer, "null");
            break;
        case JS_V2_TYPE_BOOLEAN:
            strcpy(buffer, value->data.boolean ? "true" : "false");
            break;
        case JS_V2_TYPE_NUMBER: {
            sprintf(buffer, "%lld", (long long)value->data.number);
            break;
        }
        case JS_V2_TYPE_STRING:
            buffer[0] = '"';
            strcpy(buffer + 1, value->data.string);
            strcat(buffer, "\"");
            break;
        case JS_V2_TYPE_ARRAY: {
            js_v2_array_t* arr = value->data.array;
            strcpy(buffer, "[");
            for (int i = 0; i < arr->length; i++) {
                if (i > 0) strcat(buffer, ",");
                char* elem = js_v2_json_stringify(engine, arr->elements[i], indent + 2);
                strcat(buffer, elem);
            }
            strcat(buffer, "]");
            break;
        }
        case JS_V2_TYPE_OBJECT: {
            js_v2_object_t* obj = value->data.object;
            strcpy(buffer, "{");
            for (int i = 0; i < obj->property_count; i++) {
                if (i > 0) strcat(buffer, ",");
                strcat(buffer, "\"");
                strcat(buffer, obj->properties[i].key);
                strcat(buffer, "\":");
                char* val = js_v2_json_stringify(engine, obj->properties[i].value, indent + 2);
                strcat(buffer, val);
            }
            strcat(buffer, "}");
            break;
        }
        default:
            strcpy(buffer, "null");
    }
    
    return buffer;
}

// ============================================================================
// REGISTER BUILT-INS
// ============================================================================

void js_v2_register_builtins(js_v2_engine_t* engine) {
    // Console methods
    js_v2_object_set(engine, engine->console_object, "log", 
                     js_v2_new_function(engine, "log"));
    js_v2_object_set(engine, engine->console_object, "error", 
                     js_v2_new_function(engine, "error"));
    js_v2_object_set(engine, engine->console_object, "warn", 
                     js_v2_new_function(engine, "warn"));
    js_v2_object_set(engine, engine->console_object, "info", 
                     js_v2_new_function(engine, "info"));
    
    // Math methods
    js_v2_object_set(engine, engine->math_object, "random", 
                     js_v2_new_function(engine, "random"));
    js_v2_object_set(engine, engine->math_object, "floor", 
                     js_v2_new_function(engine, "floor"));
    js_v2_object_set(engine, engine->math_object, "ceil", 
                     js_v2_new_function(engine, "ceil"));
    js_v2_object_set(engine, engine->math_object, "round", 
                     js_v2_new_function(engine, "round"));
    js_v2_object_set(engine, engine->math_object, "abs", 
                     js_v2_new_function(engine, "abs"));
    js_v2_object_set(engine, engine->math_object, "min", 
                     js_v2_new_function(engine, "min"));
    js_v2_object_set(engine, engine->math_object, "max", 
                     js_v2_new_function(engine, "max"));
    js_v2_object_set(engine, engine->math_object, "PI", 
                     js_v2_new_number(engine, 3.14159265359));
    
    // JSON methods
    js_v2_object_set(engine, engine->json_object, "parse", 
                     js_v2_new_function(engine, "parse"));
    js_v2_object_set(engine, engine->json_object, "stringify", 
                     js_v2_new_function(engine, "stringify"));
    
    // Global functions
    js_v2_set_global(engine, "parseInt", js_v2_new_function(engine, "parseInt"));
    js_v2_set_global(engine, "parseFloat", js_v2_new_function(engine, "parseFloat"));
    js_v2_set_global(engine, "String", js_v2_new_function(engine, "String"));
    js_v2_set_global(engine, "Number", js_v2_new_function(engine, "Number"));
    js_v2_set_global(engine, "Boolean", js_v2_new_function(engine, "Boolean"));
    js_v2_set_global(engine, "Array", js_v2_new_function(engine, "Array"));
    js_v2_set_global(engine, "Object", js_v2_new_function(engine, "Object"));
    js_v2_set_global(engine, "Promise", js_v2_new_function(engine, "Promise"));
    js_v2_set_global(engine, "Symbol", js_v2_new_function(engine, "Symbol"));
}

// ============================================================================
// FUNCTION CALL
// ============================================================================

js_v2_value_t* js_v2_call(js_v2_engine_t* engine, js_v2_value_t* fn, js_v2_value_t* this_val,
                           int argc, js_v2_value_t** args) {
    if (!fn) return js_v2_new_undefined(engine);
    
    if (fn->type == JS_V2_TYPE_FUNCTION && fn->data.function) {
        js_v2_function_t* func = fn->data.function;
        
        if (func->is_native && func->native_fn) {
            return func->native_fn(engine, argc, args);
        }
        
        // For interpreted functions, would execute the function body
        // This would require a full interpreter implementation
    }
    
    return js_v2_new_undefined(engine);
}

// ============================================================================
// EVALUATION (Simplified)
// ============================================================================

js_v2_value_t* js_v2_eval(js_v2_engine_t* engine, const char* code) {
    if (!engine || !code) return js_v2_new_undefined(engine);
    
    // Simple JavaScript interpreter for document.write support
    // This handles basic patterns like: document.write("...")
    
    const char* p = code;
    
    // Skip whitespace
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    
    if (!*p) return js_v2_new_undefined(engine);
    
    // Check for common patterns
    // Pattern: document.write("...")
    if (strncmp(p, "document.write(", 15) == 0) {
        p += 15; // Skip "document.write("
        
        // Find the string argument
        while (*p == ' ' || *p == '\t') p++;
        
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            char buffer[4096];
            int buf_idx = 0;
            
            // Extract string content
            while (*p && *p != quote && buf_idx < 4095) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n': buffer[buf_idx++] = '\n'; break;
                        case 't': buffer[buf_idx++] = '\t'; break;
                        case 'r': buffer[buf_idx++] = '\r'; break;
                        case '\\': buffer[buf_idx++] = '\\'; break;
                        case '"': buffer[buf_idx++] = '"'; break;
                        case '\'': buffer[buf_idx++] = '\''; break;
                        default: buffer[buf_idx++] = *p; break;
                    }
                    p++;
                } else {
                    buffer[buf_idx++] = *p++;
                }
            }
            buffer[buf_idx] = 0;
            
            // Call document.write with the string
            js_v2_value_t* arg = js_v2_new_string(engine, buffer);
            js_v2_value_t* args[1] = { arg };
            
            // Find the document.write function and call it
            js_v2_value_t* doc = engine->document_object;
            if (doc && doc->type == JS_V2_TYPE_OBJECT && doc->data.object) {
                for (int i = 0; i < doc->data.object->property_count; i++) {
                    if (strcmp(doc->data.object->properties[i].key, "write") == 0) {
                        js_v2_value_t* write_fn = doc->data.object->properties[i].value;
                        if (write_fn && write_fn->type == JS_V2_TYPE_FUNCTION && 
                            write_fn->data.function && write_fn->data.function->native_fn) {
                            return write_fn->data.function->native_fn(engine, 1, args);
                        }
                    }
                }
            }
        }
    }
    
    // Pattern: document.writeln("...")
    if (strncmp(p, "document.writeln(", 17) == 0) {
        p += 17;
        
        while (*p == ' ' || *p == '\t') p++;
        
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            char buffer[4096];
            int buf_idx = 0;
            
            while (*p && *p != quote && buf_idx < 4095) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n': buffer[buf_idx++] = '\n'; break;
                        case 't': buffer[buf_idx++] = '\t'; break;
                        case 'r': buffer[buf_idx++] = '\r'; break;
                        case '\\': buffer[buf_idx++] = '\\'; break;
                        case '"': buffer[buf_idx++] = '"'; break;
                        case '\'': buffer[buf_idx++] = '\''; break;
                        default: buffer[buf_idx++] = *p; break;
                    }
                    p++;
                } else {
                    buffer[buf_idx++] = *p++;
                }
            }
            buffer[buf_idx] = 0;
            
            js_v2_value_t* arg = js_v2_new_string(engine, buffer);
            js_v2_value_t* args[1] = { arg };
            
            js_v2_value_t* doc = engine->document_object;
            if (doc && doc->type == JS_V2_TYPE_OBJECT && doc->data.object) {
                for (int i = 0; i < doc->data.object->property_count; i++) {
                    if (strcmp(doc->data.object->properties[i].key, "writeln") == 0) {
                        js_v2_value_t* writeln_fn = doc->data.object->properties[i].value;
                        if (writeln_fn && writeln_fn->type == JS_V2_TYPE_FUNCTION && 
                            writeln_fn->data.function && writeln_fn->data.function->native_fn) {
                            return writeln_fn->data.function->native_fn(engine, 1, args);
                        }
                    }
                }
            }
        }
    }
    
    // Pattern: console.log("...")
    if (strncmp(p, "console.log(", 12) == 0) {
        p += 12;
        
        while (*p == ' ' || *p == '\t') p++;
        
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            char buffer[4096];
            int buf_idx = 0;
            
            while (*p && *p != quote && buf_idx < 4095) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n': buffer[buf_idx++] = '\n'; break;
                        case 't': buffer[buf_idx++] = '\t'; break;
                        default: buffer[buf_idx++] = *p; break;
                    }
                    p++;
                } else {
                    buffer[buf_idx++] = *p++;
                }
            }
            buffer[buf_idx] = 0;
            
            if (engine->log_callback) {
                engine->log_callback(buffer);
            }
            return js_v2_new_undefined(engine);
        }
    }
    
    // Pattern: variable assignment var x = ...
    if (strncmp(p, "var ", 4) == 0 || strncmp(p, "let ", 4) == 0 || strncmp(p, "const ", 6) == 0) {
        // Skip to variable name
        // const char* var_start = p; // unused
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
        
        // Get variable name
        char var_name[64];
        int var_idx = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '=' && *p != ';' && var_idx < 63) {
            var_name[var_idx++] = *p++;
        }
        var_name[var_idx] = 0;
        
        // Skip to value
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            
            // Parse value
            js_v2_value_t* value = js_v2_new_undefined(engine);
            
            if (*p == '"' || *p == '\'') {
                // String value
                char quote = *p++;
                char buffer[1024];
                int buf_idx = 0;
                while (*p && *p != quote && buf_idx < 1023) {
                    if (*p == '\\' && *(p+1)) {
                        p++;
                        switch (*p) {
                            case 'n': buffer[buf_idx++] = '\n'; break;
                            case 't': buffer[buf_idx++] = '\t'; break;
                            default: buffer[buf_idx++] = *p++; continue;
                        }
                        p++;
                    } else {
                        buffer[buf_idx++] = *p++;
                    }
                }
                buffer[buf_idx] = 0;
                value = js_v2_new_string(engine, buffer);
            } else if (*p >= '0' && *p <= '9') {
                // Number value
                int num = 0;
                while (*p >= '0' && *p <= '9') {
                    num = num * 10 + (*p - '0');
                    p++;
                }
                value = js_v2_new_number(engine, num);
            } else if (strncmp(p, "true", 4) == 0) {
                value = js_v2_new_boolean(engine, 1);
            } else if (strncmp(p, "false", 5) == 0) {
                value = js_v2_new_boolean(engine, 0);
            } else if (strncmp(p, "null", 4) == 0) {
                value = js_v2_new_null(engine);
            }
            
            // Set the variable
            js_v2_declare_variable(engine, var_name, 0, 0);
            js_v2_variable_t* var = js_v2_find_variable(engine, var_name);
            if (var) var->value = value;
        }
        
        return js_v2_new_undefined(engine);
    }
    
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_eval_module(js_v2_engine_t* engine, const char* code) {
    // Module evaluation would handle import/export
    return js_v2_eval(engine, code);
}

// ============================================================================
// FETCH API IMPLEMENTATION
// ============================================================================

typedef struct {
    char url[512];
    char method[16];
    char headers[1024];
    char body[4096];
    int body_len;
    int mode;           // 0=same-origin, 1=no-cors, 2=cors
    int credentials;    // 0=omit, 1=same-origin, 2=include
    int cache;          // 0=default, 1=no-store, 2=reload, etc.
    int redirect;       // 0=follow, 1=error, 2=manual
} fetch_request_t;

typedef struct {
    int status;
    char status_text[32];
    char headers[2048];
    char body[16384];
    int body_len;
    char url[512];
    int ok;
    int redirected;
} fetch_response_t;

// Global fetch response cache for async handling
static fetch_response_t g_fetch_response;
__attribute__((unused)) static int g_fetch_pending = 0;

js_v2_value_t* js_v2_fetch(js_v2_engine_t* engine, const char* url, js_v2_value_t* options) {
    // Create a Promise for the fetch operation
    js_v2_value_t* promise = js_v2_new_promise(engine);
    
    // Initialize request
    fetch_request_t request;
    memset(&request, 0, sizeof(request));
    strncpy(request.url, url, sizeof(request.url) - 1);
    strcpy(request.method, "GET");
    
    // Parse options if provided
    if (options && options->type == JS_V2_TYPE_OBJECT && options->data.object) {
        js_v2_object_t* obj = options->data.object;
        
        for (int i = 0; i < obj->property_count; i++) {
            if (strcmp(obj->properties[i].key, "method") == 0) {
                js_v2_value_t* method = obj->properties[i].value;
                if (method && method->type == JS_V2_TYPE_STRING) {
                    strncpy(request.method, method->data.string, 15);
                }
            } else if (strcmp(obj->properties[i].key, "headers") == 0) {
                js_v2_value_t* headers = obj->properties[i].value;
                if (headers && headers->type == JS_V2_TYPE_STRING) {
                    strncpy(request.headers, headers->data.string, sizeof(request.headers) - 1);
                } else if (headers && headers->type == JS_V2_TYPE_OBJECT) {
                    // Convert headers object to string
                    // For simplicity, just use empty headers
                }
            } else if (strcmp(obj->properties[i].key, "body") == 0) {
                js_v2_value_t* body = obj->properties[i].value;
                if (body && body->type == JS_V2_TYPE_STRING) {
                    strncpy(request.body, body->data.string, sizeof(request.body) - 1);
                    request.body_len = strlen(request.body);
                }
            } else if (strcmp(obj->properties[i].key, "mode") == 0) {
                js_v2_value_t* mode = obj->properties[i].value;
                if (mode && mode->type == JS_V2_TYPE_STRING) {
                    if (strcmp(mode->data.string, "no-cors") == 0) request.mode = 1;
                    else if (strcmp(mode->data.string, "cors") == 0) request.mode = 2;
                }
            } else if (strcmp(obj->properties[i].key, "credentials") == 0) {
                js_v2_value_t* creds = obj->properties[i].value;
                if (creds && creds->type == JS_V2_TYPE_STRING) {
                    if (strcmp(creds->data.string, "include") == 0) request.credentials = 2;
                    else if (strcmp(creds->data.string, "omit") == 0) request.credentials = 0;
                }
            }
        }
    }
    
    // Make the HTTP request using the browser's network layer
    // This would integrate with the browser's http_fetch function
    // For now, simulate a successful response
    g_fetch_response.status = 200;
    strcpy(g_fetch_response.status_text, "OK");
    strcpy(g_fetch_response.body, "");
    g_fetch_response.body_len = 0;
    g_fetch_response.ok = 1;
    g_fetch_response.redirected = 0;
    strncpy(g_fetch_response.url, url, sizeof(g_fetch_response.url) - 1);
    
    // Resolve the promise with a Response object
    if (promise && promise->data.promise) {
        promise->data.promise->state = 1; // fulfilled
        
        // Create Response object
        js_v2_value_t* response = js_v2_new_object(engine);
        if (response) {
            js_v2_object_set(engine, response, "status", js_v2_new_number(engine, g_fetch_response.status));
            js_v2_object_set(engine, response, "statusText", js_v2_new_string(engine, g_fetch_response.status_text));
            js_v2_object_set(engine, response, "ok", js_v2_new_boolean(engine, g_fetch_response.ok));
            js_v2_object_set(engine, response, "redirected", js_v2_new_boolean(engine, g_fetch_response.redirected));
            js_v2_object_set(engine, response, "url", js_v2_new_string(engine, g_fetch_response.url));
            js_v2_object_set(engine, response, "type", js_v2_new_string(engine, "basic"));
            
            // Add methods: json(), text(), blob(), arrayBuffer()
            js_v2_object_set(engine, response, "json", js_v2_new_function(engine, "json"));
            js_v2_object_set(engine, response, "text", js_v2_new_function(engine, "text"));
            js_v2_object_set(engine, response, "blob", js_v2_new_function(engine, "blob"));
            js_v2_object_set(engine, response, "arrayBuffer", js_v2_new_function(engine, "arrayBuffer"));
            
            promise->data.promise->result = response;
        }
    }
    
    return promise;
}

js_v2_value_t* js_v2_response_json(js_v2_engine_t* engine, js_v2_value_t* response) {
    if (!response || response->type != JS_V2_TYPE_OBJECT) {
        return js_v2_new_null(engine);
    }
    
    // Parse response body as JSON
    return js_v2_json_parse(engine, g_fetch_response.body);
}

js_v2_value_t* js_v2_response_text(js_v2_engine_t* engine, js_v2_value_t* response) {
    // Return response body as string
    return js_v2_new_string(engine, g_fetch_response.body);
}

// ============================================================================
// WEB STORAGE API (localStorage, sessionStorage)
// ============================================================================

#define STORAGE_MAX_ITEMS 64
#define STORAGE_KEY_SIZE 64
#define STORAGE_VALUE_SIZE 4096

typedef struct {
    char key[STORAGE_KEY_SIZE];
    char value[STORAGE_VALUE_SIZE];
    int used;
} storage_item_t;

static storage_item_t g_local_storage[STORAGE_MAX_ITEMS];
static storage_item_t g_session_storage[STORAGE_MAX_ITEMS];

void js_v2_storage_init(void) {
    memset(g_local_storage, 0, sizeof(g_local_storage));
    memset(g_session_storage, 0, sizeof(g_session_storage));
}

js_v2_value_t* js_v2_storage_get_item(js_v2_engine_t* engine, storage_item_t* storage, const char* key) {
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (storage[i].used && strcmp(storage[i].key, key) == 0) {
            return js_v2_new_string(engine, storage[i].value);
        }
    }
    return js_v2_new_null(engine);
}

void js_v2_storage_set_item(storage_item_t* storage, const char* key, const char* value) {
    // Find existing or empty slot
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (storage[i].used && strcmp(storage[i].key, key) == 0) {
            strncpy(storage[i].value, value, STORAGE_VALUE_SIZE - 1);
            return;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (!storage[i].used) {
            storage[i].used = 1;
            strncpy(storage[i].key, key, STORAGE_KEY_SIZE - 1);
            strncpy(storage[i].value, value, STORAGE_VALUE_SIZE - 1);
            return;
        }
    }
}

void js_v2_storage_remove_item(storage_item_t* storage, const char* key) {
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (storage[i].used && strcmp(storage[i].key, key) == 0) {
            storage[i].used = 0;
            memset(storage[i].key, 0, STORAGE_KEY_SIZE);
            memset(storage[i].value, 0, STORAGE_VALUE_SIZE);
            return;
        }
    }
}

void js_v2_storage_clear(storage_item_t* storage) {
    memset(storage, 0, sizeof(storage_item_t) * STORAGE_MAX_ITEMS);
}

js_v2_value_t* js_v2_localStorage_getItem(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_null(engine);
    }
    return js_v2_storage_get_item(engine, g_local_storage, args[0]->data.string);
}

js_v2_value_t* js_v2_localStorage_setItem(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 2 || !args[0] || args[1] || 
        args[0]->type != JS_V2_TYPE_STRING || args[1]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_undefined(engine);
    }
    js_v2_storage_set_item(g_local_storage, args[0]->data.string, args[1]->data.string);
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_localStorage_removeItem(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_undefined(engine);
    }
    js_v2_storage_remove_item(g_local_storage, args[0]->data.string);
    return js_v2_new_undefined(engine);
}

js_v2_value_t* js_v2_localStorage_clear(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    js_v2_storage_clear(g_local_storage);
    return js_v2_new_undefined(engine);
}

// ============================================================================
// URL API
// ============================================================================

typedef struct {
    char href[512];
    char origin[256];
    char protocol[16];
    char host[256];
    char hostname[256];
    char port[8];
    char pathname[256];
    char search[256];
    char hash[128];
} url_parts_t;

void js_v2_parse_url(const char* url, url_parts_t* parts) {
    memset(parts, 0, sizeof(url_parts_t));
    
    const char* p = url;
    
    // Protocol
    const char* proto_end = strstr(p, "://");
    if (proto_end) {
        int proto_len = proto_end - p;
        if (proto_len < 16) {
            strncpy(parts->protocol, p, proto_len);
            strcpy(parts->protocol + proto_len, ":");
        }
        p = proto_end + 3;
    }
    
    // Host (until / or ? or # or end)
    const char* host_start = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    int host_len = p - host_start;
    if (host_len < 256) {
        strncpy(parts->host, host_start, host_len);
        strncpy(parts->hostname, host_start, host_len);
        
        // Check for port in host
        char* colon = strchr(parts->host, ':');
        if (colon) {
            *colon = '\0';
            strcpy(parts->hostname, parts->host);
            strcpy(parts->port, colon + 1);
        }
    }
    
    // Build origin
    if (parts->protocol[0] && parts->host[0]) {
        strcpy(parts->origin, parts->protocol);
        strcat(parts->origin, "//");
        strcat(parts->origin, parts->host);
    }
    
    // Path
    if (*p == '/') {
        const char* path_start = p;
        while (*p && *p != '?' && *p != '#') p++;
        int path_len = p - path_start;
        if (path_len < 256) {
            strncpy(parts->pathname, path_start, path_len);
        }
    } else {
        strcpy(parts->pathname, "/");
    }
    
    // Search
    if (*p == '?') {
        p++;
        const char* search_start = p;
        while (*p && *p != '#') p++;
        int search_len = p - search_start;
        if (search_len < 255) {
            parts->search[0] = '?';
            strncpy(parts->search + 1, search_start, search_len);
        }
    }
    
    // Hash
    if (*p == '#') {
        p++;
        strncpy(parts->hash, p, 127);
    }
    
    // Full href
    strncpy(parts->href, url, 511);
}

js_v2_value_t* js_v2_new_url(js_v2_engine_t* engine, const char* url_str) {
    url_parts_t parts;
    js_v2_parse_url(url_str, &parts);
    
    js_v2_value_t* url = js_v2_new_object(engine);
    if (!url) return js_v2_new_null(engine);
    
    js_v2_object_set(engine, url, "href", js_v2_new_string(engine, parts.href));
    js_v2_object_set(engine, url, "origin", js_v2_new_string(engine, parts.origin));
    js_v2_object_set(engine, url, "protocol", js_v2_new_string(engine, parts.protocol));
    js_v2_object_set(engine, url, "host", js_v2_new_string(engine, parts.host));
    js_v2_object_set(engine, url, "hostname", js_v2_new_string(engine, parts.hostname));
    js_v2_object_set(engine, url, "port", js_v2_new_string(engine, parts.port));
    js_v2_object_set(engine, url, "pathname", js_v2_new_string(engine, parts.pathname));
    js_v2_object_set(engine, url, "search", js_v2_new_string(engine, parts.search));
    js_v2_object_set(engine, url, "hash", js_v2_new_string(engine, parts.hash));
    
    // Methods
    js_v2_object_set(engine, url, "toString", js_v2_new_function(engine, "toString"));
    js_v2_object_set(engine, url, "toJSON", js_v2_new_function(engine, "toJSON"));
    
    return url;
}

// ============================================================================
// ENHANCED BUILT-INS REGISTRATION
// ============================================================================

void js_v2_register_modern_builtins(js_v2_engine_t* engine) {
    // Initialize storage
    js_v2_storage_init();
    
    // localStorage
    js_v2_value_t* localStorage = js_v2_new_object(engine);
    js_v2_object_set(engine, localStorage, "getItem", js_v2_new_function(engine, "getItem"));
    js_v2_object_set(engine, localStorage, "setItem", js_v2_new_function(engine, "setItem"));
    js_v2_object_set(engine, localStorage, "removeItem", js_v2_new_function(engine, "removeItem"));
    js_v2_object_set(engine, localStorage, "clear", js_v2_new_function(engine, "clear"));
    js_v2_object_set(engine, localStorage, "length", js_v2_new_number(engine, 0));
    js_v2_set_global(engine, "localStorage", localStorage);
    
    // sessionStorage
    js_v2_value_t* sessionStorage = js_v2_new_object(engine);
    js_v2_object_set(engine, sessionStorage, "getItem", js_v2_new_function(engine, "getItem"));
    js_v2_object_set(engine, sessionStorage, "setItem", js_v2_new_function(engine, "setItem"));
    js_v2_object_set(engine, sessionStorage, "removeItem", js_v2_new_function(engine, "removeItem"));
    js_v2_object_set(engine, sessionStorage, "clear", js_v2_new_function(engine, "clear"));
    js_v2_set_global(engine, "sessionStorage", sessionStorage);
    
    // fetch
    js_v2_set_global(engine, "fetch", js_v2_new_function(engine, "fetch"));
    
    // URL
    js_v2_value_t* URL = js_v2_new_object(engine);
    js_v2_object_set(engine, URL, "parse", js_v2_new_function(engine, "parse"));
    js_v2_object_set(engine, URL, "createObjectURL", js_v2_new_function(engine, "createObjectURL"));
    js_v2_object_set(engine, URL, "revokeObjectURL", js_v2_new_function(engine, "revokeObjectURL"));
    js_v2_set_global(engine, "URL", URL);
    
    // URLSearchParams
    js_v2_value_t* URLSearchParams = js_v2_new_function(engine, "URLSearchParams");
    js_v2_set_global(engine, "URLSearchParams", URLSearchParams);
    
    // FormData
    js_v2_value_t* FormData = js_v2_new_function(engine, "FormData");
    js_v2_set_global(engine, "FormData", FormData);
    
    // Headers
    js_v2_value_t* Headers = js_v2_new_function(engine, "Headers");
    js_v2_set_global(engine, "Headers", Headers);
    
    // Request
    js_v2_value_t* Request = js_v2_new_function(engine, "Request");
    js_v2_set_global(engine, "Request", Request);
    
    // Response
    js_v2_value_t* Response = js_v2_new_function(engine, "Response");
    js_v2_set_global(engine, "Response", Response);
    
    // AbortController
    js_v2_value_t* AbortController = js_v2_new_function(engine, "AbortController");
    js_v2_set_global(engine, "AbortController", AbortController);
    
    // AbortSignal
    js_v2_value_t* AbortSignal = js_v2_new_function(engine, "AbortSignal");
    js_v2_set_global(engine, "AbortSignal", AbortSignal);
    
    // TextEncoder / TextDecoder
    js_v2_value_t* TextEncoder = js_v2_new_function(engine, "TextEncoder");
    js_v2_value_t* TextDecoder = js_v2_new_function(engine, "TextDecoder");
    js_v2_set_global(engine, "TextEncoder", TextEncoder);
    js_v2_set_global(engine, "TextDecoder", TextDecoder);
    
    // atob / btoa
    js_v2_set_global(engine, "atob", js_v2_new_function(engine, "atob"));
    js_v2_set_global(engine, "btoa", js_v2_new_function(engine, "btoa"));
    
    // setTimeout / setInterval / clearTimeout / clearInterval
    js_v2_set_global(engine, "setTimeout", js_v2_new_function(engine, "setTimeout"));
    js_v2_set_global(engine, "setInterval", js_v2_new_function(engine, "setInterval"));
    js_v2_set_global(engine, "clearTimeout", js_v2_new_function(engine, "clearTimeout"));
    js_v2_set_global(engine, "clearInterval", js_v2_new_function(engine, "clearInterval"));
    
    // requestAnimationFrame / cancelAnimationFrame
    js_v2_set_global(engine, "requestAnimationFrame", js_v2_new_function(engine, "requestAnimationFrame"));
    js_v2_set_global(engine, "cancelAnimationFrame", js_v2_new_function(engine, "cancelAnimationFrame"));
    
    // queueMicrotask
    js_v2_set_global(engine, "queueMicrotask", js_v2_new_function(engine, "queueMicrotask"));
    
    // performance
    js_v2_value_t* performance = js_v2_new_object(engine);
    js_v2_object_set(engine, performance, "now", js_v2_new_function(engine, "now"));
    js_v2_object_set(engine, performance, "timeOrigin", js_v2_new_number(engine, 0));
    js_v2_set_global(engine, "performance", performance);
}

// ============================================================================
// ADDITIONAL HELPER FUNCTIONS FOR BROWSER BRIDGE
// ============================================================================
