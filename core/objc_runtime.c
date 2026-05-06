// core/objc_runtime.c - Minimal Objective-C Runtime Implementation for CamelOS
// Provides message dispatch, class registration, and ObjC object support
// Compatible with ravynos SDK/KDK Objective-C binaries

#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"

// --- Runtime State ---

#define MAX_CLASSES      256
#define MAX_SELECTORS    1024
#define MAX_PROTOCOLS    64
#define MAX_INSTANCES    4096

// Registered classes
static Class g_classes[MAX_CLASSES];
static int g_class_count = 0;

// Registered selectors (interned)
static struct objc_selector g_selectors[MAX_SELECTORS];
static int g_selector_count = 0;

// Registered protocols
static Protocol g_protocols[MAX_PROTOCOLS];
static int g_protocol_count = 0;

// ObjC-Kernel bridge
static ObjCBridge g_bridge;

// Root class: NSObject equivalent
static struct objc_class g_root_class;
static struct objc_class g_root_metaclass;

// --- Initialization ---

static void init_root_class(void) {
    // Setup root metaclass
    memset(&g_root_metaclass, 0, sizeof(g_root_metaclass));
    g_root_metaclass.isa = &g_root_metaclass;  // Metaclass's isa points to itself
    g_root_metaclass.superclass = &g_root_class;  // Root metaclass's superclass is the root class (NOT itself — prevents infinite recursion in method lookup)
    strcpy(g_root_metaclass.name, "NSObject meta");
    g_root_metaclass.info = 0x02; // Meta-class flag
    
    // Setup root class (NSObject)
    memset(&g_root_class, 0, sizeof(g_root_class));
    g_root_class.isa = &g_root_metaclass;
    g_root_class.superclass = 0;  // No superclass - this is the root
    strcpy(g_root_class.name, "NSObject");
    g_root_class.instance_size = sizeof(struct objc_object);
    g_root_class.info = 0x01; // Regular class flag
    
    // Register root class
    g_classes[0] = &g_root_class;
    g_classes[1] = &g_root_metaclass;
    g_class_count = 2;
    
    s_printf("[ObjC] Root class NSObject initialized\n");
}

void objc_runtime_init(void) {
    memset(g_classes, 0, sizeof(g_classes));
    memset(g_selectors, 0, sizeof(g_selectors));
    memset(g_protocols, 0, sizeof(g_protocols));
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_class_count = 0;
    g_selector_count = 0;
    g_protocol_count = 0;
    
    init_root_class();
    
    // Register common selectors
    sel_registerName("alloc");
    sel_registerName("init");
    sel_registerName("dealloc");
    sel_registerName("retain");
    sel_registerName("release");
    sel_registerName("autorelease");
    sel_registerName("class");
    sel_registerName("superclass");
    sel_registerName("isEqual:");
    sel_registerName("hash");
    sel_registerName("description");
    sel_registerName("performSelector:");
    sel_registerName("respondsToSelector:");
    sel_registerName("conformsToProtocol:");
    sel_registerName("copy");
    sel_registerName("mutableCopy");
    
    s_printf("[ObjC] Runtime initialized with ");
    char buf[16];
    int_to_str(g_selector_count, buf);
    s_printf(buf);
    s_printf(" pre-registered selectors\n");
}

// --- Selector Operations ---

SEL sel_registerName(const char* name) {
    if (!name) return 0;
    
    // Check if already registered
    for (int i = 0; i < g_selector_count; i++) {
        if (strcmp(g_selectors[i].name, name) == 0) {
            return &g_selectors[i];
        }
    }
    
    // Register new selector
    if (g_selector_count >= MAX_SELECTORS) {
        s_printf("[ObjC] WARNING: Selector table full!\n");
        return 0;
    }
    
    struct objc_selector* sel = &g_selectors[g_selector_count++];
    strncpy(sel->name, name, 63);
    sel->name[63] = 0;
    sel->index = g_selector_count - 1;
    
    return sel;
}

SEL sel_getUid(const char* name) {
    return sel_registerName(name);  // Same as registerName in our impl
}

