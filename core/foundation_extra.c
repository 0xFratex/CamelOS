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
#include "../fs/pfs32.h"

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
    if (!path) return 0;

    // Use pfs32_stat to get the actual file size
    pfs32_direntry_t entry;
    if (pfs32_stat(path, &entry) == 0) {
        return entry.file_size;
    }

    // Fallback: try reading the file to determine its size
    char temp[4096];
    int total = sys_fs_read(path, temp, sizeof(temp));
    return total > 0 ? (uint32_t)total : 0;
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
            loop->accepting_input = 1;
            loop->timer_port = 0;
            loop->event_port = 0;
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

    while (loop->running && loop->accepting_input) {
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
// NSJSONSerialization Implementation
// ============================================================================

// JSON serialization: Convert NSDictionary/NSArray/NSString/NSNumber to JSON data
// Builds JSON string in a buffer and wraps it in NSData

#define JSON_BUF_SIZE 8192

static uint32_t json_serialize_value(id obj, char* buf, uint32_t buf_size, uint32_t pos);

static uint32_t json_serialize_string(const char* str, char* buf, uint32_t buf_size, uint32_t pos) {
    if (pos + 2 >= buf_size) return pos;
    buf[pos++] = '"';
    for (const char* p = str; *p && pos < buf_size - 2; p++) {
        switch (*p) {
            case '"':  if (pos + 2 < buf_size) { buf[pos++] = '\\'; buf[pos++] = '"'; } break;
            case '\\': if (pos + 2 < buf_size) { buf[pos++] = '\\'; buf[pos++] = '\\'; } break;
            case '\n': if (pos + 2 < buf_size) { buf[pos++] = '\\'; buf[pos++] = 'n'; } break;
            case '\r': if (pos + 2 < buf_size) { buf[pos++] = '\\'; buf[pos++] = 'r'; } break;
            case '\t': if (pos + 2 < buf_size) { buf[pos++] = '\\'; buf[pos++] = 't'; } break;
            default:   buf[pos++] = *p; break;
        }
    }
    buf[pos++] = '"';
    return pos;
}

static uint32_t json_serialize_value(id obj, char* buf, uint32_t buf_size, uint32_t pos) {
    if (!obj) {
        // null
        const char* null_str = "null";
        for (int i = 0; null_str[i] && pos < buf_size; i++)
            buf[pos++] = null_str[i];
        return pos;
    }

    // Check if it's an NSString
    CamelOSString* str_obj = (CamelOSString*)obj;
    Class str_class = objc_getClass("NSString");
    if (str_class && ((id)str_obj)->isa == str_class) {
        return json_serialize_string(str_obj->cstr, buf, buf_size, pos);
    }

    // Check if it's an NSNumber
    CamelOSNumber* num_obj = (CamelOSNumber*)obj;
    Class num_class = objc_getClass("NSNumber");
    if (num_class && ((id)num_obj)->isa == num_class) {
        char numbuf[32];
        if (num_obj->type == 'f') {
            // Simple integer representation of float (no FPU)
            int_to_str((int)num_obj->value.float_val, numbuf);
        } else if (num_obj->type == 'b') {
            numbuf[0] = num_obj->value.bool_val ? '1' : '0';
            numbuf[1] = '\0';
        } else {
            int_to_str(num_obj->value.int_val, numbuf);
        }
        for (int i = 0; numbuf[i] && pos < buf_size; i++)
            buf[pos++] = numbuf[i];
        return pos;
    }

    // Check if it's an NSArray
    CamelOSArray* arr_obj = (CamelOSArray*)obj;
    Class arr_class = objc_getClass("NSArray");
    if (arr_class && ((id)arr_obj)->isa == arr_class) {
        if (pos >= buf_size) return pos;
        buf[pos++] = '[';
        for (uint32_t i = 0; i < arr_obj->count; i++) {
            if (i > 0) { if (pos < buf_size) buf[pos++] = ','; }
            pos = json_serialize_value(arr_obj->objects[i], buf, buf_size, pos);
        }
        if (pos >= buf_size) return pos;
        buf[pos++] = ']';
        return pos;
    }

    // Check if it's an NSDictionary
    CamelOSDictionary* dict_obj = (CamelOSDictionary*)obj;
    Class dict_class = objc_getClass("NSDictionary");
    if (dict_class && ((id)dict_obj)->isa == dict_class) {
        if (pos >= buf_size) return pos;
        buf[pos++] = '{';
        for (uint32_t i = 0; i < dict_obj->count; i++) {
            if (i > 0) { if (pos < buf_size) buf[pos++] = ','; }
            // Key must be a string
            CamelOSString* key = (CamelOSString*)dict_obj->keys[i];
            if (key && key->cstr) {
                pos = json_serialize_string(key->cstr, buf, buf_size, pos);
                if (pos < buf_size) buf[pos++] = ':';
                pos = json_serialize_value(dict_obj->values[i], buf, buf_size, pos);
            }
        }
        if (pos >= buf_size) return pos;
        buf[pos++] = '}';
        return pos;
    }

    // Check if it's NSNull
    Class null_class = objc_getClass("NSNull");
    if (null_class && ((id)obj)->isa == null_class) {
        const char* null_str = "null";
        for (int i = 0; null_str[i] && pos < buf_size; i++)
            buf[pos++] = null_str[i];
        return pos;
    }

    // Fallback: serialize as null
    const char* null_str = "null";
    for (int i = 0; null_str[i] && pos < buf_size; i++)
        buf[pos++] = null_str[i];
    return pos;
}

id NSJSONSerialization_dataWithJSONObject(id self, SEL cmd, id obj, int opt, id* error) {
    (void)self; (void)cmd; (void)opt;
    if (error) *error = 0;
    if (!obj) {
        if (error) *error = (id)1;  // Indicate error
        return 0;
    }

    char* json_buf = (char*)kmalloc(JSON_BUF_SIZE);
    if (!json_buf) return 0;

    uint32_t pos = json_serialize_value(obj, json_buf, JSON_BUF_SIZE - 1, 0);
    json_buf[pos] = '\0';

    id result = NSData_dataWithBytes(json_buf, pos);
    kfree(json_buf);
    return result;
}

// JSON parsing: Convert JSON data to NSDictionary/NSArray/NSString/NSNumber
// Simple recursive descent parser

static const char* json_skip_whitespace(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// Forward declaration for recursive parsing
static id json_parse_value(const char** p);

static id json_parse_string(const char** p) {
    if (**p != '"') return 0;
    (*p)++;  // Skip opening quote

    char buf[512];
    int len = 0;

    while (**p && **p != '"' && len < 511) {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case '"':  buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                default:   buf[len++] = **p; break;
            }
        } else {
            buf[len++] = **p;
        }
        (*p)++;
    }
    buf[len] = '\0';
    if (**p == '"') (*p)++;  // Skip closing quote

    // Create NSString
    CamelOSString* str = (CamelOSString*)class_createInstance(objc_getClass("NSString"), 0);
    if (str) {
        strncpy(str->cstr, buf, 255);
        str->cstr[255] = '\0';
        track_object((id)str);
    }
    return (id)str;
}

