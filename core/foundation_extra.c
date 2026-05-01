// core/foundation_extra.c - Extended Foundation & CoreFoundation Implementation
// Adds the critical classes needed by most macOS apps:
// NSFileManager, NSData, NSProcessInfo, NSError, NSNull, NSSet, NSValue,
// NSURL, NSNotificationCenter, NSTimer, NSRunLoop, NSThread, NSJSONSerialization
// Plus CoreFoundation toll-free bridged types

#include "foundation_extra.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../sys/cdl_defs.h"
#include "../hal/drivers/serial.h"

// --- Class References ---
Class NSData_class = 0;
Class NSFileManager_class = 0;
Class NSProcessInfo_class = 0;
Class NSError_class = 0;
Class NSNull_class = 0;
Class NSSet_class = 0;
Class NSValue_class = 0;
Class NSURL_class = 0;
Class NSNotificationCenter_class = 0;
Class NSTimer_class = 0;
Class NSRunLoop_class = 0;
Class NSThread_class = 0;
Class NSJSONSerialization_class = 0;

// Forward declarations
id NSRunLoop_currentRunLoop(id self, SEL cmd);

// ============================================================================
// NSData Implementation
// ============================================================================

id NSData_alloc(void) {
    return NSObject_alloc(NSData_class, 0);
}

id NSData_initWithBytes(id self, SEL cmd, const void* bytes, uint32_t length) {
    (void)cmd;
    CamelOSData* data = (CamelOSData*)self;
    if (!data) return 0;

    if (length > 0 && bytes) {
        data->bytes = (uint8_t*)kmalloc(length);
        if (data->bytes) {
            memcpy(data->bytes, bytes, length);
            data->owns_bytes = 1;
        }
    } else {
        data->bytes = 0;
        data->owns_bytes = 0;
    }
    data->length = length;
    data->capacity = length;

    return self;
}

id NSData_initWithBytesNoCopy(id self, SEL cmd, void* bytes, uint32_t length) {
    (void)cmd;
    CamelOSData* data = (CamelOSData*)self;
    if (!data) return 0;

    data->bytes = (uint8_t*)bytes;
    data->length = length;
    data->capacity = length;
    data->owns_bytes = 0;  // Don't free on dealloc

    return self;
}

id NSData_dataWithBytes(const void* bytes, uint32_t length) {
    id data = NSData_alloc();
    data = NSObject_init(data, 0);
    return NSData_initWithBytes(data, 0, bytes, length);
}

uint32_t NSData_length(id self, SEL cmd) {
    (void)cmd;
    CamelOSData* data = (CamelOSData*)self;
    return data ? data->length : 0;
}

const uint8_t* NSData_bytes(id self, SEL cmd) {
    (void)cmd;
    CamelOSData* data = (CamelOSData*)self;
    return data ? data->bytes : 0;
}

id NSData_subdataWithRange(id self, SEL cmd, uint32_t loc, uint32_t len) {
    (void)cmd;
    CamelOSData* data = (CamelOSData*)self;
    if (!data) return 0;
    if (loc + len > data->length) return 0;
    return NSData_dataWithBytes(data->bytes + loc, len);
}

static void register_NSData_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("initWithBytes:length:");
    class_addMethod(cls, sel, (void*)NSData_initWithBytes, "@@:*I");
    sel = sel_registerName("initWithBytesNoCopy:length:");
    class_addMethod(cls, sel, (void*)NSData_initWithBytesNoCopy, "@@:*I");
    sel = sel_registerName("length");
    class_addMethod(cls, sel, (void*)NSData_length, "I@:");
    sel = sel_registerName("bytes");
    class_addMethod(cls, sel, (void*)NSData_bytes, "*@:");
    sel = sel_registerName("subdataWithRange:");
    class_addMethod(cls, sel, (void*)NSData_subdataWithRange, "@@:II");
}

// ============================================================================
// NSFileManager Implementation
// ============================================================================

id NSFileManager_defaultManager(void) {
    static CamelOSFileManager* mgr = 0;
    if (!mgr) {
        mgr = (CamelOSFileManager*)class_createInstance(NSFileManager_class, 0);
        if (mgr) {
            mgr->initialized = 1;
            track_object((id)mgr);
        }
    }
    return (id)mgr;
}

