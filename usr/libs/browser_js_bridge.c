// usr/libs/browser_js_bridge.c - Bridge between JS Engine V2 and Browser DOM
// This connects the JavaScript engine to the actual browser DOM

#include "browser_bridge.h"
#include "js_engine_v2.h"
#include "../../core/memory.h"
#include "../../core/string.h"

// Simple atoi implementation for kernel mode
static int simple_atoi(const char* str) {
    if (!str) return 0;
    int result = 0;
    int negative = 0;
    if (*str == '-') {
        negative = 1;
        str++;
    }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return negative ? -result : result;
}

// Stub function for element type lookup
static element_type_t get_element_type(const char* tag_name) {
    if (!tag_name) return ELEM_UNKNOWN;
    // Simple tag name to element type mapping
    if (strcmp(tag_name, "div") == 0) return ELEM_DIV;
    if (strcmp(tag_name, "span") == 0) return ELEM_SPAN;
    if (strcmp(tag_name, "p") == 0) return ELEM_P;
    if (strcmp(tag_name, "a") == 0) return ELEM_A;
    if (strcmp(tag_name, "img") == 0) return ELEM_IMG;
    if (strcmp(tag_name, "br") == 0) return ELEM_BR;
    if (strcmp(tag_name, "h1") == 0) return ELEM_H1;
    if (strcmp(tag_name, "h2") == 0) return ELEM_H2;
    if (strcmp(tag_name, "h3") == 0) return ELEM_H3;
    if (strcmp(tag_name, "ul") == 0) return ELEM_UL;
    if (strcmp(tag_name, "ol") == 0) return ELEM_OL;
    if (strcmp(tag_name, "li") == 0) return ELEM_LI;
    if (strcmp(tag_name, "table") == 0) return ELEM_TABLE;
    if (strcmp(tag_name, "tr") == 0) return ELEM_TR;
    if (strcmp(tag_name, "td") == 0) return ELEM_TD;
    if (strcmp(tag_name, "form") == 0) return ELEM_FORM;
    if (strcmp(tag_name, "input") == 0) return ELEM_INPUT;
    if (strcmp(tag_name, "button") == 0) return ELEM_BUTTON;
    if (strcmp(tag_name, "script") == 0) return ELEM_SCRIPT;
    if (strcmp(tag_name, "style") == 0) return ELEM_STYLE;
    if (strcmp(tag_name, "link") == 0) return ELEM_LINK;
    if (strcmp(tag_name, "meta") == 0) return ELEM_META;
    if (strcmp(tag_name, "title") == 0) return ELEM_TITLE;
    if (strcmp(tag_name, "head") == 0) return ELEM_HEAD;
    if (strcmp(tag_name, "body") == 0) return ELEM_BODY;
    if (strcmp(tag_name, "html") == 0) return ELEM_HTML;
    return ELEM_UNKNOWN;
}

// Stub for creating DOM nodes - uses static pool
#define BRIDGE_MAX_NODES 64
static dom_node_t bridge_nodes[BRIDGE_MAX_NODES];
static int bridge_node_count = 0;

static dom_node_t* dom_create_node(dom_node_type_t type) {
    if (bridge_node_count >= BRIDGE_MAX_NODES) return NULL;
    dom_node_t* node = &bridge_nodes[bridge_node_count++];
    memset(node, 0, sizeof(dom_node_t));
    node->type = type;
    return node;
}

// Stub for inline style parser
static void parse_inline_style(const char* style_str, css_style_t* style) {
    if (!style_str || !style) return;
    // Minimal parsing - just clear the style for now
    memset(style, 0, sizeof(css_style_t));
}

// Stub for getting element style
static css_style_t get_element_style(element_type_t elem_type, dom_node_t* parent) {
    css_style_t style;
    memset(&style, 0, sizeof(css_style_t));
    style.display = 1;  // block by default
    style.fg_color = 0xFF000000;
    style.bg_color = 0xFFFFFFFF;
    style.font_size = 16;
    return style;
}

// ============================================================================
// INTERNAL STATE
// ============================================================================

static dom_node_t* document_root = NULL;
static char current_page_url[256] = {0};

// Set document root for DOM queries
void browser_set_document_root(void* doc_root) {
    document_root = (dom_node_t*)doc_root;
}