static id json_parse_number(const char** p) {
    int negative = 0;
    if (**p == '-') { negative = 1; (*p)++; }

    int value = 0;
    int is_float = 0;
    while (**p >= '0' && **p <= '9') {
        value = value * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == '.') {
        is_float = 1;
        (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }
    if (**p == 'e' || **p == 'E') {
        is_float = 1;
        (*p)++;
        if (**p == '+' || **p == '-') (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }

    if (negative) value = -value;

    // Create NSNumber
    CamelOSNumber* num = (CamelOSNumber*)class_createInstance(objc_getClass("NSNumber"), 0);
    if (num) {
        num->value.int_val = value;
        num->type = is_float ? 'f' : 'i';
        num->value.float_val = (float)value;
        track_object((id)num);
    }
    return (id)num;
}

static id json_parse_array(const char** p) {
    if (**p != '[') return 0;
    (*p)++;

    CamelOSArray* arr = (CamelOSArray*)class_createInstance(objc_getClass("NSArray"), 0);
    if (!arr) return 0;
    arr->capacity = 16;
    arr->objects = (id*)kmalloc(arr->capacity * sizeof(id));
    arr->count = 0;

    *p = json_skip_whitespace(*p);
    if (**p == ']') { (*p)++; track_object((id)arr); return (id)arr; }

    while (**p) {
        *p = json_skip_whitespace(*p);
        id val = json_parse_value(p);
        if (val) {
            if (arr->count >= arr->capacity) {
                arr->capacity *= 2;
                id* new_objs = (id*)kmalloc(arr->capacity * sizeof(id));
                if (new_objs) {
                    memcpy(new_objs, arr->objects, arr->count * sizeof(id));
                    kfree(arr->objects);
                    arr->objects = new_objs;
                }
            }
            arr->objects[arr->count++] = val;
        }

        *p = json_skip_whitespace(*p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; break; }
        break;
    }

    track_object((id)arr);
    return (id)arr;
}

static id json_parse_object(const char** p) {
    if (**p != '{') return 0;
    (*p)++;

    CamelOSDictionary* dict = (CamelOSDictionary*)class_createInstance(objc_getClass("NSDictionary"), 0);
    if (!dict) return 0;
    dict->capacity = 16;
    dict->keys = (id*)kmalloc(dict->capacity * sizeof(id));
    dict->values = (id*)kmalloc(dict->capacity * sizeof(id));
    dict->count = 0;

    *p = json_skip_whitespace(*p);
    if (**p == '}') { (*p)++; track_object((id)dict); return (id)dict; }

    while (**p) {
        *p = json_skip_whitespace(*p);
        if (**p != '"') break;

        id key = json_parse_string(p);
        *p = json_skip_whitespace(*p);
        if (**p == ':') (*p)++;
        *p = json_skip_whitespace(*p);

        id val = json_parse_value(p);

        if (key && val && dict->count < dict->capacity) {
            dict->keys[dict->count] = key;
            dict->values[dict->count] = val;
            dict->count++;
        }

        *p = json_skip_whitespace(*p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; break; }
        break;
    }

    track_object((id)dict);
    return (id)dict;
}

static id json_parse_value(const char** p) {
    *p = json_skip_whitespace(*p);

    if (!**p) return 0;

    switch (**p) {
        case '"': return json_parse_string(p);
        case '{': return json_parse_object(p);
        case '[': return json_parse_array(p);
        case 't':  // true
            if (strncmp(*p, "true", 4) == 0) {
                *p += 4;
                CamelOSNumber* num = (CamelOSNumber*)class_createInstance(objc_getClass("NSNumber"), 0);
                if (num) { num->value.int_val = 1; num->type = 'b'; track_object((id)num); }
                return (id)num;
            }
            return 0;
        case 'f':  // false
            if (strncmp(*p, "false", 5) == 0) {
                *p += 5;
                CamelOSNumber* num = (CamelOSNumber*)class_createInstance(objc_getClass("NSNumber"), 0);
                if (num) { num->value.int_val = 0; num->type = 'b'; track_object((id)num); }
                return (id)num;
            }
            return 0;
        case 'n':  // null
            if (strncmp(*p, "null", 4) == 0) {
                *p += 4;
                return NSNull_null();
            }
            return 0;
        case '-':
        case '0' ... '9':
            return json_parse_number(p);
        default:
            return 0;
    }
}

id NSJSONSerialization_JSONObjectWithData(id self, SEL cmd, id data, int opt, id* error) {
    (void)self; (void)cmd; (void)opt;
    if (error) *error = 0;
    if (!data) return 0;

    // Get bytes from NSData
    CamelOSData* nsdata = (CamelOSData*)data;
    if (!nsdata || !nsdata->bytes || nsdata->length == 0) return 0;

    // Null-terminate the data for parsing
    char* json_str = (char*)kmalloc(nsdata->length + 1);
    if (!json_str) return 0;
    memcpy(json_str, nsdata->bytes, nsdata->length);
    json_str[nsdata->length] = '\0';

    const char* p = json_str;
    id result = json_parse_value(&p);

    kfree(json_str);
    return result;
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

// Forward declarations for mutable subclass method registration
static void register_NSMutableArray_methods(Class cls);
static void register_NSMutableDictionary_methods(Class cls);
static void register_NSMutableString_methods(Class cls);

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

    // NSMutableArray - inherits from NSArray (which is already registered)
    Class nsarray_class = objc_getClass("NSArray");
    NSMutableArray_class = objc_allocateClassPair(
        nsarray_class ? nsarray_class : nsobj, "NSMutableArray",
        sizeof(CamelOSArray) - sizeof(struct objc_object));
    if (NSMutableArray_class) {
        register_NSMutableArray_methods(NSMutableArray_class);
        objc_registerClassPair(NSMutableArray_class);
    }

    // NSMutableDictionary - inherits from NSDictionary
    Class nsdict_class = objc_getClass("NSDictionary");
    NSMutableDictionary_class = objc_allocateClassPair(
        nsdict_class ? nsdict_class : nsobj, "NSMutableDictionary",
        sizeof(CamelOSDictionary) - sizeof(struct objc_object));
    if (NSMutableDictionary_class) {
        register_NSMutableDictionary_methods(NSMutableDictionary_class);
        objc_registerClassPair(NSMutableDictionary_class);
    }

    // NSMutableString - inherits from NSString
    Class nsstring_class = objc_getClass("NSString");
    NSMutableString_class = objc_allocateClassPair(
        nsstring_class ? nsstring_class : nsobj, "NSMutableString",
        sizeof(CamelOSString) - sizeof(struct objc_object));
    if (NSMutableString_class) {
        register_NSMutableString_methods(NSMutableString_class);
        objc_registerClassPair(NSMutableString_class);
    }

    s_printf("[Foundation] Extended Foundation classes initialized\n");
}

// ============================================================================
// NSMutableArray Implementation (inherits NSArray, adds mutation methods)
// ============================================================================

Class NSMutableArray_class = 0;

id NSMutableArray_array(void) {
    CamelOSArray* arr = (CamelOSArray*)class_createInstance(NSMutableArray_class, 0);
    if (arr) {
        arr->capacity = 16;
        arr->objects = (id*)kmalloc(arr->capacity * sizeof(id));
        arr->count = 0;
        track_object((id)arr);
    }
    return (id)arr;
}

id NSMutableArray_arrayWithCapacity(uint32_t capacity) {
    if (capacity == 0) capacity = 16;
    CamelOSArray* arr = (CamelOSArray*)class_createInstance(NSMutableArray_class, 0);
    if (arr) {
        arr->capacity = capacity;
        arr->objects = (id*)kmalloc(arr->capacity * sizeof(id));
        arr->count = 0;
        track_object((id)arr);
    }
    return (id)arr;
}

void NSMutableArray_addObject(id self, SEL cmd, id object) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || !object) return;

    // Grow if needed
    if (arr->count >= arr->capacity) {
        arr->capacity = arr->capacity ? arr->capacity * 2 : 16;
        id* new_objs = (id*)kmalloc(arr->capacity * sizeof(id));
        if (new_objs) {
            memcpy(new_objs, arr->objects, arr->count * sizeof(id));
            kfree(arr->objects);
            arr->objects = new_objs;
        }
    }

    if (arr->count < arr->capacity) {
        arr->objects[arr->count++] = object;
    }
}