BOOL NSFileManager_fileExistsAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    if (!path) return NO;
    return sys_fs_exists(path) ? YES : NO;
}

BOOL NSFileManager_isDirectoryAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    if (!path) return NO;
    return sys_fs_is_dir(path) ? YES : NO;
}

id NSFileManager_contentsAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    if (!path) return 0;
    // Read file into NSData
    char buf[4096];
    int len = sys_fs_read(path, buf, sizeof(buf));
    if (len <= 0) return 0;
    return NSData_dataWithBytes(buf, len);
}

BOOL NSFileManager_createDirectoryAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    if (!path) return NO;
    return sys_fs_create(path, 1) == 0 ? YES : NO;
}

BOOL NSFileManager_removeItemAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    if (!path) return NO;
    return sys_fs_delete(path) == 0 ? YES : NO;
}

BOOL NSFileManager_copyItemAtPathToPath(id self, SEL cmd, const char* src, const char* dst) {
    (void)self; (void)cmd;
    if (!src || !dst) return NO;
    sys_fs_copy(src, dst);
    return YES;
}

BOOL NSFileManager_moveItemAtPathToPath(id self, SEL cmd, const char* src, const char* dst) {
    (void)self; (void)cmd;
    if (!src || !dst) return NO;
    sys_fs_rename(src, dst);
    return YES;
}

uint32_t NSFileManager_fileSizeAtPath(id self, SEL cmd, const char* path) {
    (void)self; (void)cmd;
    (void)path;
    // TODO: Use pfs32_stat to get actual file size
    return 0;
}

