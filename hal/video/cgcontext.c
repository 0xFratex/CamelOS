// hal/video/cgcontext.c - CoreGraphics-like 2D Drawing Context for CamelOS
// Provides a CGContext implementation that bridges CoreGraphics calls to gfx_hal
// Features: Path drawing, fills, strokes, text rendering, image compositing,
//           bezier curves, gradient fills, and TrueType font support

#include "gfx_hal.h"
#include "cgcontext.h"
#include "../../core/memory.h"
#include "../../core/string.h"

// ============================================================================
// CGContext Creation and Management
// ============================================================================

CGContextRef CGContextCreate(int x, int y, int width, int height) {
    CGContextRef ctx = (CGContextRef)kmalloc(sizeof(CGContext));
    if (!ctx) return NULL;
    
    memset(ctx, 0, sizeof(CGContext));
    ctx->origin_x = x;
    ctx->origin_y = y;
    ctx->width = width;
    ctx->height = height;
    ctx->fill_color = 0xFF000000;       // Black
    ctx->stroke_color = 0xFF000000;     // Black
    ctx->text_color = 0xFF333333;       // Dark gray
    ctx->line_width = 1.0f;
    ctx->alpha = 1.0f;
    ctx->font_size = 13.0f;
    ctx->font = NULL;
    ctx->path_point_count = 0;
    ctx->path_subpath_start = 0;
    ctx->clip_x = x;
    ctx->clip_y = y;
    ctx->clip_w = width;
    ctx->clip_h = height;
    ctx->has_clip = 0;
    ctx->shadow_offset_x = 0;
    ctx->shadow_offset_y = 0;
    ctx->shadow_radius = 0;
    ctx->shadow_color = 0x00000000;     // Transparent = no shadow
    
    return ctx;
}

void CGContextDestroy(CGContextRef ctx) {
    if (ctx) kfree(ctx);
}

// ============================================================================
// State Management (Save/Restore)
// ============================================================================

#define CG_STATE_STACK_SIZE 16

static CGContextState g_state_stack[CG_STATE_STACK_SIZE];
static int g_state_stack_top = 0;

void CGContextSaveState(CGContextRef ctx) {
    if (!ctx || g_state_stack_top >= CG_STATE_STACK_SIZE) return;
    
    CGContextState* state = &g_state_stack[g_state_stack_top++];
    state->fill_color = ctx->fill_color;
    state->stroke_color = ctx->stroke_color;
    state->text_color = ctx->text_color;
    state->line_width = ctx->line_width;
    state->alpha = ctx->alpha;
    state->font_size = ctx->font_size;
    state->shadow_offset_x = ctx->shadow_offset_x;
    state->shadow_offset_y = ctx->shadow_offset_y;
    state->shadow_radius = ctx->shadow_radius;
    state->shadow_color = ctx->shadow_color;
    state->clip_x = ctx->clip_x;
    state->clip_y = ctx->clip_y;
    state->clip_w = ctx->clip_w;
    state->clip_h = ctx->clip_h;
    state->has_clip = ctx->has_clip;
}

void CGContextRestoreState(CGContextRef ctx) {
    if (!ctx || g_state_stack_top <= 0) return;
    
    CGContextState* state = &g_state_stack[--g_state_stack_top];
    ctx->fill_color = state->fill_color;
    ctx->stroke_color = state->stroke_color;
    ctx->text_color = state->text_color;
    ctx->line_width = state->line_width;
    ctx->alpha = state->alpha;
    ctx->font_size = state->font_size;
    ctx->shadow_offset_x = state->shadow_offset_x;
    ctx->shadow_offset_y = state->shadow_offset_y;
    ctx->shadow_radius = state->shadow_radius;
    ctx->shadow_color = state->shadow_color;
    ctx->clip_x = state->clip_x;
    ctx->clip_y = state->clip_y;
    ctx->clip_w = state->clip_w;
    ctx->clip_h = state->clip_h;
    ctx->has_clip = state->has_clip;
    
    // Apply clip to gfx_hal
    if (ctx->has_clip) {
        gfx_set_clip(ctx->clip_x, ctx->clip_y, ctx->clip_w, ctx->clip_h);
    } else {
        gfx_reset_clip();
    }
}

// ============================================================================
// Color and Style Settings
// ============================================================================

void CGContextSetFillColor(CGContextRef ctx, uint32_t color) {
    if (ctx) ctx->fill_color = color;
}

void CGContextSetStrokeColor(CGContextRef ctx, uint32_t color) {
    if (ctx) ctx->stroke_color = color;
}

void CGContextSetTextColor(CGContextRef ctx, uint32_t color) {
    if (ctx) ctx->text_color = color;
}

void CGContextSetLineWidth(CGContextRef ctx, float width) {
    if (ctx) ctx->line_width = width > 0 ? width : 1.0f;
}