void NSMutableArray_removeObject(id self, SEL cmd, id object) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || !object) return;

    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->objects[i] == object) {
            // Shift remaining objects down
            for (uint32_t j = i; j < arr->count - 1; j++) {
                arr->objects[j] = arr->objects[j + 1];
            }
            arr->count--;
            return;  // Remove only first occurrence
        }
    }
}

void NSMutableArray_removeObjectAtIndex(id self, SEL cmd, uint32_t index) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || index >= arr->count) return;

    for (uint32_t j = index; j < arr->count - 1; j++) {
        arr->objects[j] = arr->objects[j + 1];
    }
    arr->count--;
}

void NSMutableArray_insertObjectAtIndex(id self, SEL cmd, id object, uint32_t index) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || !object || index > arr->count) return;

    // Grow if needed
    if (arr->count >= arr->capacity) {
        arr->capacity = arr->capacity ? arr->capacity * 2 : 16;
        id* new_objs = (id*)kmalloc(arr->capacity * sizeof(id));
        if (new_objs) {
            memcpy(new_objs, arr->objects, arr->count * sizeof(id));
            kfree(arr->objects);
            arr->objects = new_objs;
        }
    }

    // Shift objects up to make room
    for (uint32_t j = arr->count; j > index; j--) {
        arr->objects[j] = arr->objects[j - 1];
    }
    arr->objects[index] = object;
    arr->count++;
}

