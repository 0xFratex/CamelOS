// usr/libs/browser_enhanced.c - Enhanced Browser Features
// External resource loading, enhanced JS execution, better caching

#include <types.h>
#include "browser_bridge.h"
#include "../../core/memory.h"
#include "../../core/string.h"

// Serial debug output (defined in core/serial.c).
extern void s_printf(const char* fmt, ...);

// ============================================================================
// MISSING STANDARD FUNCTION (not provided by CamelOS libc)
// ============================================================================
static inline int strncasecmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0) {
        unsigned char c1 = (unsigned char)*s1++;
        unsigned char c2 = (unsigned char)*s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (int)c1 - (int)c2;
        if (c1 == 0) return 0;
    }
    return 0;
}

// ============================================================================
// EXTERNAL RESOURCE MANAGER
// ============================================================================

#define MAX_EXTERNAL_RESOURCES 32
#define MAX_RESOURCE_SIZE 65536

typedef enum {
    RESOURCE_CSS,
    RESOURCE_JS,
    RESOURCE_IMAGE,
    RESOURCE_FONT
} resource_type_t;

typedef struct {
    char url[256];
    resource_type_t type;
    char* content;
    int content_len;
    int loaded;
    uint32_t timestamp;
} external_resource_t;

static external_resource_t resources[MAX_EXTERNAL_RESOURCES];
static int resource_count = 0;

// External function from http.c
extern int http_get(const char* url, char* response, int response_size,
                    const char** headers, int header_count);

// Forward declaration — resolve_url is defined below (line ~120).
static void resolve_url(const char* base_url, const char* relative_url, char* resolved, int max_len);

// Current URL for resolving relative URLs
static char browser_current_url[256] = {0};

// Set current URL
void browser_set_current_url_for_resources(const char* url) {
    if (url) {
        strncpy(browser_current_url, url, 255);
        browser_current_url[255] = 0;
    }
}

// Public wrapper around resolve_url() so the browser click handler can
// resolve relative hrefs (e.g. "/search?q=foo" or "page.html") against
// the current page URL before calling browser_navigate(). Without this,
// relative hrefs that don't start with "http://" and don't contain a dot
// are misclassified as search queries and redirected to DuckDuckGo.
void browser_resolve_url(const char* relative_url, char* resolved, int max_len) {
    resolve_url(browser_current_url, relative_url, resolved, max_len);
}

// Normalize URL path by resolving . and .. segments
static void normalize_path(char* path) {
    char* src = path;
    char* dst = path;
    char* segments[64];
    int seg_count = 0;
    
    // Preserve leading slash
    if (*src == '/') {
        *dst++ = '/';
        src++;
    }
    
    // Split into segments
    while (*src) {
        if (*src == '/') { src++; continue; }
        segments[seg_count] = dst;
        while (*src && *src != '/') *dst++ = *src++;
        *dst++ = '\0';
        seg_count++;
    }
    
    // Resolve . and .. segments
    int write = 0;
    for (int i = 0; i < seg_count; i++) {
        if (strcmp(segments[i], ".") == 0) continue;
        if (strcmp(segments[i], "..") == 0) { if (write > 0) write--; continue; }
        segments[write++] = segments[i];
    }
    
    // Rebuild path
    dst = path;
    if (path[0] == '/') dst++;
    for (int i = 0; i < write; i++) {
        if (i > 0) *dst++ = '/';
        char* s = segments[i];
        while (*s) *dst++ = *s++;
    }
    *dst = '\0';
}