const char* sel_getName(SEL sel) {
    if (!sel) return "(null)";
    return ((struct objc_selector*)sel)->name;
}

BOOL sel_isEqual(SEL lhs, SEL rhs) {
    return lhs == rhs;  // Pointer comparison (selectors are interned)
}

// --- Class Operations ---

Class objc_getClass(const char* name) {
    if (!name) return 0;
    
    for (int i = 0; i < g_class_count; i++) {
        if (g_classes[i] && strcmp(g_classes[i]->name, name) == 0) {
            return g_classes[i];
        }
    }
    return 0;
}

Class objc_lookUpClass(const char* name) {
    return objc_getClass(name);
}

Class objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes) {
    if (!name) return 0;
    if (objc_getClass(name)) return 0;  // Already exists
    
    if (g_class_count + 2 > MAX_CLASSES) {
        s_printf("[ObjC] Class table full!\n");
        return 0;
    }
    
    // Allocate class and metaclass
    Class cls = (Class)kmalloc(sizeof(struct objc_class) + extraBytes);
    Class meta = (Class)kmalloc(sizeof(struct objc_class));
    
    if (!cls || !meta) {
        if (cls) kfree(cls);
        if (meta) kfree(meta);
        return 0;
    }
    
    memset(cls, 0, sizeof(struct objc_class) + extraBytes);
    memset(meta, 0, sizeof(struct objc_class));
    
    // Setup class
    cls->isa = meta;
    cls->superclass = superclass ? superclass : &g_root_class;
    strncpy(cls->name, name, 63);
    cls->info = 0x01; // Regular class
    cls->instance_size = superclass ? superclass->instance_size : sizeof(struct objc_object);
    
    // Setup metaclass
    meta->isa = &g_root_metaclass;
    meta->superclass = superclass ? superclass->isa : &g_root_metaclass;
    strncpy(meta->name, name, 57);  // Leave room for " meta" suffix (6 chars)
    meta->name[57] = 0;              // Ensure null termination
    strcat(meta->name, " meta");
    meta->info = 0x02; // Metaclass flag
    
    // Register
    g_classes[g_class_count++] = cls;
    g_classes[g_class_count++] = meta;
    
    return cls;
}

void objc_registerClassPair(Class cls) {
    if (!cls) return;  // Guard against NULL pointer
    // Already registered in allocateClassPair
    // This is a no-op in our implementation, but could do validation
    s_printf("[ObjC] Registered class: ");
    s_printf(cls->name);
    s_printf("\n");
}

// --- Method Operations ---

Method class_getInstanceMethod(Class cls, SEL name) {
    if (!cls || !name) return 0;
    
    // Guard against circular superclass chains (max depth 32)
    int depth = 0;
    Class cur = cls;
    while (cur && depth < 32) {
        Method m = cur->methods;
        while (m) {
            if (sel_isEqual(m->selector, name)) return m;
            m = m->next;
        }
        cur = cur->superclass;
        depth++;
    }
    
    return 0;
}

Method class_getClassMethod(Class cls, SEL name) {
    if (!cls || !cls->isa) return 0;
    return class_getInstanceMethod(cls->isa, name);
}

void* method_getImplementation(Method method) {
    if (!method) return 0;
    return method->imp;
}

BOOL class_addMethod(Class cls, SEL name, void* imp, const char* types) {
    if (!cls || !name || !imp) return NO;
    
    // Check if method already exists
    Method existing = class_getInstanceMethod(cls, name);
    if (existing) return NO;
    
    // Add new method
    Method m = (Method)kmalloc(sizeof(struct objc_method));
    if (!m) return NO;
    
    m->selector = name;
    m->imp = imp;
    if (types) strncpy(m->types, types, 31);
    m->next = cls->methods;
    cls->methods = m;
    
    return YES;
}

IMP class_replaceMethod(Class cls, SEL name, void* imp, const char* types) {
    if (!cls || !name || !imp) return 0;
    
    Method existing = class_getInstanceMethod(cls, name);
    if (existing) {
        void* old_imp = existing->imp;
        existing->imp = imp;
        if (types) strncpy(existing->types, types, 31);
        return old_imp;
    }
    
    // Add new method
    class_addMethod(cls, name, imp, types);
    return 0;
}

