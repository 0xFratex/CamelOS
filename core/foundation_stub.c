// core/foundation_stub.c - Foundation Framework Stubs Implementation
// Minimal Foundation classes for macOS app compatibility on CamelOS
// Bridges ObjC Foundation calls to CamelOS kernel services

#include "foundation_stub.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../hal/drivers/serial.h"

typedef unsigned int uintptr_t;

// Kernel API bridge
static void* g_kernel_api_ptr = 0;

// --- Class References ---
Class NSString_class = 0;
Class NSNumber_class = 0;
Class NSArray_class = 0;
Class NSDictionary_class = 0;
Class NSBundle_class = 0;

// Forward declarations for class method tables
static void register_NSString_methods(Class cls);
static void register_NSNumber_methods(Class cls);
static void register_NSArray_methods(Class cls);
static void register_NSDictionary_methods(Class cls);
static void register_NSBundle_methods(Class cls);

// ============================================================================
// NSObject Implementation
// ============================================================================

// Simple reference counting (stored in a parallel array for simplicity)
#define MAX_TRACKED_OBJECTS 4096
static uint32_t g_object_refcounts[MAX_TRACKED_OBJECTS];
static void* g_object_ptrs[MAX_TRACKED_OBJECTS];
static int g_object_tracking_count = 0;

int find_object_slot(id obj) {
    for (int i = 0; i < g_object_tracking_count; i++) {
        if (g_object_ptrs[i] == obj) return i;
    }
    return -1;
}

int track_object(id obj) {
    int slot = find_object_slot(obj);
    if (slot >= 0) return slot;
    if (g_object_tracking_count >= MAX_TRACKED_OBJECTS) return -1;
    slot = g_object_tracking_count++;
    g_object_ptrs[slot] = obj;
    g_object_refcounts[slot] = 1;
    return slot;
}

id NSObject_alloc(Class cls, SEL cmd) {
    (void)cmd;
    return class_createInstance(cls, 0);
}

id NSObject_init(id self, SEL cmd) {
    (void)cmd;
    track_object(self);
    return self;
}

void NSObject_dealloc(id self, SEL cmd) {
    (void)cmd;
    int slot = find_object_slot(self);
    if (slot >= 0) {
        g_object_ptrs[slot] = 0;
        g_object_refcounts[slot] = 0;
    }
    object_dispose(self);
}

id NSObject_retain(id self, SEL cmd) {
    (void)cmd;
    int slot = find_object_slot(self);
    if (slot >= 0) g_object_refcounts[slot]++;
    return self;
}

id NSObject_release(id self, SEL cmd) {
    (void)cmd;
    int slot = find_object_slot(self);
    if (slot >= 0) {
        g_object_refcounts[slot]--;
        if (g_object_refcounts[slot] == 0) {
            NSObject_dealloc(self, 0);
        }
    }
    return 0;
}

id NSObject_autorelease(id self, SEL cmd) {
    (void)cmd;
    return self;  // Simplified - no autorelease pool
}

uint32_t NSObject_retainCount(id self, SEL cmd) {
    (void)cmd;
    int slot = find_object_slot(self);
    return (slot >= 0) ? g_object_refcounts[slot] : 0;
}

id NSObject_self(id self, SEL cmd) {
    (void)cmd;
    return self;
}

Class NSObject_class(id self, SEL cmd) {
    (void)cmd;
    return object_getClass(self);
}

BOOL NSObject_isEqual(id self, SEL cmd, id other) {
    (void)cmd;
    return self == other;
}

uint32_t NSObject_hash(id self, SEL cmd) {
    (void)cmd;
    return (uint32_t)(uintptr_t)self;
}

id NSObject_description(id self, SEL cmd) {
    (void)cmd;
    // Return a simple description
    return NSString_stringWithCString("<NSObject>");
}

BOOL NSObject_respondsToSelector(id self, SEL cmd, SEL selector) {
    (void)cmd;
    if (!self || !selector) return NO;
    Class cls = object_getClass(self);
    Method m = class_getInstanceMethod(cls, selector);
    return m ? YES : NO;
}

// ============================================================================
// NSString Implementation
// ============================================================================

id NSString_alloc(void) {
    return NSObject_alloc(NSString_class, 0);
}

id NSString_initWithCString(id self, SEL cmd, const char* cstr) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    if (!str) return 0;
    
    uint32_t len = 0;
    const char* p = cstr;
    if (p) while (*p++) len++;
    
    str->capacity = len + 16;
    str->cstr = (char*)kmalloc(str->capacity);
    if (str->cstr && cstr) {
        memcpy(str->cstr, cstr, len + 1);
    } else if (str->cstr) {
        str->cstr[0] = 0;
    }
    str->length = len;
    
    return self;
}