void browser_set_current_url(const char* url) {
    if (url) {
        strncpy(current_page_url, url, 255);
        current_page_url[255] = 0;
    }
}

// DOM element wrapper for JS
typedef struct {
    dom_node_t* node;
    int valid;
} js_dom_element_t;

#define MAX_JS_ELEMENTS 64
static js_dom_element_t js_elements[MAX_JS_ELEMENTS];
static int js_element_count = 0;

// Create a JS element wrapper
static js_dom_element_t* js_create_element_wrapper(dom_node_t* node) {
    if (!node) return NULL;
    
    for (int i = 0; i < MAX_JS_ELEMENTS; i++) {
        if (!js_elements[i].valid) {
            js_elements[i].node = node;
            js_elements[i].valid = 1;
            return &js_elements[i];
        }
    }
    return NULL;
}

// Find element by ID in DOM tree
static dom_node_t* dom_find_by_id(dom_node_t* root, const char* id) {
    if (!root || !id) return NULL;
    
    if (root->id[0] && strcmp(root->id, id) == 0) {
        return root;
    }
    
    // Check children
    dom_node_t* child = root->first_child;
    while (child) {
        dom_node_t* found = dom_find_by_id(child, id);
        if (found) return found;
        child = child->next_sibling;
    }
    
    return NULL;
}

// Find elements by class name
static int dom_find_by_class(dom_node_t* root, const char* class_name, 
                             dom_node_t** results, int max_results) {
    if (!root || !class_name || max_results <= 0) return 0;
    
    int count = 0;
    
    // Check this node
    if (root->class_name[0] && strstr(root->class_name, class_name)) {
        results[count++] = root;
        if (count >= max_results) return count;
    }
    
    // Check children
    dom_node_t* child = root->first_child;
    while (child && count < max_results) {
        count += dom_find_by_class(child, class_name, results + count, max_results - count);
        child = child->next_sibling;
    }
    
    return count;
}

// Find elements by tag name
static int dom_find_by_tag(dom_node_t* root, const char* tag_name,
                           dom_node_t** results, int max_results) {
    if (!root || !tag_name || max_results <= 0) return 0;
    
    int count = 0;
    
    // Check this node
    if (root->type == 1 && root->tag_name[0]) {
        const char* t1 = root->tag_name;
        const char* t2 = tag_name;
        while (*t1 && *t2) {
            char c1 = (*t1 >= 'A' && *t1 <= 'Z') ? *t1 + 32 : *t1;
            char c2 = (*t2 >= 'A' && *t2 <= 'Z') ? *t2 + 32 : *t2;
            if (c1 != c2) break;
            t1++; t2++;
        }
        if (!*t1 && !*t2) {
            results[count++] = root;
            if (count >= max_results) return count;
        }
    }
    
    // Check children
    dom_node_t* child = root->first_child;
    while (child && count < max_results) {
        count += dom_find_by_tag(child, tag_name, results + count, max_results - count);
        child = child->next_sibling;
    }
    
    return count;
}

// Simple CSS selector parser
static dom_node_t* dom_query_selector(dom_node_t* root, const char* selector) {
    if (!root || !selector) return NULL;
    
    // Skip whitespace
    while (*selector == ' ') selector++;
    
    if (selector[0] == '#') {
        // ID selector
        return dom_find_by_id(root, selector + 1);
    } 
    else if (selector[0] == '.') {
        // Class selector
        dom_node_t* results[1];
        if (dom_find_by_class(root, selector + 1, results, 1) > 0) {
            return results[0];
        }
    }
    else {
        // Tag selector
        dom_node_t* results[1];
        if (dom_find_by_tag(root, selector, results, 1) > 0) {
            return results[0];
        }
    }
    
    return NULL;
}

// ============================================================================
// JS ENGINE V2 DOM CALLBACKS
// ============================================================================

// Called when JS wants to query a DOM element
static void js_dom_query_callback(const char* selector, void* result) {
    dom_node_t** result_ptr = (dom_node_t**)result;
    *result_ptr = dom_query_selector(document_root, selector);
}