static void register_NSFileManager_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("fileExistsAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_fileExistsAtPath, "B@:*");
    sel = sel_registerName("isDirectoryAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_isDirectoryAtPath, "B@:*");
    sel = sel_registerName("contentsAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_contentsAtPath, "@@:*");
    sel = sel_registerName("createDirectoryAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_createDirectoryAtPath, "B@:*");
    sel = sel_registerName("removeItemAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_removeItemAtPath, "B@:*");
    sel = sel_registerName("copyItemAtPath:toPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_copyItemAtPathToPath, "B@:**");
    sel = sel_registerName("moveItemAtPath:toPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_moveItemAtPathToPath, "B@:**");
    sel = sel_registerName("fileSizeAtPath:");
    class_addMethod(cls, sel, (void*)NSFileManager_fileSizeAtPath, "I@:*");
}

// ============================================================================
// NSProcessInfo Implementation
// ============================================================================

id NSProcessInfo_processInfo(void) {
    static CamelOSProcessInfo* info = 0;
    if (!info) {
        info = (CamelOSProcessInfo*)class_createInstance(NSProcessInfo_class, 0);
        if (info) {
            strcpy(info->process_name, "CamelOS");
            info->process_id = 1;
            strcpy(info->os_version, "CamelOS 1.0");
            track_object((id)info);
        }
    }
    return (id)info;
}

const char* NSProcessInfo_processName(id self, SEL cmd) {
    (void)cmd;
    CamelOSProcessInfo* info = (CamelOSProcessInfo*)self;
    return info ? info->process_name : "unknown";
}

int NSProcessInfo_processIdentifier(id self, SEL cmd) {
    (void)cmd;
    CamelOSProcessInfo* info = (CamelOSProcessInfo*)self;
    return info ? info->process_id : 0;
}

const char* NSProcessInfo_operatingSystemVersionString(id self, SEL cmd) {
    (void)cmd;
    CamelOSProcessInfo* info = (CamelOSProcessInfo*)self;
    return info ? info->os_version : "unknown";
}

uint32_t NSProcessInfo_processorCount(id self, SEL cmd) {
    (void)self; (void)cmd;
    return 1;  // CamelOS is single-CPU currently
}

uint32_t NSProcessInfo_physicalMemory(id self, SEL cmd) {
    (void)self; (void)cmd;
    extern kernel_api_t g_kernel_api;
    return g_kernel_api.mem_total();
}

static void register_NSProcessInfo_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("processName");
    class_addMethod(cls, sel, (void*)NSProcessInfo_processName, "*@:");
    sel = sel_registerName("processIdentifier");
    class_addMethod(cls, sel, (void*)NSProcessInfo_processIdentifier, "i@:");
    sel = sel_registerName("operatingSystemVersionString");
    class_addMethod(cls, sel, (void*)NSProcessInfo_operatingSystemVersionString, "*@:");
    sel = sel_registerName("processorCount");
    class_addMethod(cls, sel, (void*)NSProcessInfo_processorCount, "I@:");
    sel = sel_registerName("physicalMemory");
    class_addMethod(cls, sel, (void*)NSProcessInfo_physicalMemory, "I@:");
}

// ============================================================================
// NSError Implementation
// ============================================================================

id NSError_errorWithDomainCodeDescription(const char* domain, int32_t code, const char* desc) {
    CamelOSError* err = (CamelOSError*)class_createInstance(NSError_class, 0);
    if (err) {
        if (domain) strncpy(err->domain, domain, 63);
        err->domain[63] = 0;
        err->code = code;
        if (desc) strncpy(err->description, desc, 255);
        err->description[255] = 0;
        track_object((id)err);
    }
    return (id)err;
}

int32_t NSError_code(id self, SEL cmd) {
    (void)cmd;
    CamelOSError* err = (CamelOSError*)self;
    return err ? err->code : 0;
}

const char* NSError_domain(id self, SEL cmd) {
    (void)cmd;
    CamelOSError* err = (CamelOSError*)self;
    return err ? err->domain : "";
}

const char* NSError_localizedDescription(id self, SEL cmd) {
    (void)cmd;
    CamelOSError* err = (CamelOSError*)self;
    return err ? err->description : "Unknown error";
}

static void register_NSError_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("code");
    class_addMethod(cls, sel, (void*)NSError_code, "i@:");
    sel = sel_registerName("domain");
    class_addMethod(cls, sel, (void*)NSError_domain, "*@:");
    sel = sel_registerName("localizedDescription");
    class_addMethod(cls, sel, (void*)NSError_localizedDescription, "*@:");
}

// ============================================================================
// NSNull Implementation
// ============================================================================

id NSNull_null(void) {
    static CamelOSNull* null_obj = 0;
    if (!null_obj) {
        null_obj = (CamelOSNull*)class_createInstance(NSNull_class, 0);
        if (null_obj) track_object((id)null_obj);
    }
    return (id)null_obj;
}

static void register_NSNull_methods(Class cls) {
    (void)cls;
    // NSNull has no instance methods beyond NSObject
}

// ============================================================================
// NSSet Implementation
// ============================================================================

id NSSet_setWithObject(id obj) {
    CamelOSSet* set = (CamelOSSet*)class_createInstance(NSSet_class, 0);
    if (set && obj) {
        set->capacity = 8;
        set->objects = (id*)kmalloc(set->capacity * sizeof(id));
        if (set->objects) {
            set->objects[0] = obj;
            set->count = 1;
        }
        track_object((id)set);
    }
    return (id)set;
}

uint32_t NSSet_count(id self, SEL cmd) {
    (void)cmd;
    CamelOSSet* set = (CamelOSSet*)self;
    return set ? set->count : 0;
}

BOOL NSSet_containsObject(id self, SEL cmd, id obj) {
    (void)cmd;
    CamelOSSet* set = (CamelOSSet*)self;
    if (!set || !obj) return NO;
    // Compare by pointer equality (simplified)
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->objects[i] == obj) return YES;
    }
    return NO;
}

static void register_NSSet_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("count");
    class_addMethod(cls, sel, (void*)NSSet_count, "I@:");
    sel = sel_registerName("containsObject:");
    class_addMethod(cls, sel, (void*)NSSet_containsObject, "B@:@");
}

// ============================================================================
// NSValue Implementation
// ============================================================================

id NSValue_valueWithBytes(const void* value, uint32_t size, const char* type) {
    CamelOSValue* val = (CamelOSValue*)class_createInstance(NSValue_class, 0);
    if (val) {
        if (size <= 16 && value) {
            memcpy(val->value, value, size);
        }
        val->size = size;
        if (type) strncpy(val->objctype, type, 15);
        val->objctype[15] = 0;
        track_object((id)val);
    }
    return (id)val;
}

id NSValue_valueWithPointer(void* ptr) {
    return NSValue_valueWithBytes(&ptr, sizeof(void*), "^v");
}

void* NSValue_pointerValue(id self, SEL cmd) {
    (void)cmd;
    CamelOSValue* val = (CamelOSValue*)self;
    if (!val || val->size < sizeof(void*)) return 0;
    void* result;
    memcpy(&result, val->value, sizeof(void*));
    return result;
}

static void register_NSValue_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("pointerValue");
    class_addMethod(cls, sel, (void*)NSValue_pointerValue, "^v@:");
}