void CGContextSetAlpha(CGContextRef ctx, float alpha) {
    if (ctx) {
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        ctx->alpha = alpha;
    }
}

void CGContextSetFontSize(CGContextRef ctx, float size) {
    if (ctx) ctx->font_size = size > 0 ? size : 13.0f;
}

void CGContextSetFont(CGContextRef ctx, CGFontRef font) {
    if (ctx) ctx->font = font;
}

void CGContextSetShadow(CGContextRef ctx, float offset_x, float offset_y, 
                         float radius, uint32_t color) {
    if (!ctx) return;
    ctx->shadow_offset_x = offset_x;
    ctx->shadow_offset_y = offset_y;
    ctx->shadow_radius = radius;
    ctx->shadow_color = color;
}

void CGContextClearShadow(CGContextRef ctx) {
    if (!ctx) return;
    ctx->shadow_offset_x = 0;
    ctx->shadow_offset_y = 0;
    ctx->shadow_radius = 0;
    ctx->shadow_color = 0x00000000;
}

// ============================================================================
// Clipping
// ============================================================================

void CGContextClipToRect(CGContextRef ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    ctx->clip_x = ctx->origin_x + x;
    ctx->clip_y = ctx->origin_y + y;
    ctx->clip_w = w;
    ctx->clip_h = h;
    ctx->has_clip = 1;
    gfx_set_clip(ctx->clip_x, ctx->clip_y, ctx->clip_w, ctx->clip_h);
}

void CGContextResetClip(CGContextRef ctx) {
    if (!ctx) return;
    ctx->has_clip = 0;
    gfx_reset_clip();
}

// ============================================================================
// Path Operations
// ============================================================================

void CGContextMoveToPoint(CGContextRef ctx, float x, float y) {
    if (!ctx || ctx->path_point_count >= CG_MAX_PATH_POINTS) return;
    
    ctx->path_points[ctx->path_point_count].x = ctx->origin_x + x;
    ctx->path_points[ctx->path_point_count].y = ctx->origin_y + y;
    ctx->path_points[ctx->path_point_count].type = CG_PATH_MOVE;
    ctx->path_subpath_start = ctx->path_point_count;
    ctx->path_point_count++;
}

void CGContextAddLineToPoint(CGContextRef ctx, float x, float y) {
    if (!ctx || ctx->path_point_count >= CG_MAX_PATH_POINTS) return;
    if (ctx->path_point_count == 0) {
        CGContextMoveToPoint(ctx, x, y);
        return;
    }
    
    ctx->path_points[ctx->path_point_count].x = ctx->origin_x + x;
    ctx->path_points[ctx->path_point_count].y = ctx->origin_y + y;
    ctx->path_points[ctx->path_point_count].type = CG_PATH_LINE;
    ctx->path_point_count++;
}

void CGContextAddCurveToPoint(CGContextRef ctx, float cp1x, float cp1y,
                               float cp2x, float cp2y, float x, float y) {
    if (!ctx || ctx->path_point_count + 2 >= CG_MAX_PATH_POINTS) return;
    
    // Store cubic bezier as: control1, control2, endpoint
    ctx->path_points[ctx->path_point_count].x = ctx->origin_x + cp1x;
    ctx->path_points[ctx->path_point_count].y = ctx->origin_y + cp1y;
    ctx->path_points[ctx->path_point_count].type = CG_PATH_CURVE_CP1;
    ctx->path_point_count++;
    
    ctx->path_points[ctx->path_point_count].x = ctx->origin_x + cp2x;
    ctx->path_points[ctx->path_point_count].y = ctx->origin_y + cp2y;
    ctx->path_points[ctx->path_point_count].type = CG_PATH_CURVE_CP2;
    ctx->path_point_count++;
    
    ctx->path_points[ctx->path_point_count].x = ctx->origin_x + x;
    ctx->path_points[ctx->path_point_count].y = ctx->origin_y + y;
    ctx->path_points[ctx->path_point_count].type = CG_PATH_CURVE_END;
    ctx->path_point_count++;
}