// Called when JS wants to update a DOM property
static void js_dom_update_callback(void* element, const char* property, js_v2_value_t* value) {
    dom_node_t* node = (dom_node_t*)element;
    if (!node || !property) return;
    
    if (strcmp(property, "innerHTML") == 0) {
        // Update inner HTML - would need re-parsing
        if (value->type == JS_V2_TYPE_STRING) {
            // For now, store as text content
            if (node->text_content) {
                // Free old content if dynamically allocated
            }
            // Note: Full innerHTML would require re-parsing
        }
    }
    else if (strcmp(property, "textContent") == 0 || strcmp(property, "innerText") == 0) {
        if (value->type == JS_V2_TYPE_STRING) {
            if (node->text_content) {
                strncpy(node->text_content, value->data.string, node->text_len);
            }
        }
    }
    else if (strcmp(property, "style") == 0) {
        if (value->type == JS_V2_TYPE_STRING) {
            parse_inline_style(value->data.string, &node->style);
        }
    }
    else if (strcmp(property, "className") == 0 || strcmp(property, "class") == 0) {
        if (value->type == JS_V2_TYPE_STRING) {
            strncpy(node->class_name, value->data.string, 63);
            node->class_name[63] = 0;
        }
    }
}

// ============================================================================
// DOM METHOD IMPLEMENTATIONS FOR JS ENGINE
// ============================================================================

// document.getElementById(id)
js_v2_value_t* js_v2_document_getElementById(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_null(engine);
    }
    
    dom_node_t* node = dom_find_by_id(document_root, args[0]->data.string);
    if (!node) {
        return js_v2_new_null(engine);
    }
    
    // Create an object representing this element
    js_v2_value_t* elem_obj = js_v2_new_object(engine);
    
    // Store the DOM node pointer in the object
    char ptr_str[32];
    snprintf(ptr_str, sizeof(ptr_str), "__dom_ptr__%p", (void*)node);
    js_v2_object_set(engine, elem_obj, "__ptr", js_v2_new_string(engine, ptr_str));
    
    // Set element properties
    if (node->id[0]) {
        js_v2_object_set(engine, elem_obj, "id", js_v2_new_string(engine, node->id));
    }
    if (node->class_name[0]) {
        js_v2_object_set(engine, elem_obj, "className", js_v2_new_string(engine, node->class_name));
    }
    if (node->tag_name[0]) {
        js_v2_object_set(engine, elem_obj, "tagName", js_v2_new_string(engine, node->tag_name));
    }
    if (node->href[0]) {
        js_v2_object_set(engine, elem_obj, "href", js_v2_new_string(engine, node->href));
    }
    if (node->src[0]) {
        js_v2_object_set(engine, elem_obj, "src", js_v2_new_string(engine, node->src));
    }
    if (node->text_content && node->text_len > 0) {
        char text_copy[256];
        int copy_len = node->text_len < 255 ? node->text_len : 255;
        memcpy(text_copy, node->text_content, copy_len);
        text_copy[copy_len] = 0;
        js_v2_object_set(engine, elem_obj, "textContent", js_v2_new_string(engine, text_copy));
    }
    
    return elem_obj;
}

// document.querySelector(selector)
js_v2_value_t* js_v2_document_querySelector(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_null(engine);
    }
    
    dom_node_t* node = dom_query_selector(document_root, args[0]->data.string);
    if (!node) {
        return js_v2_new_null(engine);
    }
    
    // Reuse getElementById's object creation
    js_v2_value_t* single_args[1] = { js_v2_new_string(engine, node->id[0] ? node->id : "") };
    if (node->id[0]) {
        return js_v2_document_getElementById(engine, 1, single_args);
    }
    
    // Create object directly for elements without ID
    js_v2_value_t* elem_obj = js_v2_new_object(engine);
    if (node->tag_name[0]) {
        js_v2_object_set(engine, elem_obj, "tagName", js_v2_new_string(engine, node->tag_name));
    }
    if (node->class_name[0]) {
        js_v2_object_set(engine, elem_obj, "className", js_v2_new_string(engine, node->class_name));
    }
    
    return elem_obj;
}