// Resolve relative URL to absolute URL
static void resolve_url(const char* base_url, const char* relative_url, char* resolved, int max_len) {
    if (!relative_url || !relative_url[0]) {
        strncpy(resolved, base_url, max_len - 1);
        resolved[max_len - 1] = 0;
        return;
    }
    
    // Check if already absolute
    if (strncmp(relative_url, "http://", 7) == 0 || 
        strncmp(relative_url, "https://", 8) == 0) {
        strncpy(resolved, relative_url, max_len - 1);
        resolved[max_len - 1] = 0;
        return;
    }
    
    // Find protocol:// in base URL
    const char* proto_end = strstr(base_url, "://");
    if (!proto_end) {
        /* Base URL is missing or malformed — cannot resolve a relative URL
         * against it. Return the relative URL unchanged but properly
         * null-terminated, so the caller can detect the failure (e.g.
         * http_get will fail to parse it and return <= 0) instead of
         * silently feeding an empty hostname to dns_resolve(). The previous
         * code forgot to write the null terminator here, which could leak
         * stack/heap bytes into the resolved buffer. */
        strncpy(resolved, relative_url, max_len - 1);
        resolved[max_len - 1] = 0;
        return;
    }
    proto_end += 3;
    
    // Find host end (first / after protocol)
    const char* host_end = strchr(proto_end, '/');
    if (!host_end) host_end = base_url + strlen(base_url);
    
    if (relative_url[0] == '/') {
        if (relative_url[1] == '/') {
            // Protocol-relative URL (//host/path) — keep scheme + the rest.
            // Earlier code wrote the scheme (e.g. "https:") and then strncat'd
            // relative_url+2 directly, producing "https:www.example.com/..."
            // (missing the "//"). Fixed by using snprintf to build
            // "<scheme>://<rest>" explicitly.
            int scheme_len = (int)(proto_end - base_url) - 3;  /* "https" or "http" */
            if (scheme_len < 0) scheme_len = 0;
            snprintf(resolved, max_len, "%.*s://%s",
                     scheme_len, base_url, relative_url + 2);
        } else {
            // Absolute path (/path) — scheme://host + path
            int proto_host_len = (int)(host_end - base_url);
            snprintf(resolved, max_len, "%.*s%s",
                     proto_host_len, base_url, relative_url);
        }
    } else {
        // Relative path — resolve against the directory of the base URL.
        const char* last_slash = strrchr(proto_end, '/');
        if (last_slash && last_slash > proto_end) {
            int base_len = (int)(last_slash - base_url) + 1;
            snprintf(resolved, max_len, "%.*s%s",
                     base_len, base_url, relative_url);
        } else {
            snprintf(resolved, max_len, "%s/%s", base_url, relative_url);
        }
    }
    
    resolved[max_len - 1] = 0;
    
    // Normalize path to resolve . and .. segments
    // Find the start of the path (after protocol://host)
    const char* path_start = strstr(resolved, "://");
    if (path_start) {
        path_start += 3;
        // Skip past host to find the path
        while (*path_start && *path_start != '/') path_start++;
        if (*path_start == '/') {
            // Normalize just the path portion in-place
            normalize_path((char*)path_start);
        }
    }
}

// Find or create resource slot
static external_resource_t* get_resource_slot(const char* url) {
    // Check if already loaded
    for (int i = 0; i < resource_count; i++) {
        if (strcmp(resources[i].url, url) == 0) {
            return &resources[i];
        }
    }
    
    // Create new slot
    if (resource_count < MAX_EXTERNAL_RESOURCES) {
        external_resource_t* res = &resources[resource_count++];
        memset(res, 0, sizeof(external_resource_t));
        strncpy(res->url, url, 255);
        return res;
    }
    
    return NULL;
}

// ============================================================================
// RESOURCE LOADING
// ============================================================================

// Load external CSS file
char* browser_load_css(const char* url) {
    char absolute_url[256];
    resolve_url(browser_current_url, url, absolute_url, sizeof(absolute_url));
    
    external_resource_t* res = get_resource_slot(absolute_url);
    if (!res) return NULL;
    
    if (res->loaded && res->content) {
        return res->content;
    }
    
    // Allocate content buffer
    res->content = (char*)kmalloc(MAX_RESOURCE_SIZE);
    if (!res->content) {
        return NULL;
    }
    
    // Fetch the CSS file
    int result = http_get(absolute_url, res->content, MAX_RESOURCE_SIZE - 1, NULL, 0);
    if (result > 0) {
        res->content_len = result;
        res->type = RESOURCE_CSS;
        res->loaded = 1;
        // Note: timestamp would need timer function
        return res->content;
    }
    
    kfree(res->content);
    res->content = NULL;
    res->loaded = 0;
    return NULL;
}

// Load external JavaScript file
char* browser_load_js(const char* url) {
    char absolute_url[256];
    resolve_url(browser_current_url, url, absolute_url, sizeof(absolute_url));
    
    external_resource_t* res = get_resource_slot(absolute_url);
    if (!res) return NULL;
    
    if (res->loaded && res->content) {
        return res->content;
    }
    
    // Allocate content buffer
    res->content = (char*)kmalloc(MAX_RESOURCE_SIZE);
    if (!res->content) {
        return NULL;
    }
    
    // Fetch the JS file
    int result = http_get(absolute_url, res->content, MAX_RESOURCE_SIZE - 1, NULL, 0);
    if (result > 0) {
        res->content_len = result;
        res->type = RESOURCE_JS;
        res->loaded = 1;
        return res->content;
    }
    
    kfree(res->content);
    res->content = NULL;
    res->loaded = 0;
    return NULL;
}

// ============================================================================
// CSS PROCESSING
// ============================================================================

// Apply CSS rules to the current DOM document. Previously this was a stub
// that only skipped over the CSS without actually applying any rules,
// which is why <link rel=stylesheet> tags had no visible effect even after
// we wired up browser_process_link_tags. We now delegate to the DOM engine's
// dom_apply_css(), which parses selectors (tag / .class / #id) and merges
// the declared properties into each matching node's computed style.
#include "browser_dom.h"
void browser_apply_css_rules(const char* css_content) {
    if (!css_content || !css_content[0]) return;

    dom_document_t* doc = dom_get_document();
    if (!doc) return;

    dom_apply_css(doc, css_content);
}

// ============================================================================
// CACHE MANAGEMENT
// ============================================================================

void browser_clear_resource_cache(void) {
    for (int i = 0; i < resource_count; i++) {
        if (resources[i].content) {
            kfree(resources[i].content);
            resources[i].content = NULL;
        }
        resources[i].loaded = 0;
    }
    resource_count = 0;
}