void NSMutableArray_removeAllObjects(id self, SEL cmd) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr) return;
    arr->count = 0;
}

void NSMutableArray_replaceObjectAtIndexWithObject(id self, SEL cmd, uint32_t index, id object) {
    (void)cmd;
    CamelOSArray* arr = (CamelOSArray*)self;
    if (!arr || !object || index >= arr->count) return;
    arr->objects[index] = object;
}

static void register_NSMutableArray_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("addObject:");
    class_addMethod(cls, sel, (void*)NSMutableArray_addObject, "v@:@");
    sel = sel_registerName("removeObject:");
    class_addMethod(cls, sel, (void*)NSMutableArray_removeObject, "v@:@");
    sel = sel_registerName("removeObjectAtIndex:");
    class_addMethod(cls, sel, (void*)NSMutableArray_removeObjectAtIndex, "v@:I");
    sel = sel_registerName("insertObject:atIndex:");
    class_addMethod(cls, sel, (void*)NSMutableArray_insertObjectAtIndex, "v@:@I");
    sel = sel_registerName("removeAllObjects");
    class_addMethod(cls, sel, (void*)NSMutableArray_removeAllObjects, "v@:");
    sel = sel_registerName("replaceObjectAtIndex:withObject:");
    class_addMethod(cls, sel, (void*)NSMutableArray_replaceObjectAtIndexWithObject, "v@:I@");
}

