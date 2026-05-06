// core/framework_stubs.c - macOS Framework Stubs Implementation
// Bridges CoreGraphics, CoreText, CFNetwork, Security, CoreAnimation
// to CamelOS kernel implementations

#include "framework_stubs.h"
#include "dyld.h"
#include "objc_runtime.h"
#include "string.h"
#include "memory.h"
#include "../sys/api.h"
#include "../sys/cdl_defs.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/drivers/serial.h"

// Kernel API
extern kernel_api_t g_kernel_api;

// ============================================================================
// CoreGraphics Implementation
// ============================================================================

CGRect CGRectMake(float x, float y, float w, float h) {
    CGRect r;
    r.origin.x = x; r.origin.y = y;
    r.size.width = w; r.size.height = h;
    return r;
}

int CGRectContainsPoint(CGRect rect, CGPoint point) {
    return (point.x >= rect.origin.x &&
            point.x < rect.origin.x + rect.size.width &&
            point.y >= rect.origin.y &&
            point.y < rect.origin.y + rect.size.height);
}

CGRect CGRectIntersection(CGRect r1, CGRect r2) {
    CGRect result;
    float x1 = r1.origin.x > r2.origin.x ? r1.origin.x : r2.origin.x;
    float y1 = r1.origin.y > r2.origin.y ? r1.origin.y : r2.origin.y;
    float x2 = (r1.origin.x + r1.size.width) < (r2.origin.x + r2.size.width)
               ? (r1.origin.x + r1.size.width) : (r2.origin.x + r2.size.width);
    float y2 = (r1.origin.y + r1.size.height) < (r2.origin.y + r2.size.height)
               ? (r1.origin.y + r1.size.height) : (r2.origin.y + r2.size.height);

    if (x2 > x1 && y2 > y1) {
        result.origin.x = x1; result.origin.y = y1;
        result.size.width = x2 - x1; result.size.height = y2 - y1;
    } else {
        result.origin.x = 0; result.origin.y = 0;
        result.size.width = 0; result.size.height = 0;
    }
    return result;
}

CGColorRef CGColorCreateGenericRGB(float r, float g, float b, float a) {
    return (CGColorRef)((uint32_t)(a * 255) << 24 |
                        (uint32_t)(b * 255) << 16 |
                        (uint32_t)(g * 255) << 8 |
                        (uint32_t)(r * 255));
}

// NOTE: CGContextFillRect, CGContextStrokeRect, CGContextDrawImage,
// CGContextSetFillColor, CGContextSetStrokeColor, CGContextSetLineWidth,
// and CGContextDrawLine are now properly implemented in hal/video/cgcontext.c.
// The stub versions have been removed to avoid multiple definition errors.

void CGContextFillRoundRect(CGContextRef ctx, CGRect rect, float radius, uint32_t color) {
    (void)ctx;
    gfx_fill_rounded_rect((int)rect.origin.x, (int)rect.origin.y,
                          (int)rect.size.width, (int)rect.size.height,
                          color, (int)radius);
}

void CGContextDrawText(CGContextRef ctx, float x, float y, const char* text, uint32_t color) {
    (void)ctx;
    gfx_draw_string((int)x, (int)y, text, color);
}

// ============================================================================
// CoreText Implementation
// ============================================================================

CTFontRef CTFontCreateWithName(const char* name, float size, void* matrix) {
    (void)name; (void)matrix;
    // Return a simple font descriptor (just the size)
    uint32_t* font = (uint32_t*)kmalloc(8);
    if (font) {
        font[0] = (uint32_t)size;
        font[1] = 0;
    }
    return (CTFontRef)font;
}

float CTFontGetAscent(CTFontRef font) {
    (void)font;
    return 10.0f;  // Approximate
}

float CTFontGetDescent(CTFontRef font) {
    (void)font;
    return 3.0f;
}

float CTFontGetLeading(CTFontRef font) {
    (void)font;
    return 2.0f;
}

float CTFontGetCapHeight(CTFontRef font) {
    (void)font;
    return 8.0f;
}

// ============================================================================
// CFNetwork Implementation
// ============================================================================

void CFStreamCreatePairWithSocketToHost(const char* host, int port,
    CFReadStreamRef* readStream, CFWriteStreamRef* writeStream) {
    // Create a socket pair using the kernel API
    int sock = g_kernel_api.socket(2, 1, 0);  // AF_INET, SOCK_STREAM

    if (sock >= 0) {
        // Resolve hostname
        char ip_addr[16];
        g_kernel_api.dns_resolve(host, ip_addr, sizeof(ip_addr));

        // Connect
        struct { uint16_t family; uint16_t port; uint32_t addr; char zero[8]; } addr;
        addr.family = 2;
        addr.port = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);
        addr.addr = 0;  // TODO: Parse ip_addr
        g_kernel_api.connect(sock, &addr, sizeof(addr));
    }

    if (readStream) *readStream = (CFReadStreamRef)(sock >= 0 ? sock : 0);
    if (writeStream) *writeStream = (CFWriteStreamRef)(sock >= 0 ? sock : 0);
}

// ============================================================================
// Security Framework Stubs
// ============================================================================