id NSString_initWithFormat(id self, SEL cmd, const char* fmt, ...) {
    (void)cmd;
    // Simplified - just copy the format string
    return NSString_initWithCString(self, 0, fmt);
}

id NSString_stringWithCString(const char* cstr) {
    id str = NSString_alloc();
    str = NSObject_init(str, 0);
    return NSString_initWithCString(str, 0, cstr);
}

uint32_t NSString_length(id self, SEL cmd) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    return str ? str->length : 0;
}

const char* NSString_UTF8String(id self, SEL cmd) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    return str ? str->cstr : "";
}

CFComparisonResult NSString_compare(id self, SEL cmd, id other) {
    (void)cmd;
    const char* s1 = NSString_UTF8String(self, 0);
    const char* s2 = NSString_UTF8String(other, 0);
    int result = strcmp(s1, s2);
    if (result < 0) return kCFCompareLessThan;
    if (result > 0) return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}

BOOL NSString_isEqualToString(id self, SEL cmd, id other) {
    (void)cmd;
    return NSString_compare(self, 0, other) == kCFCompareEqualTo;
}

id NSString_stringByAppendingString(id self, SEL cmd, id other) {
    (void)cmd;
    const char* s1 = NSString_UTF8String(self, 0);
    const char* s2 = NSString_UTF8String(other, 0);
    uint32_t len1 = 0, len2 = 0;
    const char* p = s1; while (*p++) len1++;
    p = s2; while (*p++) len2++;
    
    char* combined = (char*)kmalloc(len1 + len2 + 1);
    if (!combined) return self;
    memcpy(combined, s1, len1);
    memcpy(combined + len1, s2, len2 + 1);
    
    id result = NSString_stringWithCString(combined);
    kfree(combined);
    return result;
}

static void register_NSString_methods(Class cls) {
    SEL sel;
    
    sel = sel_registerName("initWithCString:");
    class_addMethod(cls, sel, (void*)NSString_initWithCString, "@@:*");
    
    sel = sel_registerName("initWithFormat:");
    class_addMethod(cls, sel, (void*)NSString_initWithFormat, "@@:*");
    
    sel = sel_registerName("length");
    class_addMethod(cls, sel, (void*)NSString_length, "I@:");
    
    sel = sel_registerName("UTF8String");
    class_addMethod(cls, sel, (void*)NSString_UTF8String, "*@:");
    
    sel = sel_registerName("compare:");
    class_addMethod(cls, sel, (void*)NSString_compare, "i@:@");
    
    sel = sel_registerName("isEqualToString:");
    class_addMethod(cls, sel, (void*)NSString_isEqualToString, "B@:@");
    
    sel = sel_registerName("stringByAppendingString:");
    class_addMethod(cls, sel, (void*)NSString_stringByAppendingString, "@@:@");
}

// ============================================================================
// NSNumber Implementation
// ============================================================================

id NSNumber_numberWithInt(int value) {
    CamelOSNumber* num = (CamelOSNumber*)class_createInstance(NSNumber_class, 0);
    if (num) {
        num->value.int_val = value;
        num->type = 'i';
        track_object((id)num);
    }
    return (id)num;
}

id NSNumber_numberWithFloat(float value) {
    CamelOSNumber* num = (CamelOSNumber*)class_createInstance(NSNumber_class, 0);
    if (num) {
        num->value.float_val = value;
        num->type = 'f';
        track_object((id)num);
    }
    return (id)num;
}

id NSNumber_numberWithBool(BOOL value) {
    CamelOSNumber* num = (CamelOSNumber*)class_createInstance(NSNumber_class, 0);
    if (num) {
        num->value.bool_val = value;
        num->type = 'b';
        track_object((id)num);
    }
    return (id)num;
}

static void register_NSNumber_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("intValue");
    class_addMethod(cls, sel, (void*)0, "i@:");  // Simplified
}

// ============================================================================
// NSArray Implementation
// ============================================================================

id NSArray_arrayWithObject(id obj) {
    CamelOSArray* arr = (CamelOSArray*)class_createInstance(NSArray_class, 0);
    if (arr) {
        arr->capacity = 8;
        arr->objects = (id*)kmalloc(arr->capacity * sizeof(id));
        if (arr->objects && obj) {
            arr->objects[0] = obj;
            arr->count = 1;
        }
        track_object((id)arr);
    }
    return (id)arr;
}

uint32_t NSArray_count(id self, SEL cmd) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    return arr ? arr->count : 0;
}