// document.querySelectorAll(selector)
js_v2_value_t* js_v2_document_querySelectorAll(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_array(engine);
    }
    
    const char* selector = args[0]->data.string;
    
    // Allocate results array on heap
    dom_node_t** results = (dom_node_t**)kmalloc(64 * sizeof(dom_node_t*));
    if (!results) return js_v2_new_array(engine);
    
    int count = 0;
    
    // Skip whitespace
    while (*selector == ' ') selector++;
    
    if (selector[0] == '.') {
        count = dom_find_by_class(document_root, selector + 1, results, 64);
    } else if (selector[0] != '#') {
        count = dom_find_by_tag(document_root, selector, results, 64);
    } else {
        // ID selector - only one result
        dom_node_t* single = dom_find_by_id(document_root, selector + 1);
        if (single) {
            results[0] = single;
            count = 1;
        }
    }
    
    js_v2_value_t* arr = js_v2_new_array(engine);
    for (int i = 0; i < count; i++) {
        js_v2_value_t* elem = js_v2_new_object(engine);
        if (results[i]->tag_name[0]) {
            js_v2_object_set(engine, elem, "tagName", js_v2_new_string(engine, results[i]->tag_name));
        }
        if (results[i]->id[0]) {
            js_v2_object_set(engine, elem, "id", js_v2_new_string(engine, results[i]->id));
        }
        js_v2_array_push(engine, arr, elem);
    }
    
    kfree(results);
    return arr;
}

// document.createElement(tagName)
js_v2_value_t* js_v2_document_createElement(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_null(engine);
    }
    
    const char* tag_name = args[0]->data.string;
    int elem_type = get_element_type(tag_name);
    
    dom_node_t* node = dom_create_node(1);  // DOM_ELEMENT
    if (!node) {
        return js_v2_new_null(engine);
    }
    
    node->elem_type = elem_type;
    strncpy(node->tag_name, tag_name, 31);
    node->tag_name[31] = 0;
    node->style = get_element_style(elem_type, NULL);
    
    // Create JS object for the new element
    js_v2_value_t* elem_obj = js_v2_new_object(engine);
    js_v2_object_set(engine, elem_obj, "tagName", js_v2_new_string(engine, tag_name));
    
    char ptr_str[32];
    snprintf(ptr_str, sizeof(ptr_str), "__dom_ptr__%p", (void*)node);
    js_v2_object_set(engine, elem_obj, "__ptr", js_v2_new_string(engine, ptr_str));
    
    return elem_obj;
}

// document.createTextNode(text)
js_v2_value_t* js_v2_document_createTextNode(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0] || args[0]->type != JS_V2_TYPE_STRING) {
        return js_v2_new_null(engine);
    }
    
    dom_node_t* node = dom_create_node(2);  // DOM_TEXT
    if (!node) {
        return js_v2_new_null(engine);
    }
    
    // Allocate and copy text content
    int text_len = strlen(args[0]->data.string);
    node->text_content = (char*)kmalloc(text_len + 1);
    if (node->text_content) {
        strcpy(node->text_content, args[0]->data.string);
        node->text_len = text_len;
    }
    
    js_v2_value_t* elem_obj = js_v2_new_object(engine);
    js_v2_object_set(engine, elem_obj, "nodeType", js_v2_new_number(engine, 3));  // TEXT_NODE
    js_v2_object_set(engine, elem_obj, "textContent", js_v2_new_string(engine, args[0]->data.string));
    
    return elem_obj;
}

// element.getAttribute(name)
js_v2_value_t* js_v2_element_getAttribute(js_v2_engine_t* engine, js_v2_value_t* element, const char* attr) {
    if (!element || element->type != JS_V2_TYPE_OBJECT || !attr) {
        return js_v2_new_null(engine);
    }
    
    // Get attribute from JS object
    return js_v2_object_get(engine, element, attr);
}

// element.setAttribute(name, value)
void js_v2_element_setAttribute(js_v2_engine_t* engine, js_v2_value_t* element, const char* attr, const char* value) {
    if (!element || element->type != JS_V2_TYPE_OBJECT || !attr || !value) {
        return;
    }
    
    js_v2_object_set(engine, element, attr, js_v2_new_string(engine, value));
    
    // TODO: Update actual DOM node if we have the pointer
}

// ============================================================================
// FETCH API IMPLEMENTATION
// ============================================================================