int SecItemAdd(void* attributes, void* result) {
    (void)attributes; (void)result;
    return -25299;  // errSecNotAvailable
}

int SecItemCopyMatching(void* query, void* result) {
    (void)query; (void)result;
    return -25300;  // errSecItemNotFound
}

int SecItemDelete(void* query) {
    (void)query;
    return -25300;  // errSecItemNotFound
}

int SecItemUpdate(void* query, void* attributes) {
    (void)query; (void)attributes;
    return -25300;  // errSecItemNotFound
}

// ============================================================================
// CoreAnimation (CALayer) Implementation
// ============================================================================

Class CALayer_class = 0;

static id CALayer_init(id self, SEL cmd) {
    (void)cmd;
    CamelOSCALayer* layer = (CamelOSCALayer*)self;
    if (layer) {
        layer->opacity = 1.0f;
        layer->bg_color = 0;
        layer->corner_radius = 0;
        layer->is_hidden = 0;
        layer->sublayers = 0;
        layer->superlayer = 0;
    }
    return self;
}

static void CALayer_setFrame(id self, SEL cmd, int x, int y, int w, int h) {
    (void)cmd;
    CamelOSCALayer* layer = (CamelOSCALayer*)self;
    if (layer) {
        layer->x = x; layer->y = y; layer->w = w; layer->h = h;
    }
}

static void CALayer_setBackgroundColor(id self, SEL cmd, uint32_t color) {
    (void)cmd;
    CamelOSCALayer* layer = (CamelOSCALayer*)self;
    if (layer) layer->bg_color = color;
}

static void CALayer_setOpacity(id self, SEL cmd, float opacity) {
    (void)cmd;
    CamelOSCALayer* layer = (CamelOSCALayer*)self;
    if (layer) layer->opacity = opacity;
}

static void CALayer_setCornerRadius(id self, SEL cmd, float radius) {
    (void)cmd;
    CamelOSCALayer* layer = (CamelOSCALayer*)self;
    if (layer) layer->corner_radius = radius;
}

static void register_CALayer_methods(Class cls) {
    SEL sel;
    sel = sel_registerName("init");
    class_addMethod(cls, sel, (void*)CALayer_init, "@@:");
    sel = sel_registerName("setFrame:");
    class_addMethod(cls, sel, (void*)CALayer_setFrame, "v@:iiii");
    sel = sel_registerName("setBackgroundColor:");
    class_addMethod(cls, sel, (void*)CALayer_setBackgroundColor, "v@:I");
    sel = sel_registerName("setOpacity:");
    class_addMethod(cls, sel, (void*)CALayer_setOpacity, "v@:f");
    sel = sel_registerName("setCornerRadius:");
    class_addMethod(cls, sel, (void*)CALayer_setCornerRadius, "v@:f");
}

// ============================================================================
// Framework Symbol Registration
// ============================================================================

void framework_stubs_register_symbols(void) {
    // Register CoreGraphics symbols
    dyld_register_global_symbol("CGRectMake", (void*)CGRectMake, 0);
    dyld_register_global_symbol("CGRectContainsPoint", (void*)CGRectContainsPoint, 0);
    dyld_register_global_symbol("CGColorCreateGenericRGB", (void*)CGColorCreateGenericRGB, 0);
    dyld_register_global_symbol("CGContextFillRect", (void*)CGContextFillRect, 0);
    dyld_register_global_symbol("CGContextStrokeRect", (void*)CGContextStrokeRect, 0);
    dyld_register_global_symbol("CGContextFillRoundRect", (void*)CGContextFillRoundRect, 0);
    dyld_register_global_symbol("CGContextDrawText", (void*)CGContextDrawText, 0);

    // Register CoreText symbols
    dyld_register_global_symbol("CTFontCreateWithName", (void*)CTFontCreateWithName, 0);
    dyld_register_global_symbol("CTFontGetAscent", (void*)CTFontGetAscent, 0);
    dyld_register_global_symbol("CTFontGetDescent", (void*)CTFontGetDescent, 0);

    // Register Security framework stubs
    dyld_register_global_symbol("SecItemAdd", (void*)SecItemAdd, 0);
    dyld_register_global_symbol("SecItemCopyMatching", (void*)SecItemCopyMatching, 0);
    dyld_register_global_symbol("SecItemDelete", (void*)SecItemDelete, 0);
    dyld_register_global_symbol("SecItemUpdate", (void*)SecItemUpdate, 0);

    // Register CFNetwork
    dyld_register_global_symbol("CFStreamCreatePairWithSocketToHost",
                                (void*)CFStreamCreatePairWithSocketToHost, 0);
}

// ============================================================================
// Framework Stubs Initialization
// ============================================================================

void framework_stubs_init(void) {
    s_printf("[Frameworks] Initializing framework stubs...\n");

    // Create CALayer class
    CALayer_class = objc_allocateClassPair(objc_getClass("NSObject"), "CALayer",
        sizeof(CamelOSCALayer) - sizeof(struct objc_object));
    if (CALayer_class) {
        register_CALayer_methods(CALayer_class);
        objc_registerClassPair(CALayer_class);
    }

    // Register framework symbols in the global symbol table
    framework_stubs_register_symbols();

    s_printf("[Frameworks] Framework stubs initialized\n");
}