id NSArray_objectAtIndex(id self, SEL cmd, uint32_t index) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || index >= arr->count) return 0;
    return arr->objects[index];
}

static void register_NSArray_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("count");
    class_addMethod(cls, sel, (void*)NSArray_count, "I@:");
    sel = sel_registerName("objectAtIndex:");
    class_addMethod(cls, sel, (void*)NSArray_objectAtIndex, "@@:I");
}

// ============================================================================
// NSDictionary Implementation
// ============================================================================

id NSDictionary_dictionaryWithObjectForKey(id obj, id key) {
    CamelOSDictionary* dict = (CamelOSDictionary*)class_createInstance(NSDictionary_class, 0);
    if (dict) {
        dict->capacity = 8;
        dict->keys = (id*)kmalloc(dict->capacity * sizeof(id));
        dict->values = (id*)kmalloc(dict->capacity * sizeof(id));
        if (dict->keys && dict->values && obj && key) {
            dict->keys[0] = key;
            dict->values[0] = obj;
            dict->count = 1;
        }
        track_object((id)dict);
    }
    return (id)dict;
}

uint32_t NSDictionary_count(id self, SEL cmd) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    return dict ? dict->count : 0;
}

id NSDictionary_objectForKey(id self, SEL cmd, id key) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    if (!dict || !key) return 0;
    
    const char* key_str = NSString_UTF8String(key, 0);
    for (uint32_t i = 0; i < dict->count; i++) {
        const char* k = NSString_UTF8String(dict->keys[i], 0);
        if (strcmp(key_str, k) == 0) return dict->values[i];
    }
    return 0;
}

static void register_NSDictionary_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("count");
    class_addMethod(cls, sel, (void*)NSDictionary_count, "I@:");
    sel = sel_registerName("objectForKey:");
    class_addMethod(cls, sel, (void*)NSDictionary_objectForKey, "@@:@");
}

// ============================================================================
// NSBundle Implementation
// ============================================================================

id NSBundle_mainBundle(void) {
    return NSBundle_bundleWithPath("/Applications");
}

id NSBundle_bundleWithPath(const char* path) {
    CamelOSBundle* bundle = (CamelOSBundle*)class_createInstance(NSBundle_class, 0);
    if (bundle && path) {
        strncpy(bundle->bundle_path, path, 255);
        bundle->loaded = 0;
        track_object((id)bundle);
    }
    return (id)bundle;
}

const char* NSBundle_bundlePath(id self, SEL cmd) {
    (void)cmd;
    CamelOSBundle* bundle = (CamelOSBundle*)self;
    return bundle ? bundle->bundle_path : "";
}

const char* NSBundle_resourcePath(id self, SEL cmd) {
    (void)cmd;
    static char res_path[256];
    CamelOSBundle* bundle = (CamelOSBundle*)self;
    if (bundle) {
        strcpy(res_path, bundle->bundle_path);
        strcat(res_path, "/Resources");
        return res_path;
    }
    return "";
}

BOOL NSBundle_load(id self, SEL cmd) {
    (void)cmd;
    CamelOSBundle* bundle = (CamelOSBundle*)self;
    if (!bundle) return NO;
    
    if (bundle->loaded) return YES;
    
    // Try to load the bundle using app_bundle_load
    extern int app_bundle_load(const char* path);
    int result = app_bundle_load(bundle->bundle_path);
    bundle->loaded = (result >= 0);
    
    return bundle->loaded;
}

static void register_NSBundle_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("bundlePath");
    class_addMethod(cls, sel, (void*)NSBundle_bundlePath, "*@:");
    sel = sel_registerName("resourcePath");
    class_addMethod(cls, sel, (void*)NSBundle_resourcePath, "*@:");
    sel = sel_registerName("load");
    class_addMethod(cls, sel, (void*)NSBundle_load, "B@:");
}

// ============================================================================
// NSUserDefaults (simplified - backed by /Library/Preferences)
// ============================================================================

id NSUserDefaults_standardUserDefaults(void) {
    // Return a singleton - simplified
    static CamelOSDictionary* defaults = 0;
    if (!defaults) {
        defaults = (CamelOSDictionary*)class_createInstance(NSDictionary_class, 0);
        if (defaults) {
            defaults->capacity = 32;
            defaults->keys = (id*)kmalloc(defaults->capacity * sizeof(id));
            defaults->values = (id*)kmalloc(defaults->capacity * sizeof(id));
            defaults->count = 0;
            track_object((id)defaults);
        }
    }
    return (id)defaults;
}

