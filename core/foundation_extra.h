// core/foundation_extra.h - Extended Foundation & CoreFoundation Stubs
// Adds NSFileManager, NSData, NSProcessInfo, NSThread, NSRunLoop,
// NSTimer, NSNotificationCenter, NSJSONSerialization, NSError,
// NSNull, NSSet, NSValue, NSURL, NSInputStream, NSOutputStream,
// and CoreFoundation types (CFString, CFArray, CFDictionary, CFRunLoop, etc.)
#ifndef FOUNDATION_EXTRA_H
#define FOUNDATION_EXTRA_H

#include "foundation_stub.h"
#include "objc_runtime.h"
#include "../include/types.h"

// ============================================================================
// NSData
// ============================================================================
typedef struct {
    struct objc_object isa;
    uint8_t* bytes;
    uint32_t length;
    uint32_t capacity;
    int owns_bytes;  // 1 if we should free bytes on dealloc
} CamelOSData;
extern Class NSData_class;

// ============================================================================
// NSFileManager
// ============================================================================
typedef struct {
    struct objc_object isa;
    int initialized;
} CamelOSFileManager;
extern Class NSFileManager_class;

// ============================================================================
// NSProcessInfo
// ============================================================================
typedef struct {
    struct objc_object isa;
    char process_name[64];
    int process_id;
    char os_version[32];
} CamelOSProcessInfo;
extern Class NSProcessInfo_class;

// ============================================================================
// NSError
// ============================================================================
typedef struct {
    struct objc_object isa;
    int32_t code;
    char domain[64];
    char description[256];
} CamelOSError;
extern Class NSError_class;

// ============================================================================
// NSNull
// ============================================================================
typedef struct {
    struct objc_object isa;
} CamelOSNull;
extern Class NSNull_class;

// ============================================================================
// NSSet
// ============================================================================
typedef struct {
    struct objc_object isa;
    id* objects;
    uint32_t count;
    uint32_t capacity;
} CamelOSSet;
extern Class NSSet_class;

// ============================================================================
// NSValue
// ============================================================================
typedef struct {
    struct objc_object isa;
    uint8_t value[16];  // Store up to 128 bits
    uint32_t size;
    char objctype[16];  // @encode type
} CamelOSValue;
extern Class NSValue_class;

// ============================================================================
// NSURL
// ============================================================================
typedef struct {
    struct objc_object isa;
    char url_string[512];
    char scheme[16];
    char host[128];
    char path[256];
    uint16_t port;
} CamelOSURL;
extern Class NSURL_class;

// ============================================================================
// NSNotificationCenter
// ============================================================================
typedef struct {
    struct objc_object isa;
} CamelOSNotificationCenter;

typedef struct {
    id observer;
    SEL selector;
    char name[64];
    id object;
} CamelOSNotificationObserver;

extern Class NSNotificationCenter_class;

// ============================================================================
// NSTimer
// ============================================================================
typedef struct {
    struct objc_object isa;
    uint32_t fire_date;    // Tick count when to fire
    uint32_t interval;     // Interval in ms (0 = one-shot)
    id target;
    SEL selector;
    id user_info;
    int repeats;
    int valid;
} CamelOSTimer;
extern Class NSTimer_class;

// ============================================================================
// NSRunLoop
// ============================================================================
typedef struct {
    struct objc_object isa;
    CamelOSTimer* timers[32];
    int timer_count;
    int running;
} CamelOSRunLoop;
extern Class NSRunLoop_class;

// ============================================================================
// NSThread
// ============================================================================
typedef struct {
    struct objc_object isa;
    id target;
    SEL selector;
    id argument;
    int executing;
    int finished;
    char name[32];
} CamelOSThread;
extern Class NSThread_class;

// ============================================================================
// NSJSONSerialization
// ============================================================================
extern Class NSJSONSerialization_class;

// ============================================================================
// CoreFoundation Types
// ============================================================================

// CFString (bridges to NSString)
typedef CamelOSString* CFStringRef;
typedef const CamelOSString* CFStringRefConst;

// CFArray (bridges to NSArray)
typedef CamelOSArray* CFArrayRef;

// CFDictionary (bridges to NSDictionary)
typedef CamelOSDictionary* CFDictionaryRef;

// CFData (bridges to NSData)
typedef CamelOSData* CFDataRef;

// CFBoolean
typedef int CFBooleanRef;
#define kCFBooleanTrue  ((CFBooleanRef)1)
#define kCFBooleanFalse ((CFBooleanRef)0)

// CFNumber
typedef CamelOSNumber* CFNumberRef;

// CFURL
typedef CamelOSURL* CFURLRef;

// CFAllocator
typedef void* CFAllocatorRef;
#define kCFAllocatorDefault ((CFAllocatorRef)0)
#define kCFAllocatorSystemDefault ((CFAllocatorRef)0)
#define kCFAllocatorNull ((CFAllocatorRef)-1)

// CFRunLoop
typedef CamelOSRunLoop* CFRunLoopRef;

// CFType
typedef const void* CFTypeRef;

// CFIndex operations
#define CFArrayGetCount(arr)     ((arr) ? ((CamelOSArray*)(arr))->count : 0)
#define CFDictionaryGetCount(d)  ((d) ? ((CamelOSDictionary*)(d))->count : 0)
#define CFDataGetLength(d)       ((d) ? ((CamelOSData*)(d))->length : 0)
#define CFStringGetLength(s)     ((s) ? ((CamelOSString*)(s))->length : 0)
#define CFStringGetCString(s,b,l,e) ((s) ? (strncpy((b), ((CamelOSString*)(s))->cstr, (l)), 1) : 0)

// CFRelease / CFRetain (bridge to NSObject refcounting)
void CFRelease(CFTypeRef cf);
void CFRetain(CFTypeRef cf);

// ============================================================================
// Extended Foundation Initialization
// ============================================================================

// Initialize the extra Foundation classes (call after foundation_init)
void foundation_extra_init(void);

#endif // FOUNDATION_EXTRA_H