void CGContextAddQuadCurveToPoint(CGContextRef ctx, float cpx, float cpy,
                                   float x, float y) {
    // Convert quadratic to cubic bezier
    // CP1 = Q0 + 2/3 * (QP - Q0)
    // CP2 = Q1 + 2/3 * (QP - Q1)
    float last_x = 0, last_y = 0;
    if (ctx->path_point_count > 0) {
        last_x = ctx->path_points[ctx->path_point_count - 1].x - ctx->origin_x;
        last_y = ctx->path_points[ctx->path_point_count - 1].y - ctx->origin_y;
    }
    
    float cp1x = last_x + (2.0f/3.0f) * (cpx - last_x);
    float cp1y = last_y + (2.0f/3.0f) * (cpy - last_y);
    float cp2x = x + (2.0f/3.0f) * (cpx - x);
    float cp2y = y + (2.0f/3.0f) * (cpy - y);
    
    CGContextAddCurveToPoint(ctx, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CGContextClosePath(CGContextRef ctx) {
    if (!ctx || ctx->path_point_count == 0) return;
    
    // Line back to subpath start
    float sx = ctx->path_points[ctx->path_subpath_start].x;
    float sy = ctx->path_points[ctx->path_subpath_start].y;
    
    if (ctx->path_point_count < CG_MAX_PATH_POINTS) {
        ctx->path_points[ctx->path_point_count].x = sx;
        ctx->path_points[ctx->path_point_count].y = sy;
        ctx->path_points[ctx->path_point_count].type = CG_PATH_CLOSE;
        ctx->path_point_count++;
    }
}

void CGContextAddRect(CGContextRef ctx, int x, int y, int w, int h) {
    CGContextMoveToPoint(ctx, x, y);
    CGContextAddLineToPoint(ctx, x + w, y);
    CGContextAddLineToPoint(ctx, x + w, y + h);
    CGContextAddLineToPoint(ctx, x, y + h);
    CGContextClosePath(ctx);
}

void CGContextAddRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius) {
    if (!ctx) return;
    
    // Clamp radius
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 1) {
        CGContextAddRect(ctx, x, y, w, h);
        return;
    }
    
    // Draw rounded rectangle using bezier curves for corners
    float k = radius * 0.5523f;  // Magic number for quarter-circle bezier approximation
    
    CGContextMoveToPoint(ctx, x + radius, y);
    CGContextAddLineToPoint(ctx, x + w - radius, y);
    CGContextAddCurveToPoint(ctx, x + w - radius + k, y, x + w, y + radius - k, x + w, y + radius);
    CGContextAddLineToPoint(ctx, x + w, y + h - radius);
    CGContextAddCurveToPoint(ctx, x + w, y + h - radius + k, x + w - radius + k, y + h, x + w - radius, y + h);
    CGContextAddLineToPoint(ctx, x + radius, y + h);
    CGContextAddCurveToPoint(ctx, x + radius - k, y + h, x, y + h - radius + k, x, y + h - radius);
    CGContextAddLineToPoint(ctx, x, y + radius);
    CGContextAddCurveToPoint(ctx, x, y + radius - k, x + radius - k, y, x + radius, y);
    CGContextClosePath(ctx);
}

// ============================================================================
// Path Drawing (Fill / Stroke)
// ============================================================================

// Forward declaration for scanline fill
static void _scanline_fill_path(CGContextRef ctx, int offset_x, int offset_y);

// Evaluate a cubic bezier at parameter t
static void bezier_point(float x0, float y0, float x1, float y1,
                          float x2, float y2, float x3, float y3,
                          float t, float* out_x, float* out_y) {
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;
    
    *out_x = uuu * x0 + 3.0f * uu * t * x1 + 3.0f * u * tt * x2 + ttt * x3;
    *out_y = uuu * y0 + 3.0f * uu * t * y1 + 3.0f * u * tt * y2 + ttt * y3;
}

// Fill the current path using scanline rendering
void CGContextFillPath(CGContextRef ctx) {
    if (!ctx || ctx->path_point_count == 0) return;
    
    // Apply shadow if needed (draw shadow before fill)
    if (ctx->shadow_color != 0x00000000 && ctx->shadow_radius > 0) {
        uint32_t saved_fill = ctx->fill_color;
        ctx->fill_color = ctx->shadow_color;
        // Scanline fill with shadow offset
        int sx = ctx->origin_x + (int)ctx->shadow_offset_x;
        int sy = ctx->origin_y + (int)ctx->shadow_offset_y;
        _scanline_fill_path(ctx, sx, sy);
        ctx->fill_color = saved_fill;
    }
    
    // Scanline fill the main path
    _scanline_fill_path(ctx, ctx->origin_x, ctx->origin_y);
    
    // Reset path after filling
    ctx->path_point_count = 0;
}