int browser_get_cache_size(void) {
    int total = 0;
    for (int i = 0; i < resource_count; i++) {
        if (resources[i].content) {
            total += resources[i].content_len;
        }
    }
    return total;
}

int browser_get_cache_count(void) {
    int count = 0;
    for (int i = 0; i < resource_count; i++) {
        if (resources[i].loaded) count++;
    }
    return count;
}

// ============================================================================
// HTML PARSING HELPERS
// ============================================================================

// Extract href from attribute string
static const char* extract_attr_value(const char* attrs, const char* attr_name, char* value, int max_len) {
    const char* p = attrs;
    int name_len = strlen(attr_name);
    
    while (*p) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        
        // Check if this is the attribute we're looking for
        if (strncasecmp(p, attr_name, name_len) == 0 && 
            (p[name_len] == '=' || p[name_len] == ' ' || p[name_len] == '\t')) {
            p += name_len;
            
            // Skip whitespace
            while (*p == ' ' || *p == '\t') p++;
            
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                
                if (*p == '"' || *p == '\'') {
                    char quote = *p++;
                    int i = 0;
                    while (*p && *p != quote && i < max_len - 1) {
                        value[i++] = *p++;
                    }
                    value[i] = 0;
                    return value;
                } else {
                    int i = 0;
                    while (*p && *p != ' ' && *p != '>' && *p != '\t' && i < max_len - 1) {
                        value[i++] = *p++;
                    }
                    value[i] = 0;
                    return value;
                }
            }
        }
        
        // Skip to next attribute
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    
    value[0] = 0;
    return NULL;
}

// Process link tags in HTML (loads external CSS)
void browser_process_link_tags(const char* html) {
    if (!html) return;
    
    const char* p = html;
    while ((p = strstr(p, "<link")) != NULL) {
        p += 5;
        
        // Find end of tag
        const char* tag_end = strstr(p, ">");
        if (!tag_end) break;
        
        int tag_len = tag_end - p;
        if (tag_len > 512) tag_len = 512;
        
        char tag_content[513];
        strncpy(tag_content, p, tag_len);
        tag_content[tag_len] = 0;
        
        // Check if it's a stylesheet
        if (strstr(tag_content, "stylesheet") || 
            (strstr(tag_content, "rel=") && strstr(tag_content, ".css"))) {
            
            // Extract href
            char href[256];
            if (extract_attr_value(tag_content, "href", href, sizeof(href))) {
                // Load the CSS
                char* css_content = browser_load_css(href);
                if (css_content) {
                    // Apply CSS rules
                    browser_apply_css_rules(css_content);
                }
            }
        }
        
        p = tag_end + 1;
    }
}

// Process script tags in HTML. Concatenates inline AND fetched external
// scripts into the caller-provided buffer so the browser can execute them
// in source order. Previously, external scripts were fetched into the cache
// but the content was discarded — so `<script src="app.js"></script>` had
// no effect. Now we append the fetched JS body to the output buffer too.
void browser_process_script_tags(const char* html, char* inline_scripts, int max_inline_len) {
    if (!html) return;

    int inline_pos = 0;
    inline_scripts[0] = 0;

    const char* p = html;
    while ((p = strstr(p, "<script")) != NULL) {
        p += 7;

        // Find end of opening tag
        const char* tag_end = strstr(p, ">");
        if (!tag_end) break;

        int tag_len = tag_end - p;
        if (tag_len > 256) tag_len = 256;

        char tag_content[257];
        strncpy(tag_content, p, tag_len);
        tag_content[tag_len] = 0;

        // Check for src attribute (external script)
        char src[256];
        int is_external = extract_attr_value(tag_content, "src", src, sizeof(src)) != NULL;

        // Find the closing </script> tag
        tag_end++;
        const char* close_tag = strstr(tag_end, "</script>");

        if (is_external && src[0]) {
            // Fetch external JS and append it to the output buffer so the
            // browser's JS engine will execute it in source order.
            char* js_content = browser_load_js(src);
            if (js_content) {
                int js_len = strlen(js_content);
                if (inline_pos + js_len + 1 < max_inline_len - 1) {
                    memcpy(inline_scripts + inline_pos, js_content, js_len);
                    inline_pos += js_len;
                    inline_scripts[inline_pos++] = '\n';
                } else {
                    s_printf("[Browser] WARNING: external script %s (%d bytes) truncated — script buffer full\n",
                             src, js_len);
                }
            }
        } else if (close_tag) {
            // Inline script content
            int script_len = close_tag - tag_end;
            if (script_len > 0 && inline_pos + script_len < max_inline_len - 1) {
                strncpy(inline_scripts + inline_pos, tag_end, script_len);
                inline_pos += script_len;
                inline_scripts[inline_pos] = '\n';
                inline_pos++;
            }
        }

        if (close_tag) {
            p = close_tag + 9;
        } else {
            p = tag_end;
        }
    }

    inline_scripts[inline_pos] = 0;
}