// ============================================================================
// NSMutableDictionary Implementation (inherits NSDictionary, adds mutation)
// ============================================================================

Class NSMutableDictionary_class = 0;

id NSMutableDictionary_dictionary(void) {
    CamelOSDictionary* dict = (CamelOSDictionary*)class_createInstance(NSMutableDictionary_class, 0);
    if (dict) {
        dict->capacity = 16;
        dict->keys = (id*)kmalloc(dict->capacity * sizeof(id));
        dict->values = (id*)kmalloc(dict->capacity * sizeof(id));
        dict->count = 0;
        track_object((id)dict);
    }
    return (id)dict;
}

id NSMutableDictionary_dictionaryWithCapacity(uint32_t capacity) {
    if (capacity == 0) capacity = 16;
    CamelOSDictionary* dict = (CamelOSDictionary*)class_createInstance(NSMutableDictionary_class, 0);
    if (dict) {
        dict->capacity = capacity;
        dict->keys = (id*)kmalloc(dict->capacity * sizeof(id));
        dict->values = (id*)kmalloc(dict->capacity * sizeof(id));
        dict->count = 0;
        track_object((id)dict);
    }
    return (id)dict;
}

void NSMutableDictionary_setObjectForKey(id self, SEL cmd, id object, id key) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    if (!dict || !key) return;

    // Check if key already exists - update value if so
    for (uint32_t i = 0; i < dict->count; i++) {
        if (dict->keys[i] == key) {
            dict->values[i] = object;
            return;
        }
    }

    // Key not found - add new entry
    if (dict->count >= dict->capacity) {
        dict->capacity = dict->capacity ? dict->capacity * 2 : 16;
        id* new_keys = (id*)kmalloc(dict->capacity * sizeof(id));
        id* new_values = (id*)kmalloc(dict->capacity * sizeof(id));
        if (new_keys && new_values) {
            memcpy(new_keys, dict->keys, dict->count * sizeof(id));
            memcpy(new_values, dict->values, dict->count * sizeof(id));
            kfree(dict->keys);
            kfree(dict->values);
            dict->keys = new_keys;
            dict->values = new_values;
        }
    }

    if (dict->count < dict->capacity) {
        dict->keys[dict->count] = key;
        dict->values[dict->count] = object;
        dict->count++;
    }
}

void NSMutableDictionary_removeObjectForKey(id self, SEL cmd, id key) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    if (!dict || !key) return;

    for (uint32_t i = 0; i < dict->count; i++) {
        if (dict->keys[i] == key) {
            // Shift remaining entries down
            for (uint32_t j = i; j < dict->count - 1; j++) {
                dict->keys[j] = dict->keys[j + 1];
                dict->values[j] = dict->values[j + 1];
            }
            dict->count--;
            return;
        }
    }
}