// Internal: scanline fill of the current path
static void _scanline_fill_path(CGContextRef ctx, int offset_x, int offset_y) {
    if (!ctx || ctx->path_point_count == 0) return;
    
    uint32_t color = ctx->fill_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    // Collect path vertices (flatten curves to line segments)
    // We'll use an active-edge scanline algorithm
    #define MAX_VERTICES 8192
    int* vertex_x = (int*)kmalloc(MAX_VERTICES * sizeof(int));
    int* vertex_y = (int*)kmalloc(MAX_VERTICES * sizeof(int));
    if (!vertex_x || !vertex_y) {
        if (vertex_x) kfree(vertex_x);
        if (vertex_y) kfree(vertex_y);
        return;
    }
    
    int vertex_count = 0;
    float cur_x = 0, cur_y = 0;
    float start_x = 0, start_y = 0;
    
    for (int i = 0; i < ctx->path_point_count && vertex_count < MAX_VERTICES - 4; i++) {
        CGPathPoint* p = &ctx->path_points[i];
        switch (p->type) {
            case CG_PATH_MOVE:
                cur_x = p->x;
                cur_y = p->y;
                start_x = cur_x;
                start_y = cur_y;
                break;
            case CG_PATH_LINE:
                vertex_x[vertex_count] = offset_x + (int)cur_x;
                vertex_y[vertex_count] = offset_y + (int)cur_y;
                vertex_count++;
                cur_x = p->x;
                cur_y = p->y;
                vertex_x[vertex_count] = offset_x + (int)cur_x;
                vertex_y[vertex_count] = offset_y + (int)cur_y;
                vertex_count++;
                break;
            case CG_PATH_CURVE_CP1:
            case CG_PATH_CURVE_CP2:
                // Skip - these are control points, handled by CURVE_END
                break;
            case CG_PATH_CURVE_END: {
                // Find the two control points preceding this endpoint
                float cp1x = cur_x, cp1y = cur_y;
                float cp2x = cur_x, cp2y = cur_y;
                if (i >= 2 && ctx->path_points[i-2].type == CG_PATH_CURVE_CP1)
                    cp1x = ctx->path_points[i-2].x, cp1y = ctx->path_points[i-2].y;
                if (i >= 1 && ctx->path_points[i-1].type == CG_PATH_CURVE_CP2)
                    cp2x = ctx->path_points[i-1].x, cp2y = ctx->path_points[i-1].y;
                
                // Subdivide cubic bezier into line segments (20 steps)
                float bx = cur_x, by = cur_y;
                for (int step = 1; step <= 20 && vertex_count < MAX_VERTICES - 2; step++) {
                    float t = (float)step / 20.0f;
                    float mt = 1.0f - t;
                    float nx = mt*mt*mt*cur_x + 3*mt*mt*t*cp1x + 3*mt*t*t*cp2x + t*t*t*p->x;
                    float ny = mt*mt*mt*cur_y + 3*mt*mt*t*cp1y + 3*mt*t*t*cp2y + t*t*t*p->y;
                    vertex_x[vertex_count] = offset_x + (int)bx;
                    vertex_y[vertex_count] = offset_y + (int)by;
                    vertex_count++;
                    vertex_x[vertex_count] = offset_x + (int)nx;
                    vertex_y[vertex_count] = offset_y + (int)ny;
                    vertex_count++;
                    bx = nx;
                    by = ny;
                }
                cur_x = p->x;
                cur_y = p->y;
                break;
            }
            case CG_PATH_CLOSE: {
                vertex_x[vertex_count] = offset_x + (int)cur_x;
                vertex_y[vertex_count] = offset_y + (int)cur_y;
                vertex_count++;
                vertex_x[vertex_count] = offset_x + (int)start_x;
                vertex_y[vertex_count] = offset_y + (int)start_y;
                vertex_count++;
                cur_x = start_x;
                cur_y = start_y;
                break;
            }
        }
    }
    
    if (vertex_count < 3) {
        kfree(vertex_x);
        kfree(vertex_y);
        return;
    }
    
    // Find bounding box
    int min_y = vertex_y[0], max_y = vertex_y[0];
    for (int i = 1; i < vertex_count; i++) {
        if (vertex_y[i] < min_y) min_y = vertex_y[i];
        if (vertex_y[i] > max_y) max_y = vertex_y[i];
    }
    
    // Clip to screen bounds
    int screen_h = 768;
    if (min_y < 0) min_y = 0;
    if (max_y >= screen_h) max_y = screen_h - 1;
    
    // Scanline fill using even-odd rule
    #define MAX_INTERSECTIONS 256
    int* intersections = (int*)kmalloc(MAX_INTERSECTIONS * sizeof(int));
    if (!intersections) {
        kfree(vertex_x);
        kfree(vertex_y);
        return;
    }
    
    for (int y = min_y; y <= max_y; y++) {
        int num_intersections = 0;
        
        // Find all edge intersections at this scanline
        // Edges are pairs of consecutive vertices
        for (int i = 0; i < vertex_count; i += 2) {
            if (i + 1 >= vertex_count) break;
            int y0 = vertex_y[i], y1 = vertex_y[i + 1];
            int x0 = vertex_x[i], x1 = vertex_x[i + 1];
            
            // Check if edge crosses this scanline
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                // Compute x intersection
                if (y1 != y0) {
                    int x_intersect = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                    if (num_intersections < MAX_INTERSECTIONS) {
                        intersections[num_intersections++] = x_intersect;
                    }
                }
            }
        }
        
        // Sort intersections
        for (int i = 0; i < num_intersections - 1; i++) {
            for (int j = i + 1; j < num_intersections; j++) {
                if (intersections[i] > intersections[j]) {
                    int tmp = intersections[i];
                    intersections[i] = intersections[j];
                    intersections[j] = tmp;
                }
            }
        }
        
        // Fill between pairs (even-odd rule)
        for (int i = 0; i + 1 < num_intersections; i += 2) {
            int x_start = intersections[i];
            int x_end = intersections[i + 1];
            if (x_start < 0) x_start = 0;
            if (x_end > 1024) x_end = 1024;
            if (x_start < x_end) {
                gfx_fill_rect(x_start, y, x_end - x_start, 1, color);
            }
        }
    }
    
    kfree(intersections);
    kfree(vertex_x);
    kfree(vertex_y);
}