// ============================================================================
// NSURL Implementation
// ============================================================================

id NSURL_urlWithString(const char* string) {
    CamelOSURL* url = (CamelOSURL*)class_createInstance(NSURL_class, 0);
    if (url && string) {
        strncpy(url->url_string, string, 511);
        url->url_string[511] = 0;

        // Parse scheme
        const char* p = string;
        while (*p && *p != ':' && *p != '/') p++;
        if (*p == ':') {
            int scheme_len = p - string;
            if (scheme_len < 16) {
                memcpy(url->scheme, string, scheme_len);
                url->scheme[scheme_len] = 0;
            }
            p += 3;  // Skip "://"
        } else {
            strcpy(url->scheme, "file");
            p = string;
        }

        // Parse host and path
        const char* q = p;
        while (*q && *q != '/') q++;
        if (q > p) {
            int host_len = q - p;
            if (host_len < 128) {
                memcpy(url->host, p, host_len);
                url->host[host_len] = 0;
            }
        }
        if (*q) {
            strncpy(url->path, q, 255);
            url->path[255] = 0;
        }

        track_object((id)url);
    }
    return (id)url;
}

id NSURL_fileURLWithPath(const char* path) {
    if (!path) return 0;
    char full[512];
    strcpy(full, "file://");
    strcat(full, path);
    return NSURL_urlWithString(full);
}

const char* NSURL_absoluteString(id self, SEL cmd) {
    (void)cmd;
    CamelOSURL* url = (CamelOSURL*)self;
    return url ? url->url_string : "";
}

const char* NSURL_path(id self, SEL cmd) {
    (void)cmd;
    CamelOSURL* url = (CamelOSURL*)self;
    return url ? url->path : "";
}

static void register_NSURL_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("absoluteString");
    class_addMethod(cls, sel, (void*)NSURL_absoluteString, "*@:");
    sel = sel_registerName("path");
    class_addMethod(cls, sel, (void*)NSURL_path, "*@:");
}

// ============================================================================
// NSNotificationCenter Implementation
// ============================================================================

#define MAX_OBSERVERS 64
static CamelOSNotificationObserver g_observers[MAX_OBSERVERS];
static int g_observer_count = 0;

id NSNotificationCenter_defaultCenter(void) {
    static CamelOSNotificationCenter* center = 0;
    if (!center) {
        center = (CamelOSNotificationCenter*)class_createInstance(NSNotificationCenter_class, 0);
        if (center) track_object((id)center);
    }
    return (id)center;
}

void NSNotificationCenter_addObserverSelectorName(id self, SEL cmd,
    id observer, SEL selector, const char* name, id object) {
    (void)self; (void)cmd;
    if (!observer || !selector || !name) return;
    if (g_observer_count >= MAX_OBSERVERS) return;

    CamelOSNotificationObserver* obs = &g_observers[g_observer_count++];
    obs->observer = observer;
    obs->selector = selector;
    strncpy(obs->name, name, 63);
    obs->name[63] = 0;
    obs->object = object;
}

void NSNotificationCenter_removeObserver(id self, SEL cmd, id observer) {
    (void)self; (void)cmd;
    if (!observer) return;
    for (int i = 0; i < g_observer_count; i++) {
        if (g_observers[i].observer == observer) {
            g_observers[i] = g_observers[g_observer_count - 1];
            g_observer_count--;
            i--;  // Re-check this slot
        }
    }
}

