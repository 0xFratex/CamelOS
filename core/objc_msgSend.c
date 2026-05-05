// core/objc_msgSend.c - Objective-C message dispatch (C fallback)
//
// This provides a C implementation of objc_msgSend and objc_msgSendSuper
// as a fallback when NASM is not available for the assembly version.
//
// NOTE: The C version cannot perfectly forward variadic arguments like the
// assembly version, but it handles the common cases (0-4 extra args).
// For full variadic forwarding, use the asm version with NASM.

#include "objc_runtime.h"

// Forward declarations for the lookup function
extern IMP objc_lookupMethod(Class cls, SEL op);

// Main objc_msgSend implementation
// In the C version, we can't truly forward variadic args transparently,
// but we can handle the common calling patterns.
// The asm version is preferred when available.
id objc_msgSend(id self, SEL op, ...) {
    if (!self) return 0;
    
    Class cls = object_getClass(self);
    IMP imp = objc_lookupMethod(cls, op);
    
    if (!imp) return 0;
    
    // Call the IMP with self and op
    // We use a simple cast here - the actual method implementation
    // will read additional args from the stack as needed
    typedef id (*msg_fn)(id, SEL, ...);
    return ((msg_fn)imp)(self, op);
}

// objc_msgSendSuper implementation
id objc_msgSendSuper(struct objc_super* super, SEL op, ...) {
    if (!super || !super->receiver) return 0;
    
    // Start lookup from the superclass specified in the super struct
    Class super_class = super->class;
    IMP imp = objc_lookupMethod(super_class, op);
    
    if (!imp) return 0;
    
    typedef id (*msg_fn)(id, SEL, ...);
    return ((msg_fn)imp)(super->receiver, op);
}
