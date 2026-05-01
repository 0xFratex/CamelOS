// hal/video/cgcontext.h - CoreGraphics-like 2D Drawing Context for CamelOS
// Provides CGContext, CGFont, and CGImage types for advanced 2D drawing
#ifndef CGCONTEXT_H
#define CGCONTEXT_H

#include "../../include/types.h"

// ============================================================================
// Path Point Types
// ============================================================================
#define CG_PATH_MOVE       0
#define CG_PATH_LINE       1
#define CG_PATH_CURVE_CP1  2   // First control point of cubic bezier
#define CG_PATH_CURVE_CP2  3   // Second control point of cubic bezier
#define CG_PATH_CURVE_END  4   // End point of cubic bezier
#define CG_PATH_CLOSE      5

// ============================================================================
// Maximum path points
// ============================================================================
#define CG_MAX_PATH_POINTS 4096

// ============================================================================
// Forward declarations
// ============================================================================
typedef struct CGContext* CGContextRef;
typedef struct CGFont* CGFontRef;
typedef struct CGImage* CGImageRef;

// ============================================================================
// Path Point
// ============================================================================
typedef struct {
    float x, y;
    int type;       // CG_PATH_*
} CGPathPoint;

// ============================================================================
// Graphics Context State (for save/restore)
// ============================================================================
typedef struct {
    uint32_t fill_color;
    uint32_t stroke_color;
    uint32_t text_color;
    float line_width;
    float alpha;
    float font_size;
    float shadow_offset_x;
    float shadow_offset_y;
    float shadow_radius;
    uint32_t shadow_color;
    int clip_x, clip_y, clip_w, clip_h;
    int has_clip;
} CGContextState;

// ============================================================================
// CGContext - 2D Drawing Context
// ============================================================================
struct CGContext {
    int origin_x, origin_y;     // Translation origin
    int width, height;          // Context size
    
    // Current drawing state
    uint32_t fill_color;
    uint32_t stroke_color;
    uint32_t text_color;
    float line_width;
    float alpha;
    float font_size;
    CGFontRef font;
    
    // Shadow
    float shadow_offset_x;
    float shadow_offset_y;
    float shadow_radius;
    uint32_t shadow_color;
    
    // Clipping
    int clip_x, clip_y, clip_w, clip_h;
    int has_clip;
    
    // Path
    CGPathPoint path_points[CG_MAX_PATH_POINTS];
    int path_point_count;
    int path_subpath_start;
};

// ============================================================================
// CGFont - Font Object
// ============================================================================
struct CGFont {
    char name[64];
    float size;
    int is_bold;
    int is_italic;
    void* glyph_data;       // TrueType glyph data (stb_truetype style)
    int ascent;
    int descent;
    int line_height;
};

// ============================================================================
// CGImage - Image Object
// ============================================================================
struct CGImage {
    int width;
    int height;
    int bpp;
    uint32_t* pixel_data;   // ARGB pixel data
    int owns_data;          // Whether to free pixel_data on destroy
};

// ============================================================================
// CGContext Creation / Destruction
// ============================================================================
CGContextRef CGContextCreate(int x, int y, int width, int height);
void CGContextDestroy(CGContextRef ctx);

// ============================================================================
// State Management
// ============================================================================
void CGContextSaveState(CGContextRef ctx);
void CGContextRestoreState(CGContextRef ctx);

// ============================================================================
// Color and Style
// ============================================================================
void CGContextSetFillColor(CGContextRef ctx, uint32_t color);
void CGContextSetStrokeColor(CGContextRef ctx, uint32_t color);
void CGContextSetTextColor(CGContextRef ctx, uint32_t color);
void CGContextSetLineWidth(CGContextRef ctx, float width);
void CGContextSetAlpha(CGContextRef ctx, float alpha);
void CGContextSetFontSize(CGContextRef ctx, float size);
void CGContextSetFont(CGContextRef ctx, CGFontRef font);
void CGContextSetShadow(CGContextRef ctx, float offset_x, float offset_y, 
                         float radius, uint32_t color);
void CGContextClearShadow(CGContextRef ctx);

// ============================================================================
// Clipping
// ============================================================================
void CGContextClipToRect(CGContextRef ctx, int x, int y, int w, int h);
void CGContextResetClip(CGContextRef ctx);

// ============================================================================
// Path Construction
// ============================================================================
void CGContextMoveToPoint(CGContextRef ctx, float x, float y);
void CGContextAddLineToPoint(CGContextRef ctx, float x, float y);
void CGContextAddCurveToPoint(CGContextRef ctx, float cp1x, float cp1y,
                               float cp2x, float cp2y, float x, float y);
void CGContextAddQuadCurveToPoint(CGContextRef ctx, float cpx, float cpy,
                                   float x, float y);
void CGContextClosePath(CGContextRef ctx);
void CGContextAddRect(CGContextRef ctx, int x, int y, int w, int h);
void CGContextAddRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius);

// ============================================================================
// Path Drawing
// ============================================================================
void CGContextFillPath(CGContextRef ctx);
void CGContextStrokePath(CGContextRef ctx);

// ============================================================================
// Convenience Drawing Functions
// ============================================================================
void CGContextFillRect(CGContextRef ctx, int x, int y, int w, int h);
void CGContextStrokeRect(CGContextRef ctx, int x, int y, int w, int h);
void CGContextFillRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius);
void CGContextStrokeRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius);
void CGContextDrawLine(CGContextRef ctx, int x0, int y0, int x1, int y1);

// ============================================================================
// Text Drawing
// ============================================================================
void CGContextDrawString(CGContextRef ctx, int x, int y, const char* str);
void CGContextDrawStringCentered(CGContextRef ctx, int cx, int y, const char* str);

// ============================================================================
// Gradient Drawing
// ============================================================================
void CGContextDrawLinearGradient(CGContextRef ctx, int x, int y, int w, int h,
                                  uint32_t start_color, uint32_t end_color,
                                  int vertical);

// ============================================================================
// Image Drawing
// ============================================================================
void CGContextDrawImage(CGContextRef ctx, int x, int y, CGImageRef image);
void CGContextDrawImageScaled(CGContextRef ctx, int x, int y, 
                               int dest_w, int dest_h, CGImageRef image);

// ============================================================================
// Ellipse / Circle Drawing
// ============================================================================
void CGContextFillEllipse(CGContextRef ctx, int x, int y, int w, int h);
void CGContextStrokeEllipse(CGContextRef ctx, int x, int y, int w, int h);
void CGContextFillCircle(CGContextRef ctx, int cx, int cy, int radius);
void CGContextStrokeCircle(CGContextRef ctx, int cx, int cy, int radius);

// ============================================================================
// CGFont Functions
// ============================================================================
CGFontRef CGFontCreate(const char* name);
void CGFontDestroy(CGFontRef font);
void CGFontDrawString(CGFontRef font, const char* str, int x, int y, 
                       float size, uint32_t color);
int CGFontMeasureString(CGFontRef font, const char* str, float size);

// ============================================================================
// CGImage Functions
// ============================================================================
CGImageRef CGImageCreate(int width, int height);
CGImageRef CGImageCreateWithData(int width, int height, uint32_t* data);
void CGImageDestroy(CGImageRef image);
CGImageRef CGImageLoadPNG(const char* path);
CGImageRef CGImageLoadJPEG(const char* path);

#endif // CGCONTEXT_H
