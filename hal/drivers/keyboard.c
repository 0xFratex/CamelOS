#include "../common/ports.h"
#include "../sys/api.h"
#include "../../include/input_defs.h"
#include "keyboard.h"

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

// Current keyboard layout (0 = US QWERTY)
int kbd_layout = 0;

// ============================================================
// Keyboard Layout Tables
// Each layout has a standard (no-shift) and shift mapping
// for PS/2 scancodes 0-57.
// ============================================================

typedef struct {
    const char std[58];
    const char shift[58];
    const char* name;
} KeyboardLayout;

// Layout ID constants are defined in keyboard.h

static const KeyboardLayout kbd_layouts[] = {
    // 0: US QWERTY (default)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "US QWERTY"
    },
    // 1: UK QWERTY
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '#',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '"', 163, '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '@', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "UK QWERTY"
    },
    // 2: German QWERTZ
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 223, '\'', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 252, '+', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 228, '#',
         0, '<', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', 167, '$', '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 220, '*', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 196, '\'',
         0, '>', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        "German QWERTZ"
    },
    // 3: French AZERTY
    {
        {0,  27, '&', 233, '"', '\'', '(', '-', 232, '_', 231, 224, ')', '=', '\b',
         '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
         0, 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 249, '*',
         0, '<', 'w', 'x', 'c', 'v', 'b', 'n', ',', ';', ':', '!', 0, '*', 0, ' '},
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 176, '+', '\b',
         '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 168, 163, '\n',
         0, 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 181,
         0, '>', 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 167, 0, '*', 0, ' '},
        "French AZERTY"
    },
    // 4: Spanish QWERTY
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '"', 183, '$', '%', '&', '/', '(', ')', '=', '?', 168, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "Spanish QWERTY"
    },
    // 5: Italian QWERTY
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 236, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 232, '+', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 242, 224, 249,
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', 163, '$', '%', '&', '/', '(', ')', '=', '?', '^', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 233, '*', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 231, 176, 167,
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        "Italian QWERTY"
    },
    // 6: Portuguese (Brazil) QWERTY
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 231, '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', ';', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', 168, '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', (char)199, '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', ':', 0, '*', 0, ' '},
        "Portuguese BR"
    },
    // 7: Dvorak
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '[', ']', '\b',
         '\t', '\'', ',', '.', 'p', 'y', 'f', 'g', 'c', 'r', 'l', '/', '=', '\n',
         0, 'a', 'o', 'e', 'u', 'i', 'd', 'h', 't', 'n', 's', '-', '`',
         0, '\\', ';', 'q', 'j', 'k', 'x', 'b', 'm', 'w', 'v', 'z', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '{', '}', '\b',
         '\t', '"', '<', '>', 'P', 'Y', 'F', 'G', 'C', 'R', 'L', '?', '+', '\n',
         0, 'A', 'O', 'E', 'U', 'I', 'D', 'H', 'T', 'N', 'S', '_', '~',
         0, '|', ':', 'Q', 'J', 'K', 'X', 'B', 'M', 'W', 'V', 'Z', 0, '*', 0, ' '},
        "Dvorak"
    },
    // 8: Japanese (Romaji - same as US for ASCII input)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "Japanese"
    },
    // 9: Korean (same as US for ASCII input)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "Korean"
    },
    // 10: Chinese Pinyin (same as US for ASCII input)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        "Chinese Pinyin"
    },
};

// Keep legacy names for backward compatibility
const char scancode_std[58] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

const char scancode_shift[58] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

// Set keyboard layout
void kbd_set_layout(int layout_id) {
    if (layout_id >= 0 && layout_id < KBD_LAYOUT_COUNT) {
        kbd_layout = layout_id;
    }
}

// Get layout display name
const char* kbd_layout_name(int layout_id) {
    if (layout_id >= 0 && layout_id < KBD_LAYOUT_COUNT) {
        return kbd_layouts[layout_id].name;
    }
    return "Unknown";
}

// Find layout by name string
int kbd_find_layout_by_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < KBD_LAYOUT_COUNT; i++) {
        // Simple prefix match (handles "US", "German", "French", etc.)
        const char* lname = kbd_layouts[i].name;
        int match = 1;
        for (int j = 0; name[j] && lname[j]; j++) {
            if (name[j] != lname[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    return 0; // Default to US
}

void init_keyboard() {
    inb(0x60); // flush
    write_ptr = 0;
    read_ptr = 0;
    kbd_shift_pressed = 0;
    kbd_ctrl_pressed = 0;
    kbd_alt_pressed = 0;
    kbd_caps_lock = 0;
    kbd_extended = 0;
    kbd_layout = 0; // Default US QWERTY
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
            // Use active layout tables
            const char* layout_std = kbd_layouts[kbd_layout].std;
            const char* layout_shift = kbd_layouts[kbd_layout].shift;

            // Char mapping
            if (kbd_shift_pressed ^ kbd_caps_lock) {
                // Handle letters specifically for Caps Lock
                char base = layout_std[scancode];
                if (base >= 'a' && base <= 'z') {
                    key_out = layout_shift[scancode];
                } else {
                    // Non-letters affected by Shift only, mostly
                    key_out = kbd_shift_pressed ? layout_shift[scancode] : layout_std[scancode];
                }
            } else {
                key_out = kbd_shift_pressed ? layout_shift[scancode] : layout_std[scancode];
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
