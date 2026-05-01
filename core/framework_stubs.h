// core/framework_stubs.h - macOS Framework Stubs for CamelOS
// Provides stub implementations for CoreGraphics, CoreText, CFNetwork,
// Security, CoreAnimation, and other frameworks that macOS apps depend on
// These are registered with dyld as "virtual" libraries whose symbols
// are resolved to CamelOS kernel implementations
#ifndef FRAMEWORK_STUBS_H
#define FRAMEWORK_STUBS_H

#include "../include/types.h"
#include "objc_runtime.h"

// ============================================================================
// CoreGraphics (Quartz) - Maps to gfx_hal
// ============================================================================

// CGPoint
typedef struct { float x; float y; } CGPoint;

// CGSize
typedef struct { float width; float height; } CGSize;

// CGRect
typedef struct { CGPoint origin; CGSize size; } CGRect;

// CGAffineTransform
typedef struct {
    float a, b, c, d;
    float tx, ty;
} CGAffineTransform;

// CGContextRef - maps to CamelOS gfx context
typedef void* CGContextRef;

// CGColorSpaceRef
typedef void* CGColorSpaceRef;

// CGImageRef
typedef void* CGImageRef;

// CGPathRef
typedef void* CGPathRef;

// CGColorRef
typedef uint32_t CGColorRef;

// CGFontRef
typedef void* CGFontRef;

// Drawing operations (bridge to gfx_hal)
void CGContextFillRect(CGContextRef ctx, CGRect rect, uint32_t color);
void CGContextStrokeRect(CGContextRef ctx, CGRect rect, uint32_t color, float width);
void CGContextFillRoundRect(CGContextRef ctx, CGRect rect, float radius, uint32_t color);
void CGContextDrawText(CGContextRef ctx, float x, float y, const char* text, uint32_t color);
void CGContextDrawImage(CGContextRef ctx, CGRect rect, CGImageRef image);
void CGContextSetFillColor(CGContextRef ctx, uint32_t color);
void CGContextSetStrokeColor(CGContextRef ctx, uint32_t color);
void CGContextSetLineWidth(CGContextRef ctx, float width);
void CGContextDrawLine(CGContextRef ctx, float x1, float y1, float x2, float y2);

// Color creation
CGColorRef CGColorCreateGenericRGB(float r, float g, float b, float a);

// Rect helpers
CGRect CGRectMake(float x, float y, float w, float h);
int CGRectContainsPoint(CGRect rect, CGPoint point);
CGRect CGRectIntersection(CGRect r1, CGRect r2);

// ============================================================================
// CoreText - Maps to gfx_hal text rendering
// ============================================================================

typedef void* CTFontRef;
typedef void* CTLineRef;
typedef void* CTParagraphStyleRef;

CTFontRef CTFontCreateWithName(const char* name, float size, void* matrix);
float CTFontGetAscent(CTFontRef font);
float CTFontGetDescent(CTFontRef font);
float CTFontGetLeading(CTFontRef font);
float CTFontGetCapHeight(CTFontRef font);

// ============================================================================
// CFNetwork - Maps to CamelOS socket/http/tls
// ============================================================================

// Simple wrapper - uses the existing CamelOS network stack
// CFReadStreamRef / CFWriteStreamRef map to socket FDs
typedef void* CFReadStreamRef;
typedef void* CFWriteStreamRef;

void CFStreamCreatePairWithSocketToHost(const char* host, int port,
    CFReadStreamRef* readStream, CFWriteStreamRef* writeStream);

// ============================================================================
// Security Framework (Keychain) - Stub
// ============================================================================

// Minimal stubs - always returns "not found" / "not implemented"
int SecItemAdd(void* attributes, void* result);
int SecItemCopyMatching(void* query, void* result);
int SecItemDelete(void* query);
int SecItemUpdate(void* query, void* attributes);

// ============================================================================
// CoreAnimation - Maps to CamelOS animation system
// ============================================================================

typedef struct {
    struct objc_object isa;
    int x, y, w, h;
    float opacity;
    uint32_t bg_color;
    float corner_radius;
    int is_hidden;
    void* sublayers;
    void* superlayer;
} CamelOSCALayer;
extern Class CALayer_class;

// ============================================================================
// Initialization
// ============================================================================

// Initialize framework stubs and register symbols with dyld
void framework_stubs_init(void);

// Register framework symbols in the global symbol table
void framework_stubs_register_symbols(void);

#endif // FRAMEWORK_STUBS_H