void NSNotificationCenter_postNotificationName(id self, SEL cmd,
    const char* name, id object) {
    (void)self; (void)cmd;
    if (!name) return;
    for (int i = 0; i < g_observer_count; i++) {
        if (strcmp(g_observers[i].name, name) == 0) {
            // Invoke the observer's selector
            if (g_observers[i].observer && g_observers[i].selector) {
                // Simplified: call via objc_msgSend
                objc_msgSend(g_observers[i].observer, g_observers[i].selector, object);
            }
        }
    }
}

static void register_NSNotificationCenter_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("addObserver:selector:name:object:");
    class_addMethod(cls, sel, (void*)NSNotificationCenter_addObserverSelectorName, "v@:@:*@");
    sel = sel_registerName("removeObserver:");
    class_addMethod(cls, sel, (void*)NSNotificationCenter_removeObserver, "v@:@");
    sel = sel_registerName("postNotificationName:object:");
    class_addMethod(cls, sel, (void*)NSNotificationCenter_postNotificationName, "v@:*@");
}

// ============================================================================
// NSTimer Implementation
// ============================================================================

id NSTimer_scheduledTimerWithTimeInterval(id target, SEL cmd,
    uint32_t interval_ms, id t, SEL sel, id user_info, int repeats) {
    (void)cmd;
    CamelOSTimer* timer = (CamelOSTimer*)class_createInstance(NSTimer_class, 0);
    if (timer) {
        extern kernel_api_t g_kernel_api;
        timer->fire_date = g_kernel_api.get_ticks() + interval_ms;
        timer->interval = interval_ms;
        timer->target = t;
        timer->selector = sel;
        timer->user_info = user_info;
        timer->repeats = repeats;
        timer->valid = 1;
        track_object((id)timer);

        // Register with the current run loop
        id runloop = NSRunLoop_currentRunLoop(0, 0);
        if (runloop) {
            CamelOSRunLoop* rl = (CamelOSRunLoop*)runloop;
            if (rl->timer_count < 32) {
                rl->timers[rl->timer_count++] = timer;
            }
        }
    }
    return (id)timer;
}

BOOL NSTimer_isValid(id self, SEL cmd) {
    (void)cmd;
    CamelOSTimer* timer = (CamelOSTimer*)self;
    return timer ? timer->valid : NO;
}

void NSTimer_invalidate(id self, SEL cmd) {
    (void)cmd;
    CamelOSTimer* timer = (CamelOSTimer*)self;
    if (timer) timer->valid = 0;
}

void NSTimer_fire(id self, SEL cmd) {
    (void)cmd;
    CamelOSTimer* timer = (CamelOSTimer*)self;
    if (!timer || !timer->valid || !timer->target || !timer->selector) return;

    // Invoke the target's selector
    objc_msgSend(timer->target, timer->selector, self);

    if (timer->repeats) {
        extern kernel_api_t g_kernel_api;
        timer->fire_date = g_kernel_api.get_ticks() + timer->interval;
    } else {
        timer->valid = 0;
    }
}

static void register_NSTimer_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("isValid");
    class_addMethod(cls, sel, (void*)NSTimer_isValid, "B@:");
    sel = sel_registerName("invalidate");
    class_addMethod(cls, sel, (void*)NSTimer_invalidate, "v@:");
    sel = sel_registerName("fire");
    class_addMethod(cls, sel, (void*)NSTimer_fire, "v@:");
}

// ============================================================================
// NSRunLoop Implementation
// ============================================================================

id NSRunLoop_currentRunLoop(id self, SEL cmd) {
    (void)self; (void)cmd;
    static CamelOSRunLoop* loop = 0;
    if (!loop) {
        loop = (CamelOSRunLoop*)class_createInstance(NSRunLoop_class, 0);
        if (loop) {
            memset(loop->timers, 0, sizeof(loop->timers));
            loop->timer_count = 0;
            loop->running = 0;
            track_object((id)loop);
        }
    }
    return (id)loop;
}