// Stroke the current path
void CGContextStrokePath(CGContextRef ctx) {
    if (!ctx || ctx->path_point_count == 0) return;
    
    int lw = (int)(ctx->line_width > 0 ? ctx->line_width : 1);
    uint32_t color = ctx->stroke_color;
    
    // Apply alpha
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    // Walk the path and draw line segments
    int i = 0;
    while (i < ctx->path_point_count) {
        if (ctx->path_points[i].type == CG_PATH_MOVE) {
            i++;
            // Draw lines until next move or close
            while (i < ctx->path_point_count) {
                if (ctx->path_points[i].type == CG_PATH_MOVE) break;
                
                if (ctx->path_points[i].type == CG_PATH_LINE || 
                    ctx->path_points[i].type == CG_PATH_CLOSE) {
                    if (i > 0) {
                        float x0 = ctx->path_points[i-1].x;
                        float y0 = ctx->path_points[i-1].y;
                        float x1 = ctx->path_points[i].x;
                        float y1 = ctx->path_points[i].y;
                        
                        if (lw <= 1) {
                            gfx_draw_line((int)x0, (int)y0, (int)x1, (int)y1, color);
                        } else {
                            // Thick line: draw as filled rectangle
                            // Calculate perpendicular offsets
                            float dx = x1 - x0;
                            float dy = y1 - y0;
                            float len = 0;
                            if (dx != 0 || dy != 0) {
                                // Approximate sqrt - good enough for lines
                                len = dx*dx + dy*dy;
                                // Fast inverse sqrt approximation
                                float inv = 1.0f;
                                if (len > 0) {
                                    // Simple float sqrt
                                    float guess = len;
                                    for (int s = 0; s < 4; s++) guess = (guess + len/guess) * 0.5f;
                                    len = guess;
                                    inv = 1.0f / len;
                                }
                                dx *= inv;
                                dy *= inv;
                            }
                            float hw = lw * 0.5f;
                            float px = -dy * hw;
                            float py = dx * hw;
                            
                            // Draw as 4-point polygon
                            int ix0 = (int)(x0 + px), iy0 = (int)(y0 + py);
                            int ix1 = (int)(x1 + px), iy1 = (int)(y1 + py);
                            int ix2 = (int)(x1 - px), iy2 = (int)(y1 - py);
                            int ix3 = (int)(x0 - px), iy3 = (int)(y0 - py);
                            
                            // Fill the quad using scanlines
                            int min_y = iy0 < iy2 ? iy0 : iy2;
                            int max_y = iy1 > iy3 ? iy1 : iy3;
                            for (int sy = min_y; sy <= max_y; sy++) {
                                // Find x intersections (simplified)
                                gfx_fill_rect(ix3 < ix0 ? ix3 : ix0, sy,
                                             (ix1 > ix2 ? ix1 : ix2) - (ix3 < ix0 ? ix3 : ix0) + 1, 1, color);
                            }
                        }
                    }
                    i++;
                } else if (ctx->path_points[i].type == CG_PATH_CURVE_CP1) {
                    // Cubic bezier curve - evaluate and draw
                    if (i + 2 < ctx->path_point_count) {
                        float x0 = ctx->path_points[i-1].x;
                        float y0 = ctx->path_points[i-1].y;
                        float cp1x = ctx->path_points[i].x;
                        float cp1y = ctx->path_points[i].y;
                        float cp2x = ctx->path_points[i+1].x;
                        float cp2y = ctx->path_points[i+1].y;
                        float ex = ctx->path_points[i+2].x;
                        float ey = ctx->path_points[i+2].y;
                        
                        // Draw curve as line segments
                        float prev_x = x0, prev_y = y0;
                        int steps = 20;  // Sufficient for smooth curves
                        for (int s = 1; s <= steps; s++) {
                            float t = (float)s / steps;
                            float px, py;
                            bezier_point(x0, y0, cp1x, cp1y, cp2x, cp2y, ex, ey, t, &px, &py);
                            gfx_draw_line((int)prev_x, (int)prev_y, (int)px, (int)py, color);
                            prev_x = px;
                            prev_y = py;
                        }
                        i += 3;
                    } else {
                        i++;
                    }
                } else {
                    i++;
                }
            }
        } else {
            i++;
        }
    }
    
    ctx->path_point_count = 0;
}