BOOL class_addIvar(Class cls, const char* name, size_t size, uint8_t alignment, const char* types) {
    if (!cls || !name) return NO;
    
    Ivar ivar = (Ivar)kmalloc(sizeof(struct objc_ivar));
    if (!ivar) return NO;
    
    strncpy(ivar->name, name, 63);
    if (types) strncpy(ivar->types, types, 31);
    ivar->offset = cls->instance_size;
    // Align
    if (alignment > 1) {
        ivar->offset = (ivar->offset + alignment - 1) & ~(alignment - 1);
    }
    ivar->next = cls->ivars;
    cls->ivars = ivar;
    cls->instance_size = ivar->offset + size;
    
    return YES;
}

Ivar class_getInstanceVariable(Class cls, const char* name) {
    if (!cls || !name) return 0;
    
    Ivar iv = cls->ivars;
    while (iv) {
        if (strcmp(iv->name, name) == 0) return iv;
        iv = iv->next;
    }
    return 0;
}

const char* ivar_getName(Ivar ivar) {
    if (!ivar) return 0;
    return ivar->name;
}

// --- Object Operations ---

id class_createInstance(Class cls, size_t extraBytes) {
    if (!cls) return 0;
    
    size_t size = cls->instance_size + extraBytes;
    id obj = (id)kmalloc(size);
    if (!obj) return 0;
    
    memset(obj, 0, size);
    obj->isa = cls;
    return obj;
}

void object_dispose(id obj) {
    if (obj) kfree(obj);
}

Class object_getClass(id obj) {
    if (!obj) return 0;
    return obj->isa;
}

const char* object_getClassName(id obj) {
    if (!obj || !obj->isa) return "(null)";
    return obj->isa->name;
}

// --- Message Sending ---

// Method lookup helper - called by assembly objc_msgSend
// Returns the IMP (function pointer) for the method, or NULL if not found
IMP objc_lookupMethod(Class cls, SEL op) {
    if (!cls || !op) return 0;
    
    Method method = class_getInstanceMethod(cls, op);
    if (method && method->imp) {
        return method->imp;
    }
    
    // Method not found
    s_printf("[ObjC] WARNING: Unrecognized selector ");
    s_printf(sel_getName(op));
    s_printf(" sent to class ");
    s_printf(cls->name);
    s_printf("\n");
    
    return 0;
}

// C fallback for objc_msgSend - used when assembly version is not available
// The assembly version in objc_msgSend.asm properly forwards variadic args
id objc_msgSend_c(id self, SEL op, ...) {
    if (!self || !op) return 0;
    
    Class cls = self->isa;
    IMP imp = objc_lookupMethod(cls, op);
    
    if (imp) {
        // Cast and call - simplified, real implementation needs va_args
        typedef id (*msg_impl)(id, SEL, ...);
        return ((msg_impl)imp)(self, op);
    }
    
    return 0;
}

// C fallback for objc_msgSendSuper
id objc_msgSendSuper_c(struct objc_super* super, SEL op, ...) {
    if (!super || !op) return 0;
    
    IMP imp = objc_lookupMethod(super->class, op);
    
    if (imp) {
        typedef id (*msg_impl)(id, SEL, ...);
        return ((msg_impl)imp)(super->receiver, op);
    }
    
    return 0;
}

// --- Protocol Operations ---

struct objc_protocol* objc_getProtocol(const char* name) {
    if (!name) return 0;
    
    for (int i = 0; i < g_protocol_count; i++) {
        if (strcmp(g_protocols[i]->name, name) == 0) {
            return g_protocols[i];
        }
    }
    return 0;
}

BOOL class_conformsToProtocol(Class cls, Protocol* protocol) {
    (void)cls;
    (void)protocol;
    // Simplified - always return NO for now
    return NO;
}

// --- Bridge Operations ---

void objc_set_bridge(ObjCBridge* bridge) {
    if (bridge) {
        memcpy(&g_bridge, bridge, sizeof(ObjCBridge));
    }
}

ObjCBridge* objc_get_bridge(void) {
    return &g_bridge;
}
