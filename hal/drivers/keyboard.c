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
// Dead Key Support
// Dead keys are accent keys that don't produce a character
// immediately, but instead modify the next key pressed.
// For example: ´ + a = á, ~ + o = õ, ^ + e = ê, etc.
// If the dead key is pressed twice, or space is pressed,
// the accent character itself is produced.
// ============================================================

#define DEAD_KEY_NONE     0
#define DEAD_KEY_ACUTE    1   // ´ (acute accent)
#define DEAD_KEY_GRAVE    2   // ` (grave accent)
#define DEAD_KEY_TILDE    3   // ~ (tilde)
#define DEAD_KEY_CIRCUM   4   // ^ (circumflex)
#define DEAD_KEY_DIAER    5   // ¨ (diaeresis/umlaut)

static int kbd_dead_key = DEAD_KEY_NONE;

// Dead key composition table
// Maps (dead_key_type, base_char) -> composed_char
// Returns 0 if no valid composition exists
static int kbd_compose_dead_key(int dead, int ch) {
    // ISO-8859-1 (Latin-1) character codes
    switch (dead) {
    case DEAD_KEY_ACUTE:
        switch (ch) {
        case 'a': return 225;   // á
        case 'e': return 233;   // é
        case 'i': return 237;   // í
        case 'o': return 243;   // ó
        case 'u': return 250;   // ú
        case 'A': return 193;   // Á
        case 'E': return 201;   // É
        case 'I': return 205;   // Í
        case 'O': return 211;   // Ó
        case 'U': return 218;   // Ú
        case 'c': return 263;   // ć
        case 'C': return 262;   // Ć
        case 'y': return 253;   // ý
        case 'Y': return 221;   // Ý
        case 'z': return 378;   // ź
        case 'Z': return 377;   // Ź
        }
        break;
    case DEAD_KEY_GRAVE:
        switch (ch) {
        case 'a': return 224;   // à
        case 'e': return 232;   // è
        case 'i': return 236;   // ì
        case 'o': return 242;   // ò
        case 'u': return 249;   // ù
        case 'A': return 192;   // À
        case 'E': return 200;   // È
        case 'I': return 204;   // Ì
        case 'O': return 210;   // Ò
        case 'U': return 217;   // Ù
        }
        break;
    case DEAD_KEY_TILDE:
        switch (ch) {
        case 'a': return 227;   // ã
        case 'o': return 245;   // õ
        case 'n': return 241;   // ñ
        case 'A': return 195;   // Ã
        case 'O': return 213;   // Õ
        case 'N': return 209;   // Ñ
        }
        break;
    case DEAD_KEY_CIRCUM:
        switch (ch) {
        case 'a': return 226;   // â
        case 'e': return 234;   // ê
        case 'i': return 238;   // î
        case 'o': return 244;   // ô
        case 'u': return 251;   // û
        case 'A': return 194;   // Â
        case 'E': return 202;   // Ê
        case 'I': return 206;   // Î
        case 'O': return 212;   // Ô
        case 'U': return 219;   // Û
        }
        break;
    case DEAD_KEY_DIAER:
        switch (ch) {
        case 'a': return 228;   // ä
        case 'e': return 235;   // ë
        case 'i': return 239;   // ï
        case 'o': return 246;   // ö
        case 'u': return 252;   // ü
        case 'A': return 196;   // Ä
        case 'E': return 203;   // Ë
        case 'I': return 207;   // Ï
        case 'O': return 214;   // Ö
        case 'U': return 220;   // Ü
        case 'y': return 255;   // ÿ
        case 'Y': return 159;   // Ÿ
        }
        break;
    }
    return 0; // No valid composition
}

// Returns the standalone accent character for a dead key
// (used when dead key is pressed twice or space follows)
static int kbd_dead_key_char(int dead) {
    switch (dead) {
    case DEAD_KEY_ACUTE:  return 180; // ´
    case DEAD_KEY_GRAVE:  return 96;  // `
    case DEAD_KEY_TILDE:  return 126; // ~
    case DEAD_KEY_CIRCUM: return 94;  // ^
    case DEAD_KEY_DIAER:  return 168; // ¨
    }
    return 0;
}

// Check if a character is a dead key accent for the current layout
static int kbd_detect_dead_key(int ch) {
    switch (ch) {
    case 180: return DEAD_KEY_ACUTE;   // ´ (acute)
    case 96:  return DEAD_KEY_GRAVE;    // ` (grave)
    case 126: return DEAD_KEY_TILDE;    // ~ (tilde)
    case 94:  return DEAD_KEY_CIRCUM;   // ^ (circumflex)
    case 168: return DEAD_KEY_DIAER;    // ¨ (diaeresis)
    }
    return DEAD_KEY_NONE;
}