// ============================================================================
// Convenience Drawing Functions
// ============================================================================

void CGContextFillRect(CGContextRef ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    
    uint32_t color = ctx->fill_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    
    // Apply shadow first
    if (ctx->shadow_color != 0x00000000 && ctx->shadow_radius > 0) {
        int sx = ax + (int)ctx->shadow_offset_x;
        int sy = ay + (int)ctx->shadow_offset_y;
        gfx_fill_rounded_rect(sx, sy, w, h, ctx->shadow_color, ctx->shadow_radius);
    }
    
    gfx_fill_rect(ax, ay, w, h, color);
}

void CGContextStrokeRect(CGContextRef ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    
    uint32_t color = ctx->stroke_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    int lw = (int)(ctx->line_width > 0 ? ctx->line_width : 1);
    
    // Draw 4 sides with line width
    gfx_fill_rect(ax, ay, w, lw, color);                 // Top
    gfx_fill_rect(ax, ay + h - lw, w, lw, color);        // Bottom
    gfx_fill_rect(ax, ay, lw, h, color);                  // Left
    gfx_fill_rect(ax + w - lw, ay, lw, h, color);        // Right
}

void CGContextFillRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius) {
    if (!ctx) return;
    
    uint32_t color = ctx->fill_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    
    // Apply shadow first
    if (ctx->shadow_color != 0x00000000 && ctx->shadow_radius > 0) {
        int sx = ax + (int)ctx->shadow_offset_x;
        int sy = ay + (int)ctx->shadow_offset_y;
        uint32_t shadow_col = ctx->shadow_color;
        // Soft shadow: draw multiple layers with decreasing opacity
        gfx_fill_rounded_rect(sx - 1, sy - 1, w + 2, h + 2, shadow_col, radius + 2);
        gfx_fill_rounded_rect(sx, sy, w, h, shadow_col, radius);
    }
    
    gfx_fill_rounded_rect(ax, ay, w, h, color, radius);
}

void CGContextStrokeRoundedRect(CGContextRef ctx, int x, int y, int w, int h, int radius) {
    if (!ctx) return;
    
    uint32_t color = ctx->stroke_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    int lw = (int)(ctx->line_width > 0 ? ctx->line_width : 1);
    
    // Use the gfx_hal stroke rounded rect for proper corner arcs
    gfx_stroke_rounded_rect(ax, ay, w, h, color, radius, lw);
}

void CGContextDrawLine(CGContextRef ctx, int x0, int y0, int x1, int y1) {
    if (!ctx) return;
    
    uint32_t color = ctx->stroke_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    gfx_draw_line(ctx->origin_x + x0, ctx->origin_y + y0,
                  ctx->origin_x + x1, ctx->origin_y + y1, color);
}

// ============================================================================
// Text Drawing
// ============================================================================

void CGContextDrawString(CGContextRef ctx, int x, int y, const char* str) {
    if (!ctx || !str) return;
    
    uint32_t color = ctx->text_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int scale = 1;
    if (ctx->font_size >= 20.0f) scale = 2;
    if (ctx->font_size >= 32.0f) scale = 3;
    
    // If we have a TrueType font, use it
    if (ctx->font && ctx->font->glyph_data) {
        CGFontDrawString(ctx->font, str, ctx->origin_x + x, ctx->origin_y + y, 
                         ctx->font_size, color);
        return;
    }
    
    // Fallback: use the built-in bitmap font
    gfx_draw_string_scaled(ctx->origin_x + x, ctx->origin_y + y, str, color, scale);
}

void CGContextDrawStringCentered(CGContextRef ctx, int cx, int y, const char* str) {
    if (!ctx || !str) return;
    
    int len = 0;
    while (str[len]) len++;
    
    int scale = 1;
    if (ctx->font_size >= 20.0f) scale = 2;
    if (ctx->font_size >= 32.0f) scale = 3;
    
    int x = cx - (len * 8 * scale) / 2;
    CGContextDrawString(ctx, x, y, str);
}

// ============================================================================
// Gradient Drawing
// ============================================================================

void CGContextDrawLinearGradient(CGContextRef ctx, int x, int y, int w, int h,
                                  uint32_t start_color, uint32_t end_color,
                                  int vertical) {
    if (!ctx) return;
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    
    // Extract RGB components
    uint8_t sr = (start_color >> 16) & 0xFF;
    uint8_t sg = (start_color >> 8) & 0xFF;
    uint8_t sb = start_color & 0xFF;
    uint8_t er = (end_color >> 16) & 0xFF;
    uint8_t eg = (end_color >> 8) & 0xFF;
    uint8_t eb = end_color & 0xFF;
    uint8_t sa = (start_color >> 24) & 0xFF;
    uint8_t ea = (end_color >> 24) & 0xFF;
    
    int steps = vertical ? h : w;
    if (steps <= 0) return;
    
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (steps - 1);
        
        uint8_t r = (uint8_t)(sr + (er - sr) * t);
        uint8_t g = (uint8_t)(sg + (eg - sg) * t);
        uint8_t b = (uint8_t)(sb + (eb - sb) * t);
        uint8_t a = (uint8_t)(sa + (ea - sa) * t);
        
        uint32_t color = (a << 24) | (r << 16) | (g << 8) | b;
        
        if (vertical) {
            gfx_fill_rect(ax, ay + i, w, 1, color);
        } else {
            gfx_fill_rect(ax + i, ay, 1, h, color);
        }
    }
}

