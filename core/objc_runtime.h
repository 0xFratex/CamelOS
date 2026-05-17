// core/objc_runtime.h - Minimal Objective-C Runtime for CamelOS
// Provides core ObjC functionality for macOS app compatibility
// Based on concepts from ravynos SDK/KDK and GNUstep runtime
#ifndef OBJC_RUNTIME_H
#define OBJC_RUNTIME_H

#include "../include/types.h"

// Forward declarations
typedef struct objc_class* Class;
typedef struct objc_object* id;
typedef struct objc_selector* SEL;
typedef struct objc_method* Method;
typedef struct objc_ivar* Ivar;
typedef struct objc_protocol* Protocol;

// ObjC BOOL type
typedef int BOOL;
#define YES  1
#define NO   0

// ObjC selector - interned string representation of method name
struct objc_selector {
    char name[64];      // Method name (e.g., "initWithFrame:")
    char types[32];     // Type encoding (simplified)
    uint32_t index;     // Selector index in table
};

// Instance variable
struct objc_ivar {
    char name[64];
    char types[32];
    int offset;
    Ivar next;
};

// Method
struct objc_method {
    SEL selector;
    char types[32];
    void* imp;          // Implementation (function pointer)
    Method next;        // Next method in list (for categories)
};

// Class structure - mirrors macOS objc_class layout
struct objc_class {
    Class isa;                  // Points to metaclass
    Class superclass;           // Parent class
    char name[64];              // Class name
    uint32_t version;
    uint32_t info;              // Class flags
    uint32_t instance_size;
    Ivar ivars;                 // Instance variables
    Method methods;             // Instance methods
    Method class_methods;       // Class methods (via metaclass)
    // CamelOS extensions
    void* reserved[4];
};

// Object - base type for all ObjC instances
struct objc_object {
    Class isa;                  // Class pointer
};

// Protocol
struct objc_protocol {
    char name[64];
    Method required_methods;
    Method optional_methods;
};

// objc_super - used by objc_msgSendSuper
struct objc_super {
    id receiver;        // The object that received the message
    Class class;        // The superclass to start searching from
};

// --- ObjC Message Sending ---
// The core of ObjC - message dispatch
id objc_msgSend(id self, SEL op, ...);
id objc_msgSendSuper(struct objc_super* super, SEL op, ...);

// --- Class Operations ---
Class objc_getClass(const char* name);
Class objc_lookUpClass(const char* name);
Class objc_getClassByIndex(int index);  // Iterate registered classes
Class objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes);
void objc_registerClassPair(Class cls);

// --- Method Operations ---
SEL sel_registerName(const char* name);
SEL sel_getUid(const char* name);
const char* sel_getName(SEL sel);
BOOL sel_isEqual(SEL lhs, SEL rhs);

typedef void* IMP;

Method class_getInstanceMethod(Class cls, SEL name);
Method class_getClassMethod(Class cls, SEL name);
void* method_getImplementation(Method method);
IMP class_replaceMethod(Class cls, SEL name, void* imp, const char* types);

BOOL class_addMethod(Class cls, SEL name, void* imp, const char* types);
BOOL class_addIvar(Class cls, const char* name, size_t size, uint8_t alignment, const char* types);

// --- Ivar Operations ---
Ivar class_getInstanceVariable(Class cls, const char* name);
const char* ivar_getName(Ivar ivar);

// --- Object Operations ---
id class_createInstance(Class cls, size_t extraBytes);
void object_dispose(id obj);
Class object_getClass(id obj);
const char* object_getClassName(id obj);

IMP objc_lookupMethod(Class cls, SEL op);   // Called by assembly objc_msgSend
id objc_msgSend_c(id self, SEL op, ...);    // C fallback (assembly version preferred)
id objc_msgSendSuper_c(struct objc_super* super, SEL op, ...);  // C fallback

// --- Runtime Initialization ---
void objc_runtime_init(void);

// --- CamelOS-specific: Bridge to kernel API ---
// Allows ObjC code to call kernel services
typedef struct {
    void* kernel_api;           // Pointer to kernel_api_t
    void* foundation_classes;   // Pointer to Foundation class registry
} ObjCBridge;

void objc_set_bridge(ObjCBridge* bridge);
ObjCBridge* objc_get_bridge(void);

// --- Protocol Operations ---
struct objc_protocol* objc_getProtocol(const char* name);
BOOL class_conformsToProtocol(Class cls, Protocol* protocol);

#endif // OBJC_RUNTIME_H