id NSRunLoop_mainRunLoop(id self, SEL cmd) {
    return NSRunLoop_currentRunLoop(self, cmd);
}

void NSRunLoop_run(id self, SEL cmd) {
    (void)cmd;
    CamelOSRunLoop* loop = (CamelOSRunLoop*)self;
    if (!loop) return;

    loop->running = 1;
    extern kernel_api_t g_kernel_api;

    while (loop->running) {
        // Check timers
        uint32_t now = g_kernel_api.get_ticks();
        for (int i = 0; i < loop->timer_count; i++) {
            if (loop->timers[i] && loop->timers[i]->valid) {
                if (now >= loop->timers[i]->fire_date) {
                    NSTimer_fire((id)loop->timers[i], 0);
                }
            }
        }

        // Process system events
        g_kernel_api.process_events();

        // Small delay to prevent busy-waiting
        sys_delay(1);
    }
}

void NSRunLoop_stop(id self, SEL cmd) {
    (void)cmd;
    CamelOSRunLoop* loop = (CamelOSRunLoop*)self;
    if (loop) loop->running = 0;
}

static void register_NSRunLoop_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("run");
    class_addMethod(cls, sel, (void*)NSRunLoop_run, "v@:");
    sel = sel_registerName("stop");
    class_addMethod(cls, sel, (void*)NSRunLoop_stop, "v@:");
}

// ============================================================================
// NSThread Implementation
// ============================================================================

id NSThread_currentThread(id self, SEL cmd) {
    (void)self; (void)cmd;
    static CamelOSThread* main_thread = 0;
    if (!main_thread) {
        main_thread = (CamelOSThread*)class_createInstance(NSThread_class, 0);
        if (main_thread) {
            strcpy(main_thread->name, "main");
            main_thread->executing = 1;
            track_object((id)main_thread);
        }
    }
    return (id)main_thread;
}

BOOL NSThread_isMultiThreaded(id self, SEL cmd) {
    (void)self; (void)cmd;
    return NO;  // CamelOS is single-threaded currently
}

void NSThread_sleepForTimeInterval(id self, SEL cmd, uint32_t ms) {
    (void)self; (void)cmd;
    sys_delay(ms);
}

static void register_NSThread_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("sleepForTimeInterval:");
    class_addMethod(cls, sel, (void*)NSThread_sleepForTimeInterval, "v@:I");
}

// ============================================================================
// NSJSONSerialization Implementation (stub - returns empty data)
// ============================================================================

id NSJSONSerialization_dataWithJSONObject(id self, SEL cmd, id obj, int opt, id* error) {
    (void)self; (void)cmd; (void)obj; (void)opt;
    if (error) *error = 0;
    // TODO: Implement actual JSON serialization
    // For now return empty data
    return NSData_dataWithBytes("{}", 2);
}

id NSJSONSerialization_JSONObjectWithData(id self, SEL cmd, id data, int opt, id* error) {
    (void)self; (void)cmd; (void)data; (void)opt;
    if (error) *error = 0;
    // TODO: Implement actual JSON parsing
    return 0;
}

static void register_NSJSONSerialization_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("dataWithJSONObject:options:error:");
    class_addMethod(cls, sel, (void*)NSJSONSerialization_dataWithJSONObject, "@@:@i^@");
    sel = sel_registerName("JSONObjectWithData:options:error:");
    class_addMethod(cls, sel, (void*)NSJSONSerialization_JSONObjectWithData, "@@:@i^@");
}

// ============================================================================
// CoreFoundation Bridge
// ============================================================================

void CFRelease(CFTypeRef cf) {
    if (!cf) return;
    NSObject_release((id)cf, 0);
}

void CFRetain(CFTypeRef cf) {
    if (!cf) return;
    NSObject_retain((id)cf, 0);
}

// ============================================================================
// Extended Foundation Initialization
// ============================================================================