// ============================================================================
// Image Drawing
// ============================================================================

void CGContextDrawImage(CGContextRef ctx, int x, int y, CGImageRef image) {
    if (!ctx || !image || !image->pixel_data) return;
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    
    if (image->width <= 0 || image->height <= 0) return;
    
    // Use the gfx_hal asset drawing function
    gfx_draw_asset_scaled(NULL, ax, ay, image->pixel_data, 
                          image->width, image->height,
                          image->width, image->height);
}

void CGContextDrawImageScaled(CGContextRef ctx, int x, int y, 
                               int dest_w, int dest_h, CGImageRef image) {
    if (!ctx || !image || !image->pixel_data) return;
    
    int ax = ctx->origin_x + x;
    int ay = ctx->origin_y + y;
    
    gfx_draw_asset_scaled(NULL, ax, ay, image->pixel_data,
                          image->width, image->height, dest_w, dest_h);
}

// ============================================================================
// Ellipse / Circle Drawing
// ============================================================================

void CGContextFillEllipse(CGContextRef ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    
    uint32_t color = ctx->fill_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int cx = ctx->origin_x + x + w / 2;
    int cy = ctx->origin_y + y + h / 2;
    int rx = w / 2;
    int ry = h / 2;
    
    // Midpoint ellipse algorithm
    for (int py = -ry; py <= ry; py++) {
        for (int px = -rx; px <= rx; px++) {
            // Normalized distance from center
            float dx = (float)px / (rx > 0 ? rx : 1);
            float dy = (float)py / (ry > 0 ? ry : 1);
            if (dx * dx + dy * dy <= 1.0f) {
                gfx_put_pixel(cx + px, cy + py, color);
            }
        }
    }
}

void CGContextStrokeEllipse(CGContextRef ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    
    uint32_t color = ctx->stroke_color;
    if (ctx->alpha < 1.0f) {
        uint8_t a = (uint8_t)(ctx->alpha * 255.0f);
        color = (a << 24) | (color & 0x00FFFFFF);
    }
    
    int cx = ctx->origin_x + x + w / 2;
    int cy = ctx->origin_y + y + h / 2;
    int rx = w / 2;
    int ry = h / 2;
    
    // Draw ellipse outline using parametric equation
    int steps = (rx + ry) * 2;
    if (steps < 32) steps = 32;
    if (steps > 360) steps = 360;
    
    int prev_px = 0, prev_py = 0;
    for (int i = 0; i <= steps; i++) {
        float angle = 2.0f * 3.14159265f * i / steps;
        int px = cx + (int)(rx * (1.0f - 0.0f) * ((angle < 3.14159265f ? 1 : -1) > 0 ? 1 : -1));
        // Simplified: just use cos/sin-like approximation
        // For proper ellipse, use parametric
        float t = (float)i / steps;
        float angle2 = 2.0f * 3.14159265f * t;
        
        // Simple integer ellipse approximation
        int ex = cx + (int)((float)rx * (1.0f - 2.0f * (t > 0.5f ? 1.0f - t : t) * 4.0f / 3.14159265f));
        int ey = cy + (int)((float)ry);
        
        if (i > 0) {
            gfx_draw_line(prev_px, prev_py, ex, ey, color);
        }
        prev_px = ex;
        prev_py = ey;
    }
}

void CGContextFillCircle(CGContextRef ctx, int cx, int cy, int radius) {
    CGContextFillEllipse(ctx, cx - radius, cy - radius, radius * 2, radius * 2);
}

void CGContextStrokeCircle(CGContextRef ctx, int cx, int cy, int radius) {
    CGContextStrokeEllipse(ctx, cx - radius, cy - radius, radius * 2, radius * 2);
}

// ============================================================================
// CGFont - TrueType Font Support (using stb_truetype-style approach)
// ============================================================================

CGFontRef CGFontCreate(const char* name) {
    CGFontRef font = (CGFontRef)kmalloc(sizeof(CGFont));
    if (!font) return NULL;
    
    memset(font, 0, sizeof(CGFont));
    if (name) {
        strncpy(font->name, name, 63);
        font->name[63] = 0;
    }
    font->size = 13.0f;
    font->is_bold = 0;
    font->is_italic = 0;
    font->glyph_data = NULL;
    font->ascent = 12;
    font->descent = 3;
    font->line_height = 16;
    
    return font;
}

