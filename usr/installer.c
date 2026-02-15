// usr/installer.c - Modern Avant-Garde Redesign
#include "../hal/drivers/vga.h"
#include "../hal/drivers/ata.h"
#include "../hal/drivers/mouse.h"
#include "../hal/video/gfx_hal.h"
#include "../core/string.h"
#include "mbr_bytes.h"

// --- Modern Design Tokens ---
#define C_BG_WHITE      0xFFFFFFFF
#define C_TEXT_DARK     0xFF1A1A1A
#define C_ACCENT_TEAL   0xFF00BFA5
#define C_ACCENT_HOVER  0xFF009E89
#define C_SLIDER_TRACK  0xFFE0E0E0
#define C_SLIDER_FILL   0xFF263238
#define C_DANGER        0xFFFF5252

// --- State ---
int slider_val = 50; // Percentage
int is_dragging = 0;
int current_step = 0; // 0=Welcome, 1=Disk, 2=Install, 3=Done

void draw_text_centered(int y, const char* str, int scale, uint32_t color) {
    int w = strlen(str) * 6 * scale;
    gfx_draw_string_scaled((1024 - w) / 2, y, str, color, scale);
}

void draw_modern_button(int x, int y, int w, int h, const char* label, int primary) {
    extern int mouse_x, mouse_y, mouse_btn_left;
    int hover = (mouse_x >= x && mouse_x <= x+w && mouse_y >= y && mouse_y <= y+h);
    
    uint32_t bg = primary ? (hover ? C_ACCENT_HOVER : C_ACCENT_TEAL) : (hover ? 0xFFEEEEEE : 0xFFFFFFFF);
    uint32_t border = primary ? bg : 0xFFCCCCCC;
    uint32_t text = primary ? 0xFFFFFFFF : C_TEXT_DARK;
    
    // Shadow
    if (hover) gfx_fill_rounded_rect(x+4, y+4, w, h, 0x10000000, 8);
    
    gfx_fill_rounded_rect(x, y, w, h, bg, 8);
    if (!primary) gfx_draw_rect(x, y, w, h, border); // Border for secondary
    
    int tw = strlen(label) * 6;
    gfx_draw_string(x + (w-tw)/2, y + (h-8)/2, label, text);
    
    // Logic (Simple click detection)
    static int was_clicked = 0;
    if (hover && mouse_btn_left && !was_clicked) {
        was_clicked = 1;
        // Trigger action based on label (Hack for demo)
        if (strcmp(label, "Install Camel OS") == 0) current_step = 1;
        if (strcmp(label, "Start Installation") == 0) current_step = 2;
        if (strcmp(label, "Reboot System") == 0) outb(0x64, 0xFE);
    }
    if (!mouse_btn_left) was_clicked = 0;
}

void render_slider(int x, int y, int w) {
    extern int mouse_x, mouse_y, mouse_btn_left;
    
    // Track
    gfx_fill_rounded_rect(x, y, w, 12, C_SLIDER_TRACK, 6);
    
    // Fill
    int fill_w = (w * slider_val) / 100;
    gfx_fill_rounded_rect(x, y, fill_w, 12, C_ACCENT_TEAL, 6);
    
    // Knob
    int knob_x = x + fill_w - 10;
    gfx_fill_rounded_rect(knob_x, y-6, 20, 24, C_SLIDER_FILL, 10);
    
    // Labels
    char buf[32]; 
    int_to_str(slider_val, buf); strcat(buf, "% System");
    gfx_draw_string(x, y-20, buf, C_TEXT_DARK);
    
    int_to_str(100-slider_val, buf); strcat(buf, "% User Data");
    gfx_draw_string(x+w-100, y-20, buf, C_TEXT_DARK);
    
    // Interaction
    if (mouse_btn_left) {
        if (mouse_x >= x && mouse_x <= x+w && mouse_y >= y-10 && mouse_y <= y+30) is_dragging = 1;
    } else {
        is_dragging = 0;
    }
    
    if (is_dragging) {
        int rel = mouse_x - x;
        slider_val = (rel * 100) / w;
        if (slider_val < 10) slider_val = 10;
        if (slider_val > 90) slider_val = 90;
    }
}

void installer_main() {
    gfx_init_hal(0); // Assuming bootloader setup
    
    while(1) {
        // Clear White
        gfx_fill_rect(0, 0, 1024, 768, C_BG_WHITE);
        
        // Header
        gfx_fill_rect(0, 0, 1024, 80, C_BG_WHITE);
        draw_text_centered(30, "Camel OS", 2, C_TEXT_DARK);
        
        int center_x = 1024/2;
        int center_y = 768/2;
        
        if (current_step == 0) {
            // Welcome
            draw_text_centered(center_y - 100, "The Avant-Garde Operating System", 1, 0xFF666666);
            draw_modern_button(center_x - 100, center_y + 50, 200, 50, "Install Camel OS", 1);
        }
        else if (current_step == 1) {
            // Partition
            draw_text_centered(center_y - 150, "Configure Partition Layout", 2, C_TEXT_DARK);
            render_slider(center_x - 250, center_y, 500);
            draw_modern_button(center_x - 100, center_y + 100, 200, 50, "Start Installation", 1);
        }
        else if (current_step == 2) {
            // Installing
            static int progress = 0;
            if (progress < 100) progress++;
            else current_step = 3;
            
            draw_text_centered(center_y - 50, "Installing...", 2, C_TEXT_DARK);
            
            // Progress Bar
            int bar_w = 400;
            gfx_fill_rounded_rect(center_x - bar_w/2, center_y + 20, bar_w, 10, C_SLIDER_TRACK, 5);
            gfx_fill_rounded_rect(center_x - bar_w/2, center_y + 20, (bar_w * progress)/100, 10, C_ACCENT_TEAL, 5);
            
            // Simulate work
            for(volatile int i=0; i<1000000; i++);
        }
        else {
            // Done
            draw_text_centered(center_y - 50, "Installation Complete", 2, C_ACCENT_TEAL);
            draw_modern_button(center_x - 100, center_y + 50, 200, 50, "Reboot System", 1);
        }
        
        // Mouse Cursor
        extern int mouse_x, mouse_y;
        extern void mouse_handler();
        mouse_handler();
        gfx_fill_rect(mouse_x, mouse_y, 10, 10, C_TEXT_DARK); // Simple square cursor
        
        gfx_swap_buffers();
    }
}