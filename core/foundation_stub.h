// core/foundation_stub.h - Foundation Framework Stubs for CamelOS
// Provides minimal implementations of core Foundation classes
// Required for macOS app compatibility via ravynos SDK/KDK
#ifndef FOUNDATION_STUB_H
#define FOUNDATION_STUB_H

#include "objc_runtime.h"
#include "../include/types.h"

// --- Core Foundation Types ---
typedef const void* CFTypeRef;
typedef uint32_t CFTypeID;
typedef int32_t CFIndex;
typedef uint32_t CFStringEncoding;
typedef uint8_t CFComparisonResult;

#define kCFStringEncodingUTF8  0x08000100
#define kCFStringEncodingASCII 0x0600

#define kCFCompareLessThan    (-1)
#define kCFCompareEqualTo     0
#define kCFCompareGreaterThan 1

// --- NSString ---
// CamelOS NSString is backed by a simple C string
typedef struct {
    struct objc_object isa;
    char* cstr;           // UTF-8 C string
    uint32_t length;      // String length
    uint32_t capacity;    // Allocated capacity
} CamelOSString;

// NSString class reference
extern Class NSString_class;

// NSString creation
id NSString_alloc(void);
id NSString_initWithCString(id self, SEL cmd, const char* cstr);
id NSString_initWithFormat(id self, SEL cmd, const char* fmt, ...);
id NSString_stringWithCString(const char* cstr);

// NSString methods
uint32_t NSString_length(id self, SEL cmd);
const char* NSString_UTF8String(id self, SEL cmd);
CFComparisonResult NSString_compare(id self, SEL cmd, id other);
BOOL NSString_isEqualToString(id self, SEL cmd, id other);
id NSString_stringByAppendingString(id self, SEL cmd, id other);

// --- NSNumber ---
typedef struct {
    struct objc_object isa;
    union {
        int32_t int_val;
        float float_val;
        int bool_val;
    } value;
    char type;  // 'i' int, 'f' float, 'b' bool
} CamelOSNumber;

extern Class NSNumber_class;

id NSNumber_numberWithInt(int value);
id NSNumber_numberWithFloat(float value);
id NSNumber_numberWithBool(BOOL value);

// --- NSArray ---
typedef struct {
    struct objc_object isa;
    id* objects;
    uint32_t count;
    uint32_t capacity;
} CamelOSArray;

extern Class NSArray_class;

id NSArray_arrayWithObject(id obj);
id NSArray_arrayWithObjects(id first, ...);
uint32_t NSArray_count(id self, SEL cmd);
id NSArray_objectAtIndex(id self, SEL cmd, uint32_t index);

// --- NSDictionary ---
typedef struct {
    struct objc_object isa;
    id* keys;
    id* values;
    uint32_t count;
    uint32_t capacity;
} CamelOSDictionary;

extern Class NSDictionary_class;

id NSDictionary_dictionaryWithObjectForKey(id obj, id key);
uint32_t NSDictionary_count(id self, SEL cmd);
id NSDictionary_objectForKey(id self, SEL cmd, id key);

// --- NSBundle ---
typedef struct {
    struct objc_object isa;
    char bundle_path[256];
    char identifier[128];
    int loaded;
} CamelOSBundle;

extern Class NSBundle_class;

id NSBundle_mainBundle(void);
id NSBundle_bundleWithPath(const char* path);
const char* NSBundle_bundlePath(id self, SEL cmd);
const char* NSBundle_resourcePath(id self, SEL cmd);
BOOL NSBundle_load(id self, SEL cmd);

// --- NSObject stubs ---
id NSObject_alloc(Class cls, SEL cmd);
id NSObject_init(id self, SEL cmd);
void NSObject_dealloc(id self, SEL cmd);
id NSObject_retain(id self, SEL cmd);
id NSObject_release(id self, SEL cmd);
id NSObject_autorelease(id self, SEL cmd);
uint32_t NSObject_retainCount(id self, SEL cmd);
id NSObject_self(id self, SEL cmd);
Class NSObject_class(id self, SEL cmd);
BOOL NSObject_isEqual(id self, SEL cmd, id other);
uint32_t NSObject_hash(id self, SEL cmd);
id NSObject_description(id self, SEL cmd);
BOOL NSObject_respondsToSelector(id self, SEL cmd, SEL selector);

// --- NSUserDefaults (simplified) ---
id NSUserDefaults_standardUserDefaults(void);
id NSUserDefaults_objectForKey(id self, SEL cmd, id key);
void NSUserDefaults_setObjectForKey(id self, SEL cmd, id obj, id key);
void NSUserDefaults_synchronize(id self, SEL cmd);

// --- Foundation initialization ---
void foundation_init(void);

// --- Bridge to CamelOS kernel ---
// Maps Foundation calls to CamelOS kernel API
void foundation_set_kernel_api(void* api);

// --- Object tracking (used by extended Foundation and AppKit) ---
#define MAX_TRACKED_OBJECTS 4096
extern void* g_object_ptrs[];
extern int g_object_tracking_count;
extern uint32_t g_object_refcounts[];
int track_object(id obj);
int find_object_slot(id obj);

#endif // FOUNDATION_STUB_H