void foundation_extra_init(void) {
    s_printf("[Foundation] Initializing extended Foundation classes...\n");

    Class nsobj = objc_getClass("NSObject");

    // NSData
    NSData_class = objc_allocateClassPair(nsobj, "NSData",
        sizeof(CamelOSData) - sizeof(struct objc_object));
    if (NSData_class) {
        register_NSData_methods(NSData_class);
        objc_registerClassPair(NSData_class);
    }

    // NSFileManager
    NSFileManager_class = objc_allocateClassPair(nsobj, "NSFileManager",
        sizeof(CamelOSFileManager) - sizeof(struct objc_object));
    if (NSFileManager_class) {
        register_NSFileManager_methods(NSFileManager_class);
        objc_registerClassPair(NSFileManager_class);
    }

    // NSProcessInfo
    NSProcessInfo_class = objc_allocateClassPair(nsobj, "NSProcessInfo",
        sizeof(CamelOSProcessInfo) - sizeof(struct objc_object));
    if (NSProcessInfo_class) {
        register_NSProcessInfo_methods(NSProcessInfo_class);
        objc_registerClassPair(NSProcessInfo_class);
    }

    // NSError
    NSError_class = objc_allocateClassPair(nsobj, "NSError",
        sizeof(CamelOSError) - sizeof(struct objc_object));
    if (NSError_class) {
        register_NSError_methods(NSError_class);
        objc_registerClassPair(NSError_class);
    }

    // NSNull
    NSNull_class = objc_allocateClassPair(nsobj, "NSNull",
        sizeof(CamelOSNull) - sizeof(struct objc_object));
    if (NSNull_class) {
        register_NSNull_methods(NSNull_class);
        objc_registerClassPair(NSNull_class);
    }

    // NSSet
    NSSet_class = objc_allocateClassPair(nsobj, "NSSet",
        sizeof(CamelOSSet) - sizeof(struct objc_object));
    if (NSSet_class) {
        register_NSSet_methods(NSSet_class);
        objc_registerClassPair(NSSet_class);
    }

    // NSValue
    NSValue_class = objc_allocateClassPair(nsobj, "NSValue",
        sizeof(CamelOSValue) - sizeof(struct objc_object));
    if (NSValue_class) {
        register_NSValue_methods(NSValue_class);
        objc_registerClassPair(NSValue_class);
    }

    // NSURL
    NSURL_class = objc_allocateClassPair(nsobj, "NSURL",
        sizeof(CamelOSURL) - sizeof(struct objc_object));
    if (NSURL_class) {
        register_NSURL_methods(NSURL_class);
        objc_registerClassPair(NSURL_class);
    }

    // NSNotificationCenter
    NSNotificationCenter_class = objc_allocateClassPair(nsobj, "NSNotificationCenter",
        sizeof(CamelOSNotificationCenter) - sizeof(struct objc_object));
    if (NSNotificationCenter_class) {
        register_NSNotificationCenter_methods(NSNotificationCenter_class);
        objc_registerClassPair(NSNotificationCenter_class);
    }

    // NSTimer
    NSTimer_class = objc_allocateClassPair(nsobj, "NSTimer",
        sizeof(CamelOSTimer) - sizeof(struct objc_object));
    if (NSTimer_class) {
        register_NSTimer_methods(NSTimer_class);
        objc_registerClassPair(NSTimer_class);
    }

    // NSRunLoop
    NSRunLoop_class = objc_allocateClassPair(nsobj, "NSRunLoop",
        sizeof(CamelOSRunLoop) - sizeof(struct objc_object));
    if (NSRunLoop_class) {
        register_NSRunLoop_methods(NSRunLoop_class);
        objc_registerClassPair(NSRunLoop_class);
    }

    // NSThread
    NSThread_class = objc_allocateClassPair(nsobj, "NSThread",
        sizeof(CamelOSThread) - sizeof(struct objc_object));
    if (NSThread_class) {
        register_NSThread_methods(NSThread_class);
        objc_registerClassPair(NSThread_class);
    }

    // NSJSONSerialization
    NSJSONSerialization_class = objc_allocateClassPair(nsobj, "NSJSONSerialization",
        0);
    if (NSJSONSerialization_class) {
        register_NSJSONSerialization_methods(NSJSONSerialization_class);
        objc_registerClassPair(NSJSONSerialization_class);
    }

    s_printf("[Foundation] Extended Foundation classes initialized\n");
}