void NSMutableDictionary_removeAllObjects(id self, SEL cmd) {
    (void)cmd;
    CamelOSDictionary* dict = (CamelOSDictionary*)self;
    if (!dict) return;
    dict->count = 0;
}

static void register_NSMutableDictionary_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("setObject:forKey:");
    class_addMethod(cls, sel, (void*)NSMutableDictionary_setObjectForKey, "v@:@@");
    sel = sel_registerName("removeObjectForKey:");
    class_addMethod(cls, sel, (void*)NSMutableDictionary_removeObjectForKey, "v@:@");
    sel = sel_registerName("removeAllObjects");
    class_addMethod(cls, sel, (void*)NSMutableDictionary_removeAllObjects, "v@:");
}

// ============================================================================
// NSMutableString Implementation (inherits NSString, adds mutation)
// ============================================================================

Class NSMutableString_class = 0;

id NSMutableString_string(void) {
    CamelOSString* str = (CamelOSString*)class_createInstance(NSMutableString_class, 0);
    if (str) {
        str->capacity = 256;
        str->cstr = (char*)kmalloc(str->capacity);
        if (str->cstr) {
            str->cstr[0] = '\0';
        }
        str->length = 0;
        track_object((id)str);
    }
    return (id)str;
}

id NSMutableString_stringWithCString(const char* cstr) {
    CamelOSString* str = (CamelOSString*)class_createInstance(NSMutableString_class, 0);
    if (str && cstr) {
        uint32_t len = 0;
        while (cstr[len]) len++;
        str->capacity = len + 256;  // Extra room for mutations
        str->cstr = (char*)kmalloc(str->capacity);
        if (str->cstr) {
            memcpy(str->cstr, cstr, len + 1);
        }
        str->length = len;
        track_object((id)str);
    }
    return (id)str;
}

void NSMutableString_appendString(id self, SEL cmd, id other) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    if (!str || !other) return;

    // Get the C string from the other string
    CamelOSString* other_str = (CamelOSString*)other;
    if (!other_str || !other_str->cstr) return;

    uint32_t other_len = other_str->length;
    uint32_t needed = str->length + other_len + 1;

    // Grow buffer if needed
    if (needed > str->capacity) {
        str->capacity = needed + 256;
        char* new_cstr = (char*)kmalloc(str->capacity);
        if (new_cstr) {
            memcpy(new_cstr, str->cstr, str->length);
            new_cstr[str->length] = '\0';
            if (str->cstr) kfree(str->cstr);
            str->cstr = new_cstr;
        } else {
            return;
        }
    }

    memcpy(str->cstr + str->length, other_str->cstr, other_len);
    str->length += other_len;
    str->cstr[str->length] = '\0';
}

void NSMutableString_setString(id self, SEL cmd, const char* cstr) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    if (!str) return;

    if (!cstr) {
        str->length = 0;
        if (str->cstr) str->cstr[0] = '\0';
        return;
    }

    uint32_t len = 0;
    while (cstr[len]) len++;
    uint32_t needed = len + 1;

    if (needed > str->capacity) {
        str->capacity = needed + 256;
        if (str->cstr) kfree(str->cstr);
        str->cstr = (char*)kmalloc(str->capacity);
    }

    if (str->cstr) {
        memcpy(str->cstr, cstr, len + 1);
    }
    str->length = len;
}

void NSMutableString_deleteCharactersInRange(id self, SEL cmd, uint32_t loc, uint32_t len) {
    (void)cmd;
    CamelOSString* str = (CamelOSString*)self;
    if (!str || !str->cstr) return;
    if (loc + len > str->length) return;

    // Shift characters after the deleted range backward
    for (uint32_t i = loc; i + len < str->length; i++) {
        str->cstr[i] = str->cstr[i + len];
    }
    str->length -= len;
    str->cstr[str->length] = '\0';
}

static void register_NSMutableString_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("appendString:");
    class_addMethod(cls, sel, (void*)NSMutableString_appendString, "v@:@");
    sel = sel_registerName("setString:");
    class_addMethod(cls, sel, (void*)NSMutableString_setString, "v@:*");
    sel = sel_registerName("deleteCharactersInRange:");
    class_addMethod(cls, sel, (void*)NSMutableString_deleteCharactersInRange, "v@:II");
}
