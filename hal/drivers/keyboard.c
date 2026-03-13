#include "../common/ports.h"
#include "../sys/api.h"
#include "../../include/input_defs.h"

#define KBD_BUFFER_SIZE 256
int kbd_buffer[KBD_BUFFER_SIZE]; // Changed to int to support > 127
int write_ptr = 0;
int read_ptr = 0;

// Global flags
int kbd_shift_pressed = 0;
int kbd_ctrl_pressed = 0;
int kbd_alt_pressed = 0;
int kbd_caps_lock = 0;

// Internal state for extended codes (e.g. E0 xx)
static int kbd_extended = 0;

// Standard Map (No Shift)
const char scancode_std[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

// Shift Map
const char scancode_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

void init_keyboard() {
    inb(0x60); // flush
    write_ptr = 0;
    read_ptr = 0;
    kbd_shift_pressed = 0;
    kbd_ctrl_pressed = 0;
    kbd_alt_pressed = 0;
    kbd_caps_lock = 0;
    kbd_extended = 0;
}

void kbd_flush() {
    write_ptr = 0;
    read_ptr = 0;
}

// Updated to return int
int sys_get_key() {
    if (read_ptr == write_ptr) return 0;
    int c = kbd_buffer[read_ptr];
    read_ptr = (read_ptr + 1) % KBD_BUFFER_SIZE;
    return c;
}

void keyboard_callback() {
    uint8_t scancode = inb(0x60);

    // Handle Extended Prefix (E0)
    if (scancode == 0xE0) {
        kbd_extended = 1;
        return;
    }

    // Handle Key Release (Bit 7 set)
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) kbd_shift_pressed = 0;
        if (released == 0x1D) kbd_ctrl_pressed = 0;
        if (released == 0x38) kbd_alt_pressed = 0;
        kbd_extended = 0; // Reset extended state on release too
        return;
    }

    // --- Special Keys Logic ---
    int key_out = 0;

    if (kbd_extended) {
        // Extended codes (Arrows, Home, End, etc.)
        switch (scancode) {
            case 0x48: key_out = KEY_UP; break;
            case 0x50: key_out = KEY_DOWN; break;
            case 0x4B: key_out = KEY_LEFT; break;
            case 0x4D: key_out = KEY_RIGHT; break;
            case 0x47: key_out = KEY_HOME; break;
            case 0x4F: key_out = KEY_END; break;
            case 0x49: key_out = KEY_PGUP; break;
            case 0x51: key_out = KEY_PGDN; break;
            case 0x52: key_out = KEY_INSERT; break;
            case 0x53: key_out = KEY_DELETE; break;
            case 0x5B: key_out = KEY_LWIN; break; // Left Windows/Command
        }
        kbd_extended = 0;
    } else {
        // Normal codes
        if (scancode == 0x2A || scancode == 0x36) { kbd_shift_pressed = 1; return; }
        if (scancode == 0x1D) { kbd_ctrl_pressed = 1; return; }
        if (scancode == 0x38) { kbd_alt_pressed = 1; return; }
        if (scancode == 0x3A) { kbd_caps_lock = !kbd_caps_lock; return; } // Toggle Caps

        // F-Keys
        if (scancode >= 0x3B && scancode <= 0x44) key_out = KEY_F1 + (scancode - 0x3B);
        if (scancode == 0x57) key_out = KEY_F11;
        if (scancode == 0x58) key_out = KEY_F12;

        if (key_out == 0 && scancode < 58) {
            // Char mapping
            if (kbd_shift_pressed ^ kbd_caps_lock) {
                // Handle letters specifically for Caps Lock
                char base = scancode_std[scancode];
                if (base >= 'a' && base <= 'z') {
                    key_out = scancode_shift[scancode];
                } else {
                    // Non-letters affected by Shift only, mostly
                    key_out = kbd_shift_pressed ? scancode_shift[scancode] : scancode_std[scancode];
                }
            } else {
                key_out = kbd_shift_pressed ? scancode_shift[scancode] : scancode_std[scancode];
            }
        }
    }

    if (key_out) {
        int next = (write_ptr + 1) % KBD_BUFFER_SIZE;
        if (next != read_ptr) {
            kbd_buffer[write_ptr] = key_out;
            write_ptr = next;
        }
    }
}