// window.fetch(url, options)
js_v2_value_t* js_v2_window_fetch(js_v2_engine_t* engine, int argc, js_v2_value_t** args) {
    if (argc < 1 || !args[0]) {
        js_v2_throw_error(engine, "fetch requires a URL argument", "TypeError");
        return js_v2_new_null(engine);
    }
    
    const char* url = NULL;
    
    // Handle string URL or Request object
    if (args[0]->type == JS_V2_TYPE_STRING) {
        url = args[0]->data.string;
    } else if (args[0]->type == JS_V2_TYPE_OBJECT) {
        js_v2_value_t* url_val = js_v2_object_get(engine, args[0], "url");
        if (url_val && url_val->type == JS_V2_TYPE_STRING) {
            url = url_val->data.string;
        }
    }
    
    if (!url) {
        js_v2_throw_error(engine, "Invalid URL", "TypeError");
        return js_v2_new_null(engine);
    }
    
    // Get options (method, headers, body)
    const char* method = "GET";
    const char* body = NULL;
    
    if (argc >= 2 && args[1] && args[1]->type == JS_V2_TYPE_OBJECT) {
        js_v2_value_t* method_val = js_v2_object_get(engine, args[1], "method");
        if (method_val && method_val->type == JS_V2_TYPE_STRING) {
            method = method_val->data.string;
        }
        
        js_v2_value_t* body_val = js_v2_object_get(engine, args[1], "body");
        if (body_val && body_val->type == JS_V2_TYPE_STRING) {
            body = body_val->data.string;
        }
    }
    
    // Create a promise for the fetch
    js_v2_value_t* promise = js_v2_new_promise(engine);
    
    // Allocate response buffer on heap to avoid stack overflow
    char* response_buffer = (char*)kmalloc(32768);  // 32KB response buffer
    if (!response_buffer) {
        js_v2_throw_error(engine, "Memory allocation failed", "Error");
        return promise;
    }
    
    // Perform HTTP request
    extern int http_get(const char* url, char* response, int response_size,
                        const char** headers, int header_count);
    
    int result = http_get(url, response_buffer, 32767, NULL, 0);
    
    if (result > 0) {
        // Parse response
        int status = 0;
        if (strncmp(response_buffer, "HTTP/1.", 7) == 0) {
            status = simple_atoi(response_buffer + 9);
        }
        
        // Find body
        char* body_start = strstr(response_buffer, "\r\n\r\n");
        char* response_body = body_start ? body_start + 4 : response_buffer;
        
        // Create response object
        js_v2_value_t* response_obj = js_v2_new_object(engine);
        js_v2_object_set(engine, response_obj, "status", js_v2_new_number(engine, status));
        js_v2_object_set(engine, response_obj, "ok", js_v2_new_boolean(engine, status >= 200 && status < 300));
        js_v2_object_set(engine, response_obj, "statusText", js_v2_new_string(engine, "OK"));
        js_v2_object_set(engine, response_obj, "url", js_v2_new_string(engine, url));
        
        // Store body for later access
        js_v2_value_t* body_obj = js_v2_new_string(engine, response_body);
        js_v2_object_set(engine, response_obj, "_body", body_obj);
        
        // Add text() method (as a function property)
        // Note: This is simplified - full implementation would add proper method
        
        // Resolve promise with response
        js_v2_promise_resolve(engine, response_obj);
    } else {
        // Reject promise
        js_v2_throw_error(engine, "Network request failed", "NetworkError");
    }
    
    kfree(response_buffer);
    return promise;
}

// ============================================================================
// INITIALIZE BROWSER-JS BRIDGE
// ============================================================================

void browser_js_bridge_init(js_v2_engine_t* engine) {
    // Clear element cache
    memset(js_elements, 0, sizeof(js_elements));
    js_element_count = 0;
    
    // Set up callbacks
    engine->dom_query_callback = js_dom_query_callback;
    engine->dom_update_callback = js_dom_update_callback;
    
    // Register document methods
    js_v2_register_native(engine, "document.getElementById", 
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_document_getElementById);
    js_v2_register_native(engine, "document.querySelector",
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_document_querySelector);
    js_v2_register_native(engine, "document.querySelectorAll",
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_document_querySelectorAll);
    js_v2_register_native(engine, "document.createElement",
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_document_createElement);
    js_v2_register_native(engine, "document.createTextNode",
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_document_createTextNode);
    
    // Register window.fetch
    js_v2_register_native(engine, "fetch",
        (js_v2_value_t* (*)(int, js_v2_value_t**, void*))js_v2_window_fetch);
}