void NSUserDefaults_setObjectForKey(id self, SEL cmd, id obj, id key) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    if (!dict || !obj || !key) return;
    
    const char* key_str = NSString_UTF8String(key, 0);
    for (uint32_t i = 0; i < dict->count; i++) {
        const char* k = NSString_UTF8String(dict->keys[i], 0);
        if (strcmp(key_str, k) == 0) {
            dict->values[i] = obj;
            return;
        }
    }
    
    if (dict->count < dict->capacity) {
        dict->keys[dict->count] = key;
        dict->values[dict->count] = obj;
        dict->count++;
    }
}

id NSUserDefaults_objectForKey(id self, SEL cmd, id key) {
    (void)cmd;
    return NSDictionary_objectForKey(self, 0, key);
}

void NSUserDefaults_synchronize(id self, SEL cmd) {
    (void)self;
    (void)cmd;
    // TODO: Write defaults to /Library/Preferences
}

// ============================================================================
// Foundation Initialization
// ============================================================================

void foundation_set_kernel_api(void* api) {
    g_kernel_api_ptr = api;
}

void foundation_init(void) {
    s_printf("[Foundation] Initializing Foundation framework stubs...\n");
    
    // Create NSString class (subclass of NSObject)
    NSString_class = objc_allocateClassPair(objc_getClass("NSObject"), "NSString", 
                                             sizeof(CamelOSString) - sizeof(struct objc_object));
    if (NSString_class) {
        register_NSString_methods(NSString_class);
        objc_registerClassPair(NSString_class);
    }
    
    // Create NSNumber class
    NSNumber_class = objc_allocateClassPair(objc_getClass("NSObject"), "NSNumber",
                                             sizeof(CamelOSNumber) - sizeof(struct objc_object));
    if (NSNumber_class) {
        register_NSNumber_methods(NSNumber_class);
        objc_registerClassPair(NSNumber_class);
    }
    
    // Create NSArray class
    NSArray_class = objc_allocateClassPair(objc_getClass("NSObject"), "NSArray",
                                            sizeof(CamelOSArray) - sizeof(struct objc_object));
    if (NSArray_class) {
        register_NSArray_methods(NSArray_class);
        objc_registerClassPair(NSArray_class);
    }
    
    // Create NSDictionary class
    NSDictionary_class = objc_allocateClassPair(objc_getClass("NSObject"), "NSDictionary",
                                                  sizeof(CamelOSDictionary) - sizeof(struct objc_object));
    if (NSDictionary_class) {
        register_NSDictionary_methods(NSDictionary_class);
        objc_registerClassPair(NSDictionary_class);
    }
    
    // Create NSBundle class
    NSBundle_class = objc_allocateClassPair(objc_getClass("NSObject"), "NSBundle",
                                              sizeof(CamelOSBundle) - sizeof(struct objc_object));
    if (NSBundle_class) {
        register_NSBundle_methods(NSBundle_class);
        objc_registerClassPair(NSBundle_class);
    }
    
    // Register NSObject methods on the root class
    Class nsobj = objc_getClass("NSObject");
    if (nsobj) {
        class_addMethod(nsobj, sel_registerName("alloc"), (void*)NSObject_alloc, "@@:");
        class_addMethod(nsobj, sel_registerName("init"), (void*)NSObject_init, "@@:");
        class_addMethod(nsobj, sel_registerName("dealloc"), (void*)NSObject_dealloc, "v@:");
        class_addMethod(nsobj, sel_registerName("retain"), (void*)NSObject_retain, "@@:");
        class_addMethod(nsobj, sel_registerName("release"), (void*)NSObject_release, "@@:");
        class_addMethod(nsobj, sel_registerName("autorelease"), (void*)NSObject_autorelease, "@@:");
        class_addMethod(nsobj, sel_registerName("retainCount"), (void*)NSObject_retainCount, "I@:");
        class_addMethod(nsobj, sel_registerName("self"), (void*)NSObject_self, "@@:");
        class_addMethod(nsobj, sel_registerName("class"), (void*)NSObject_class, "#@:");
        class_addMethod(nsobj, sel_registerName("isEqual:"), (void*)NSObject_isEqual, "B@:@");
        class_addMethod(nsobj, sel_registerName("hash"), (void*)NSObject_hash, "I@:");
        class_addMethod(nsobj, sel_registerName("description"), (void*)NSObject_description, "@@:");
        class_addMethod(nsobj, sel_registerName("respondsToSelector:"), (void*)NSObject_respondsToSelector, "B@::");
    }
    
    s_printf("[Foundation] Foundation framework initialized\n");
}