// ============================================================
// Keyboard Layout Tables
// Each layout has a standard (no-shift) and shift mapping
// for PS/2 scancodes 0-57, plus OEM_102 key (scancode 0x56)
// which is outside the 0-57 range and handled separately.
//
// Layout data verified against Microsoft kbdlayout.info
// scancode tables for each keyboard layout.
// ============================================================

typedef struct {
    const char std[58];      // scancodes 0-57 (base/no-shift)
    const char shift[58];    // scancodes 0-57 (shifted)
    const char oem102_std;   // scancode 0x56 base (0 = no OEM_102 key)
    const char oem102_shift; // scancode 0x56 shift
    const char* name;
} KeyboardLayout;

// Layout ID constants are defined in keyboard.h

static const KeyboardLayout kbd_layouts[] = {
    // 0: US QWERTY (default) - ANSI layout, no OEM_102 key
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,  // No OEM_102 key on ANSI US keyboard
        "US QWERTY"
    },
    // 1: UK QWERTY - ISO layout with OEM_102 key
    // Scancode 0x29=`/¬, 0x2B=#/~, 0x56=\ /|
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '#',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '"', 163, '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '@', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        '\\', '|',  // OEM_102 key: \ / |
        "UK QWERTY"
    },
    // 2: German QWERTZ - ISO layout
    // Scancode 0x29=^/°, 0x0C=ß/? 0x0D=dead acute/grave,
    // 0x1A=ü/Ü, 0x1B=+/*, 0x27=ö/Ö, 0x28=ä/Ä, 0x2B=#/'
    // 0x35=-/_, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 223, 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 252, '+', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 228, '#',
         0, '<', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', 167, '$', '%', '&', '/', '(', ')', '=', '?', 96, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 220, '*', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 196, '\'',
         0, '>', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "German QWERTZ"
    },
    // 3: French AZERTY - ISO layout
    // Scancode 0x29=², 0x02=&/1, 0x03=é/2, ..., 0x1B=$/£
    // 0x27=m/M, 0x28=ù/%, 0x2B=*/µ, 0x33=,/? 0x34=;/. 0x35=! /§
    // 0x56=< / >
    {
        {0,  27, '&', 233, '"', '\'', '(', '-', 232, '_', 231, 224, ')', '=', '\b',
         '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
         0, 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 249, 178,
         0, '*', 'w', 'x', 'c', 'v', 'b', 'n', ',', ';', ':', '!', 0, '*', 0, ' '},
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 176, '+', '\b',
         '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 168, 163, '\n',
         0, 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 181,
         0, 181, 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 167, 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "French AZERTY"
    },
    // 4: Spanish QWERTY - ISO layout
    // Scancode 0x29=º/ª, 0x0C='/?, 0x0D=¡/¿, 0x1A=`/^ (dead),
    // 0x1B=+/*, 0x27=ñ/Ñ, 0x28=dead acute/" (dead),
    // 0x2B=ç/Ç, 0x35=-/_, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 161, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 96, '+', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 241, 180, 186,
         0, 231, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', 183, '$', '%', '&', '/', '(', ')', '=', '?', 191, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '^', '*', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 209, 168, 170,
         0, 199, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Spanish QWERTY"
    },
    // 5: Italian QWERTY - ISO layout
    // Scancode 0x29=\ / |, 0x0C='/?, 0x0D=ì/^,
    // 0x1A=è/é, 0x1B=+/*, 0x27=ò/ç, 0x28=à/°, 0x2B=ù/§
    // 0x35=-/_, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 236, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 232, '+', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 242, 224, '\\',
         0, 249, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', 163, '$', '%', '&', '/', '(', ')', '=', '?', '^', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 233, '*', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 231, 176, '|',
         0, 167, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Italian QWERTY"
    },
    // 6: Brazilian ABNT2 - ISO layout with extra keys
    // Scancode 0x29='/", 0x07=6/¨(dead), 0x1A=dead acute/grave,
    // 0x1B=[/{, 0x27=ç/Ç, 0x28=~(dead)/^(dead), 0x2B=]/}
    // 0x35=;/:, 0x56=\ /|, 0x73=/? (ABNT_C1), 0x7E=. (ABNT_C2 numpad)
    // Data verified against kbdlayout.info KBDBR scancodes
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 180, '[', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 231, '~', '\'',
         0, ']', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', ';', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', 168, '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 96, '{', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 199, '^', '"',
         0, '}', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', ':', 0, '*', 0, ' '},
        '\\', '|',  // OEM_102 key: \ / |
        "Brazilian ABNT2"
    },
    // 7: Dvorak - ANSI layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '[', ']', '\b',
         '\t', '\'', ',', '.', 'p', 'y', 'f', 'g', 'c', 'r', 'l', '/', '=', '\n',
         0, 'a', 'o', 'e', 'u', 'i', 'd', 'h', 't', 'n', 's', '-', '`',
         0, '\\', ';', 'q', 'j', 'k', 'x', 'b', 'm', 'w', 'v', 'z', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '{', '}', '\b',
         '\t', '"', '<', '>', 'P', 'Y', 'F', 'G', 'C', 'R', 'L', '?', '+', '\n',
         0, 'A', 'O', 'E', 'U', 'I', 'D', 'H', 'T', 'N', 'S', '_', '~',
         0, '|', ':', 'Q', 'J', 'K', 'X', 'B', 'M', 'W', 'V', 'Z', 0, '*', 0, ' '},
        0, 0,  // No OEM_102 key on ANSI Dvorak
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
        0, 0,  // Japanese layouts vary; using ANSI for now
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
        0, 0,
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
        0, 0,
        "Chinese Pinyin"
    },
    // 11: Swiss (similar to German but with Swiss-specific chars) - ISO layout
    // Scancode 0x29=^/°(dead), 0x0C='/?, 0x0D=^/`(dead),
    // 0x1A=è/ü, 0x1B=¨/!, 0x27=ö/é, 0x28=ä/à,
    // 0x2B=$/£, 0x35=-/_, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', '^', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 232, 168, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 228, '$',
         0, '<', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '+', '"', '*', 231, '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 252, '!', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 220, 196, 163,
         0, '>', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Swiss"
    },
    // 12: Swedish - ISO layout
    // Scancode 0x29=§/°, 0x0C=+/? 0x0D=dead acute/grave,
    // 0x1A=å/Å, 0x1B=dead diaeresis/circumflex, 0x27=ö/Ö, 0x28=ä/Ä,
    // 0x2B=dead tilde/circumflex(or ø), 0x35=-/_, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 229, 168, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 228, 230,
         0, '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', 164, '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 197, '^', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 196, 198,
         0, '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Swedish"
    },
    // 13: Norwegian - ISO layout
    // Similar to Swedish but 0x28=ø/Ø, 0x2B=å/Å
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 229, 168, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 248, 229,
         0, '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', 164, '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 197, '^', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 216, 197,
         0, '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Norwegian"
    },
    // 14: Danish - ISO layout
    // Similar to Norwegian
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 229, 168, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 230, 230,
         0, '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', 164, '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 197, '^', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 198, 216,
         0, '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Danish"
    },
    // 15: Finnish - ISO layout (same as Swedish)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 229, 168, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 246, 228, 228,
         0, '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', 164, '%', '&', '/', '(', ')', '=', '?', '`', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 197, '^', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 214, 196, 196,
         0, '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Finnish"
    },
    // 16: Polish (Programmer's - US-based with AltGr for Polish chars)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,  // Polish programmer's uses ANSI layout
        "Polish"
    },
    // 17: Czech QWERTZ - ISO layout
    {
        {0,  27, '+', 283, 353, 269, 345, 382, 253, 225, 237, 233, '=', 250, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 250, '/', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 367, 167, 366,
         0, '\\', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '%', 270, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 218, '?', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 344, '"', 327,
         0, '|', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Czech QWERTZ"
    },
    // 18: Hungarian - ISO layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', 246, 252, 243, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 245, 250, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 233, 225, 250,
         0, 237, 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '\'', '"', '+', '!', '%', '/', '=', '(', ')', 214, 220, 211, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 213, 218, '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 201, 193, 218,
         0, 205, 'Y', 'X', 'C', 'V', 'B', 'N', 'M', '?', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Hungarian"
    },
    // 19: Romanian - ISO layout (close to US, with 0x29=â/Â at top-left)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 259,
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', 258,
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Romanian"
    },
    // 20: Turkish Q - ISO layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '*', '-', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 305, 'o', 'p', 287, 252, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 351, ',', 246,
         0, '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', 246, 231, '.', 0, '*', 0, ' '},
        {0,  27, '!', '\'', '^', '+', '%', '&', '/', '(', ')', '=', '?', '_', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 304, 220, '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 350, ';', 214,
         0, '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 214, 199, ':', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Turkish Q"
    },
    // 21: Turkish F - ISO layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '/', '-', '\b',
         '\t', 'f', 'g', 287, 'o', 'd', 'r', 'n', 'h', 'p', 'q', 'w', 'u', '\n',
         0, 'l', 'u', 'i', 'e', 'a', 252, 't', 'k', 'm', 'l', 'y', 351,
         0, ',', 'j', 246, 'v', 'c', 231, 'z', 's', 'b', '.', 'x', 0, '*', 0, ' '},
        {0,  27, '!', '\'', '^', '+', '%', '&', '/', '(', ')', '=', '?', '_', '\b',
         '\t', 'F', 'G', 304, 'O', 'D', 'R', 'N', 'H', 'P', 'Q', 'W', 'U', '\n',
         0, 'L', 'U', 'I', 'E', 'A', 220, 'T', 'K', 'M', 'L', 'Y', 350,
         0, ';', 'J', 214, 'V', 'C', 199, 'Z', 'S', 'B', ':', 'X', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Turkish F"
    },
    // 22: Russian (JCUKEN layout - Cyrillic mapped to US positions)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 233, 246, 243, 234, 229, 237, 227, 248, 249, 231, 245, 250, '\n',
         0, 244, 251, 226, 224, 239, 240, 238, 235, 228, 230, 251, 255,
         0, '\\', 255, 247, 241, 236, 232, 242, 252, 225, 254, 231, 0, '*', 0, ' '},
        {0,  27, '!', '"', 185, ';', '%', ':', '?', '*', '(', ')', '_', '+', '\b',
         '\t', 201, 214, 211, 202, 197, 205, 195, 216, 217, 199, 213, 218, '\n',
         0, 212, 219, 194, 192, 207, 208, 206, 203, 196, 198, 219, 223,
         0, '/', 223, 215, 209, 204, 200, 210, 220, 193, 222, 199, 0, '*', 0, ' '},
        0, 0,  // Russian uses ANSI-like layout
        "Russian"
    },
    // 23: Arabic (US-based for ASCII input layer)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,
        "Arabic"
    },
    // 24: Hebrew (placeholder - needs proper Hebrew character mapping)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', '/', '\'', 247, 248, 224, 233, 233, 233, 233, 237, 231, 240, '\n',
         0, 249, 231, 231, 231, 231, 233, 233, 233, 231, 231, 231, 231,
         0, '\\', 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        0, 0,
        "Hebrew"
    },
    // 25: Thai (Kedmanee layout - US-based for ASCII layer)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,
        "Thai"
    },
    // 26: Vietnamese (US-based with dead keys for tone marks)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,
        "Vietnamese"
    },
    // 27: Greek (placeholder - needs proper Greek character mapping)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', ';', 243, 235, 231, 244, 253, 248, 233, 239, 240, 232, 250, '\n',
         0, 224, 243, 228, 246, 231, 252, 233, 234, 239, 239, 253, 247,
         0, '\\', 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
         '\t', ':', 211, 203, 199, 212, 221, 216, 201, 212, 200, 202, 218, '\n',
         0, 192, 211, 208, 214, 199, 220, 201, 202, 202, 221, 215,
         0, '|', 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 0, '*', 0, ' '},
        0, 0,
        "Greek"
    },
    // 28: Croatian/Serbian/Slovenian - ISO layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 180, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 353, 273, '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 269, 273, 382,
         0, '<', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', '$', '%', '&', '/', '(', ')', '=', '?', 168, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 352, 272, '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 268, 272, 381,
         0, '>', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Croatian"
    },
    // 29: Portuguese (Portugal) - ISO layout
    // Scancode 0x29=\\/|, 0x0C='/?, 0x0D=«/», 0x27=ç/Ç
    // 0x28=º/ª, 0x2B=~/^, 0x56=< / >
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 171, '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '+', '\'', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 231, 186, '\\',
         0, '~', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' '},
        {0,  27, '!', '"', '#', '$', '%', '&', '/', '(', ')', '=', '?', 187, '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '*', 171, '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 199, 170, '|',
         0, '^', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' '},
        '<', '>',  // OEM_102 key: < / >
        "Portuguese PT"
    },
    // 30: Canadian Multilingual - ANSI layout
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
         0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', '?', '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
         0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        0, 0,
        "Canadian"
    },
    // 31: Brazilian ABNT1
    // Like US QWERTY but with ç at scancode 0x27, dead tilde at 0x28,
    // and dead acute/grave at 0x29. Uses standard / at 0x35 (no ABNT_C1 extra key).
    // Scancode 0x29='/", 0x27=ç/Ç, 0x28=~(dead)/^(dead)
    {
        {0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
         '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 180, '[', '\n',
         0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 231, '~', '\'',
         0, ']', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
        {0,  27, '!', '@', '#', '$', '%', 168, '&', '*', '(', ')', '_', '+', '\b',
         '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 96, '{', '\n',
         0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 199, '^', '"',
         0, '}', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '},
        '\\', '|',  // OEM_102 key: \ / | (ABNT1 may have ISO key)
        "Brazilian ABNT1"
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
    kbd_dead_key = DEAD_KEY_NONE;
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

        // Handle OEM_102 key (scancode 0x56 = extra key between LShift and Z on ISO keyboards)
        // Present on most European and Brazilian keyboards
        if (scancode == 0x56 && key_out == 0) {
            const KeyboardLayout* layout = &kbd_layouts[kbd_layout];
            if (layout->oem102_std) {
                key_out = kbd_shift_pressed ? (unsigned char)layout->oem102_shift
                                            : (unsigned char)layout->oem102_std;
            }
        }

        // Handle ABNT_C1 key (scancode 0x73 = extra '/' key on Brazilian ABNT2 keyboards)
        // Located between ';' and Right Shift
        if (scancode == 0x73 && key_out == 0) {
            key_out = kbd_shift_pressed ? '?' : '/';
        }

        // Handle ABNT_C2 key (scancode 0x7E = numpad '.' on Brazilian ABNT2 keyboards)
        // This replaces the standard numpad delete/period on ABNT2 keyboards
        if (scancode == 0x7E && key_out == 0) {
            key_out = '.';
        }

        if (key_out == 0 && scancode < 58) {
            // Use active layout tables
            const char* layout_std = kbd_layouts[kbd_layout].std;
            const char* layout_shift = kbd_layouts[kbd_layout].shift;

            // Char mapping - cast to unsigned char to prevent sign extension
            // for Latin-1 characters > 127 (e.g., ç=231, ü=252, etc.)
            if (kbd_shift_pressed ^ kbd_caps_lock) {
                // Handle letters specifically for Caps Lock
                unsigned char base = (unsigned char)layout_std[scancode];
                if (base >= 'a' && base <= 'z') {
                    key_out = (unsigned char)layout_shift[scancode];
                } else {
                    // Non-letters affected by Shift only, mostly
                    key_out = kbd_shift_pressed ? (unsigned char)layout_shift[scancode] : (unsigned char)layout_std[scancode];
                }
            } else {
                key_out = kbd_shift_pressed ? (unsigned char)layout_shift[scancode] : (unsigned char)layout_std[scancode];
            }

            // --- Dead Key Detection ---
            // Check if the produced character is a dead key accent
            int dead = kbd_detect_dead_key(key_out);
            if (dead != DEAD_KEY_NONE) {
                // If we already had a pending dead key, produce the
                // previous accent first, then store the new one
                if (kbd_dead_key != DEAD_KEY_NONE) {
                    int prev_char = kbd_dead_key_char(kbd_dead_key);
                    if (prev_char) {
                        int next = (write_ptr + 1) % KBD_BUFFER_SIZE;
                        if (next != read_ptr) {
                            kbd_buffer[write_ptr] = prev_char;
                            write_ptr = next;
                        }
                    }
                }
                kbd_dead_key = dead;
                key_out = 0; // Don't produce a character yet
            }
        }
    }

    if (key_out) {
        // --- Dead Key Composition ---
        if (kbd_dead_key != DEAD_KEY_NONE) {
            // Try to compose dead key with the new character
            int composed = kbd_compose_dead_key(kbd_dead_key, key_out);
            if (composed) {
                // Valid composition - produce the composed character
                key_out = composed;
            } else if (key_out == ' ') {
                // Space after dead key - produce the accent itself
                key_out = kbd_dead_key_char(kbd_dead_key);
            } else if (kbd_detect_dead_key(key_out) != DEAD_KEY_NONE) {
                // Another dead key pressed - produce previous accent,
                // set new dead key (handled in detection above)
            } else {
                // Invalid composition - produce the accent first, then the key
                int accent = kbd_dead_key_char(kbd_dead_key);
                if (accent) {
                    int next = (write_ptr + 1) % KBD_BUFFER_SIZE;
                    if (next != read_ptr) {
                        kbd_buffer[write_ptr] = accent;
                        write_ptr = next;
                    }
                }
                // key_out remains the original character
            }
            kbd_dead_key = DEAD_KEY_NONE;
        }

        int next2 = (write_ptr + 1) % KBD_BUFFER_SIZE;
        if (next2 != read_ptr) {
            kbd_buffer[write_ptr] = key_out;
            write_ptr = next2;
        }
    }
}