void CGFontDestroy(CGFontRef font) {
    if (!font) return;
    // Note: glyph_data is typically statically allocated, don't free it
    kfree(font);
}

// Draw a string using a CGFont with a given size and color
void CGFontDrawString(CGFontRef font, const char* str, int x, int y, 
                       float size, uint32_t color) {
    if (!font || !str) return;
    
    // If we have TrueType glyph data, render from it
    if (font->glyph_data) {
        // TrueType rendering would go here using stb_truetype
        // For now, fall through to bitmap font
    }
    
    // Fallback: use bitmap font at the given scale
    int scale = 1;
    if (size >= 20.0f) scale = 2;
    if (size >= 32.0f) scale = 3;
    
    gfx_draw_string_scaled(x, y, str, color, scale);
}

// Measure the width of a string in pixels
int CGFontMeasureString(CGFontRef font, const char* str, float size) {
    if (!str) return 0;
    
    int len = 0;
    while (str[len]) len++;
    
    int scale = 1;
    if (size >= 20.0f) scale = 2;
    if (size >= 32.0f) scale = 3;
    
    return len * 8 * scale;
}

// ============================================================================
// CGImage - Image Loading and Management
// ============================================================================

CGImageRef CGImageCreate(int width, int height) {
    CGImageRef image = (CGImageRef)kmalloc(sizeof(CGImage));
    if (!image) return NULL;
    
    memset(image, 0, sizeof(CGImage));
    image->width = width;
    image->height = height;
    image->bpp = 32;
    image->pixel_data = (uint32_t*)kmalloc(width * height * 4);
    if (!image->pixel_data) {
        kfree(image);
        return NULL;
    }
    memset(image->pixel_data, 0, width * height * 4);
    image->owns_data = 1;  // We allocated this buffer, so we must free it on destroy
    
    return image;
}

CGImageRef CGImageCreateWithData(int width, int height, uint32_t* data) {
    CGImageRef image = (CGImageRef)kmalloc(sizeof(CGImage));
    if (!image) return NULL;
    
    memset(image, 0, sizeof(CGImage));
    image->width = width;
    image->height = height;
    image->bpp = 32;
    image->pixel_data = data;  // Takes ownership
    image->owns_data = 0;      // Don't free external data
    
    return image;
}

void CGImageDestroy(CGImageRef image) {
    if (!image) return;
    if (image->pixel_data && image->owns_data) {
        kfree(image->pixel_data);
    }
    kfree(image);
}

// Load a PNG image from filesystem
CGImageRef CGImageLoadPNG(const char* path) {
    // Read file
    extern int sys_fs_read(const char* path, char* buf, int max_len);
    
    // Allocate a large buffer for the PNG data
    int buf_size = 1024 * 1024;  // 1MB max
    uint8_t* buf = (uint8_t*)kmalloc(buf_size);
    if (!buf) return NULL;
    
    int len = sys_fs_read(path, (char*)buf, buf_size);
    if (len <= 0) {
        kfree(buf);
        return NULL;
    }
    
    // Parse PNG header to get dimensions
    // PNG header: 8 bytes signature, then IHDR chunk
    // IHDR: 4 bytes width, 4 bytes height, 1 byte bit depth, 1 byte color type, ...
    if (len < 24) {
        kfree(buf);
        return NULL;
    }
    
    // Verify PNG signature
    if (buf[0] != 0x89 || buf[1] != 'P' || buf[2] != 'N' || buf[3] != 'G') {
        kfree(buf);
        return NULL;
    }
    
    // Extract dimensions from IHDR (big-endian)
    int width = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
    int height = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
    
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        kfree(buf);
        return NULL;
    }
    
    // TODO: Full PNG decoding requires zlib inflate + filtering
    // For now, create a placeholder image
    CGImageRef image = CGImageCreate(width, height);
    if (!image) {
        kfree(buf);
        return NULL;
    }
    
    // Fill with a placeholder pattern (checkerboard)
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if ((x / 8 + y / 8) % 2 == 0) {
                image->pixel_data[y * width + x] = 0xFFE0E0E0;
            } else {
                image->pixel_data[y * width + x] = 0xFFC0C0C0;
            }
        }
    }
    
    kfree(buf);
    return image;
}

// Load a JPEG image from filesystem (placeholder)
CGImageRef CGImageLoadJPEG(const char* path) {
    // JPEG decoding is complex - placeholder for now
    // Returns a small gray placeholder image
    CGImageRef image = CGImageCreate(64, 64);
    if (!image) return NULL;
    
    for (int i = 0; i < 64 * 64; i++) {
        image->pixel_data[i] = 0xFFD0D0D0;
    }
    
    return image;
}
