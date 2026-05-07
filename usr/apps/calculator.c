// usr/apps/calculator.c - CamelOS Calculator App
// A macOS-inspired calculator with clean rounded buttons
// Supports: +, -, *, /, %, +/-, decimal, clear, backspace
#include "../lib/camel_framework.h"
#include "../framework.h"
#include "../../sys/api.h"
#include "../../core/string.h"
#include "../../hal/video/gfx_hal.h"
#include "../dock.h"
#include "../../core/window_server.h"
#include "../../hal/drivers/serial.h"

// ============================================================================
// LAYOUT CONSTANTS
// ============================================================================
#define CALC_WIN_W   300
#define CALC_WIN_H   420

#define DISPLAY_H    80
#define MARGIN        8
#define GAP           6
#define BTN_W        66
#define BTN_H        50
#define BTN_WIDE     138   // "0" button spans 2 cols: 66*2 + 6

// ============================================================================
// COLOR PALETTE (macOS-inspired, ARGB)
// ============================================================================
#define COL_DISPLAY_BG     0xFF1C1C1E    // Dark display background
#define COL_FUNC_BTN       0xFFA5A5A5    // Function button (C, +/-, %)
#define COL_FUNC_BTN_HOVER 0xFFB8B8B8
#define COL_DIGIT_BTN      0xFF505050    // Digit button (0-9)
#define COL_DIGIT_BTN_HOVER 0xFF636363
#define COL_OP_BTN         0xFFFF9500    // Operator button (orange)
#define COL_OP_BTN_HOVER   0xFFFFAA33
#define COL_OP_BTN_ACTIVE  0xFFFFFFFF    // Active operator: white bg
#define COL_TEXT_LIGHT     0xFFFFFFFF    // White text
#define COL_TEXT_DARK      0xFF000000    // Black text (for active op)
#define COL_DISPLAY_TEXT   0xFFFFFFFF    // White display text
#define COL_OP_INDICATOR   0xFFFF9500    // Orange operation indicator
#define COL_BODY_BG        0xFF2C2C2E    // Dark body background

// ============================================================================
// BUTTON TYPES & OPERATION CODES
// ============================================================================
#define BTN_DIGIT    0
#define BTN_OPERATOR 1
#define BTN_FUNCTION 2

#define OP_NONE  0
#define OP_ADD   1
#define OP_SUB   2
#define OP_MUL   3
#define OP_DIV   4

// Button IDs
#define BID_7        7
#define BID_8        8
#define BID_9        9
#define BID_4        4
#define BID_5        5
#define BID_6        6
#define BID_1        1
#define BID_2        2
#define BID_3        3
#define BID_0        0
#define BID_C        100
#define BID_NEGATE   101
#define BID_PERCENT  102
#define BID_DIV      103
#define BID_MUL      104
#define BID_SUB      105
#define BID_ADD      106
#define BID_EQUALS   107
#define BID_DECIMAL  108

// ============================================================================
// CALCULATOR STATE
// ============================================================================
typedef struct {
    char display[32];       // Current display text
    double current_value;   // Current accumulator
    double operand;         // Stored operand
    int operation;          // Pending operation (OP_NONE, OP_ADD, etc.)
    int has_decimal;        // Decimal point entered in current input
    int new_number;         // Start new number on next digit press
    int error;              // Error state (division by zero, etc.)
    int decimal_places;     // Number of decimal places entered
} calc_state_t;

static calc_state_t calc;
static Window* calc_window = 0;

// ============================================================================
// BUTTON DEFINITIONS
// ============================================================================
typedef struct {
    int row, col;
    int col_span;           // 1 = normal, 2 = wide ("0" button)
    const char* label;      // Display label
    int type;               // BTN_DIGIT, BTN_OPERATOR, BTN_FUNCTION
    int id;                 // Button ID
} calc_button_t;

// Button layout — matches macOS calculator order
// Row 0: C    +/-   %     ÷
// Row 1: 7    8     9     ×
// Row 2: 4    5     6     −
// Row 3: 1    2     3     +
// Row 4: 0(wide)   .     =
static calc_button_t buttons[] = {
    { 0, 0, 1, "C",    BTN_FUNCTION,  BID_C       },
    { 0, 1, 1, "+/-",  BTN_FUNCTION,  BID_NEGATE  },
    { 0, 2, 1, "%",    BTN_FUNCTION,  BID_PERCENT },
    { 0, 3, 1, "/",    BTN_OPERATOR,  BID_DIV     },
    { 1, 0, 1, "7",    BTN_DIGIT,     BID_7       },
    { 1, 1, 1, "8",    BTN_DIGIT,     BID_8       },
    { 1, 2, 1, "9",    BTN_DIGIT,     BID_9       },
    { 1, 3, 1, "*",    BTN_OPERATOR,  BID_MUL     },
    { 2, 0, 1, "4",    BTN_DIGIT,     BID_4       },
    { 2, 1, 1, "5",    BTN_DIGIT,     BID_5       },
    { 2, 2, 1, "6",    BTN_DIGIT,     BID_6       },
    { 2, 3, 1, "-",    BTN_OPERATOR,  BID_SUB     },
    { 3, 0, 1, "1",    BTN_DIGIT,     BID_1       },
    { 3, 1, 1, "2",    BTN_DIGIT,     BID_2       },
    { 3, 2, 1, "3",    BTN_DIGIT,     BID_3       },
    { 3, 3, 1, "+",    BTN_OPERATOR,  BID_ADD     },
    { 4, 0, 2, "0",    BTN_DIGIT,     BID_0       },
    { 4, 2, 1, ".",    BTN_FUNCTION,  BID_DECIMAL },
    { 4, 3, 1, "=",    BTN_OPERATOR,  BID_EQUALS  },
};
#define BTN_COUNT (sizeof(buttons) / sizeof(buttons[0]))

// ============================================================================
// HELPER: compute button rectangle (relative to content area origin)
// ============================================================================
static void calc_button_rect(const calc_button_t* b, int* bx, int* by, int* bw, int* bh) {
    *bx = MARGIN + b->col * (BTN_W + GAP);
    *by = DISPLAY_H + MARGIN + b->row * (BTN_H + GAP);
    *bw = (b->col_span == 2) ? BTN_WIDE : BTN_W;
    *bh = BTN_H;
}

// ============================================================================
// DOUBLE-TO-STRING CONVERSION (no libc printf with %f available)
// ============================================================================
static void double_to_str(double val, char* buf, int max_len) {
    // Handle special cases
    if (val != val) {
        // NaN
        strcpy(buf, "Error");
        return;
    }

    // Handle sign
    int negative = 0;
    if (val < 0) {
        negative = 1;
        val = -val;
    }

    // Handle very large or very small numbers
    if (val > 999999999.0) {
        // Scientific notation for very large numbers
        int exp = 0;
        double v = val;
        while (v >= 10.0) { v /= 10.0; exp++; }
        // Format: X.XXe+N
        // Integer part of mantissa
        int mantissa_int = (int)v;
        v -= (double)mantissa_int;
        int mantissa_frac = (int)(v * 100.0 + 0.5);
        if (mantissa_frac >= 100) { mantissa_int++; mantissa_frac = 0; }

        int pos = 0;
        if (negative && pos < max_len - 1) buf[pos++] = '-';
        if (mantissa_int < 10 && pos < max_len - 1) buf[pos++] = '0' + mantissa_int;
        if (pos < max_len - 1) buf[pos++] = '.';
        if (pos < max_len - 1) buf[pos++] = '0' + (mantissa_frac / 10);
        if (pos < max_len - 1) buf[pos++] = '0' + (mantissa_frac % 10);
        if (pos < max_len - 1) buf[pos++] = 'e';
        if (pos < max_len - 1) buf[pos++] = '+';
        // Write exponent digits
        char exp_str[8];
        int_to_str(exp, exp_str);
        for (int i = 0; exp_str[i] && pos < max_len - 1; i++)
            buf[pos++] = exp_str[i];
        buf[pos] = 0;
        return;
    }

    // Very small numbers close to zero
    if (val > 0.0 && val < 0.0000001) {
        strcpy(buf, "0");
        return;
    }

    // Normal number formatting
    // Separate integer and fractional parts
    unsigned long integer_part = (unsigned long)val;
    double frac = val - (double)integer_part;

    // Round: if frac is very close to 1.0
    if (frac > 0.9999995) {
        integer_part++;
        frac = 0.0;
    }

    // Convert integer part
    char int_buf[20];
    if (integer_part == 0) {
        int_buf[0] = '0';
        int_buf[1] = 0;
    } else {
        int pos = 0;
        unsigned long n = integer_part;
        while (n > 0 && pos < 19) {
            int_buf[pos++] = '0' + (n % 10);
            n /= 10;
        }
        int_buf[pos] = 0;
        // Reverse
        for (int i = 0; i < pos / 2; i++) {
            char tmp = int_buf[i];
            int_buf[i] = int_buf[pos - 1 - i];
            int_buf[pos - 1 - i] = tmp;
        }
    }

    // Convert fractional part (up to 9 digits)
    char frac_buf[12];
    int frac_digits = 0;
    int non_zero_found = 0;
    for (int i = 0; i < 9; i++) {
        frac *= 10.0;
        int digit = (int)frac;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        frac -= (double)digit;
        frac_buf[i] = '0' + digit;
        frac_digits = i + 1;
        if (digit != 0) non_zero_found = 1;
        // Round check for next digit
        if (frac > 0.9999995 && (i + 1) < 9) {
            // Carry over
            frac_buf[i]++;
            if (frac_buf[i] > '9') {
                // Need to carry to previous digit or integer
                frac_buf[i] = '0';
                if (i > 0) {
                    frac_buf[i-1]++;
                } else {
                    integer_part++;
                }
            }
            // Remove trailing digits after rounding
            frac_digits = i + 1;
            break;
        }
    }
    frac_buf[frac_digits] = 0;

    // Remove trailing zeros from fractional part
    while (frac_digits > 0 && frac_buf[frac_digits - 1] == '0') {
        frac_buf[--frac_digits] = 0;
    }

    // Compose the final string
    int pos = 0;
    if (negative && pos < max_len - 1) buf[pos++] = '-';
    for (int i = 0; int_buf[i] && pos < max_len - 1; i++)
        buf[pos++] = int_buf[i];
    if (frac_digits > 0 && pos < max_len - 1) {
        buf[pos++] = '.';
        for (int i = 0; i < frac_digits && pos < max_len - 1; i++)
            buf[pos++] = frac_buf[i];
    }
    buf[pos] = 0;
}

// ============================================================================
// UPDATE THE DISPLAY STRING FROM THE CURRENT VALUE
// ============================================================================
static void calc_update_display(void) {
    if (calc.error) {
        strcpy(calc.display, "Error");
        return;
    }
    double_to_str(calc.current_value, calc.display, sizeof(calc.display) - 1);
}

// ============================================================================
// RESET CALCULATOR STATE
// ============================================================================
static void calc_reset(void) {
    calc.display[0] = '0';
    calc.display[1] = 0;
    calc.current_value = 0.0;
    calc.operand = 0.0;
    calc.operation = OP_NONE;
    calc.has_decimal = 0;
    calc.new_number = 1;
    calc.error = 0;
    calc.decimal_places = 0;
}

// ============================================================================
// EXECUTE THE PENDING OPERATION
// ============================================================================
static void calc_execute_op(void) {
    if (calc.operation == OP_NONE) return;
    if (calc.error) return;

    double result = 0.0;
    switch (calc.operation) {
        case OP_ADD:
            result = calc.operand + calc.current_value;
            break;
        case OP_SUB:
            result = calc.operand - calc.current_value;
            break;
        case OP_MUL:
            result = calc.operand * calc.current_value;
            break;
        case OP_DIV:
            if (calc.current_value == 0.0) {
                calc.error = 1;
                strcpy(calc.display, "Error");
                return;
            }
            result = calc.operand / calc.current_value;
            break;
        default:
            return;
    }
    calc.current_value = result;
    calc.operation = OP_NONE;
    calc_update_display();
}

// ============================================================================
// HANDLE DIGIT PRESS (0-9)
// ============================================================================
static void calc_press_digit(int digit) {
    if (calc.error) {
        calc_reset();
    }

    if (calc.new_number) {
        calc.current_value = (double)digit;
        calc.has_decimal = 0;
        calc.decimal_places = 0;
        calc.new_number = 0;
    } else {
        if (calc.has_decimal) {
            // Append decimal digit
            calc.decimal_places++;
            double frac = (double)digit;
            for (int i = 0; i < calc.decimal_places; i++)
                frac /= 10.0;
            calc.current_value += frac;
        } else {
            // Shift left and add digit
            if (calc.current_value < 0)
                calc.current_value = calc.current_value * 10.0 - digit;
            else
                calc.current_value = calc.current_value * 10.0 + digit;
        }
    }
    calc_update_display();
}

// ============================================================================
// HANDLE OPERATOR PRESS (+, -, *, /)
// ============================================================================
static void calc_press_operator(int op) {
    if (calc.error) return;

    // If there's a pending operation, execute it first
    if (calc.operation != OP_NONE && !calc.new_number) {
        calc_execute_op();
        if (calc.error) return;
    }

    calc.operand = calc.current_value;
    calc.operation = op;
    calc.new_number = 1;
    calc.has_decimal = 0;
    calc.decimal_places = 0;
}

// ============================================================================
// HANDLE EQUALS PRESS
// ============================================================================
static void calc_press_equals(void) {
    if (calc.error) return;

    if (calc.operation != OP_NONE) {
        calc_execute_op();
    }
    calc.new_number = 1;
    calc.has_decimal = 0;
    calc.decimal_places = 0;
}

// ============================================================================
// HANDLE DECIMAL POINT
// ============================================================================
static void calc_press_decimal(void) {
    if (calc.error) {
        calc_reset();
    }

    if (calc.new_number) {
        calc.current_value = 0.0;
        calc.new_number = 0;
    }
    if (!calc.has_decimal) {
        calc.has_decimal = 1;
        calc.decimal_places = 0;
        // Update display to show the decimal point
        calc_update_display();
        // Append "." if not already present
        int len = strlen(calc.display);
        if (len > 0 && len < (int)sizeof(calc.display) - 2) {
            // Check if display already ends with "."
            if (calc.display[len - 1] != '.') {
                calc.display[len] = '.';
                calc.display[len + 1] = 0;
            }
        }
    }
}

// ============================================================================
// HANDLE CLEAR (C)
// ============================================================================
static void calc_press_clear(void) {
    calc_reset();
}

// ============================================================================
// HANDLE NEGATE (+/-)
// ============================================================================
static void calc_press_negate(void) {
    if (calc.error) return;
    if (calc.current_value != 0.0) {
        calc.current_value = -calc.current_value;
        calc_update_display();
    }
}

// ============================================================================
// HANDLE PERCENT (%)
// ============================================================================
static void calc_press_percent(void) {
    if (calc.error) return;
    calc.current_value = calc.current_value / 100.0;
    calc_update_display();
    calc.new_number = 1;
}

// ============================================================================
// HANDLE BACKSPACE (keyboard only)
// ============================================================================
static void calc_press_backspace(void) {
    if (calc.error) {
        calc_reset();
        return;
    }
    if (calc.new_number) return;

    // Simple approach: remove last digit from display, reparse
    int len = strlen(calc.display);
    if (len > 1) {
        // Remove last character
        calc.display[len - 1] = 0;
        // If we removed the digit after a decimal point, clear decimal state
        int new_len = strlen(calc.display);
        if (new_len > 0 && calc.display[new_len - 1] == '.') {
            calc.has_decimal = 0;
            calc.decimal_places = 0;
        }
        // Re-parse the display string back into current_value
        // Simple integer parsing
        double val = 0.0;
        int neg = 0;
        int i = 0;
        if (calc.display[0] == '-') { neg = 1; i = 1; }
        int decimal_seen = 0;
        double decimal_div = 1.0;
        for (; calc.display[i]; i++) {
            if (calc.display[i] == '.') {
                decimal_seen = 1;
            } else if (calc.display[i] >= '0' && calc.display[i] <= '9') {
                if (decimal_seen) {
                    decimal_div *= 10.0;
                    val += (double)(calc.display[i] - '0') / decimal_div;
                } else {
                    val = val * 10.0 + (calc.display[i] - '0');
                }
            }
        }
        if (neg) val = -val;
        calc.current_value = val;
    } else {
        calc.current_value = 0.0;
        calc.display[0] = '0';
        calc.display[1] = 0;
    }
}

// ============================================================================
// HANDLE A BUTTON PRESS BY ID
// ============================================================================
static void calc_handle_button(int btn_id) {
    switch (btn_id) {
        case BID_0: case BID_1: case BID_2: case BID_3: case BID_4:
        case BID_5: case BID_6: case BID_7: case BID_8: case BID_9:
            calc_press_digit(btn_id);
            break;
        case BID_ADD:
            calc_press_operator(OP_ADD);
            break;
        case BID_SUB:
            calc_press_operator(OP_SUB);
            break;
        case BID_MUL:
            calc_press_operator(OP_MUL);
            break;
        case BID_DIV:
            calc_press_operator(OP_DIV);
            break;
        case BID_EQUALS:
            calc_press_equals();
            break;
        case BID_C:
            calc_press_clear();
            break;
        case BID_NEGATE:
            calc_press_negate();
            break;
        case BID_PERCENT:
            calc_press_percent();
            break;
        case BID_DECIMAL:
            calc_press_decimal();
            break;
    }
}

// ============================================================================
// GET OPERATION SYMBOL FOR INDICATOR
// ============================================================================
static const char* calc_op_symbol(int op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        default:     return "";
    }
}

// ============================================================================
// PAINT CALLBACK — draw the entire calculator UI
// ============================================================================
static void calculator_on_paint(window_t* win, int x, int y, int w, int h) {
    (void)win;

    // ---- Dark body background ----
    gfx_fill_rect(x, y, w, h, COL_BODY_BG);

    // ---- Display area ----
    gfx_fill_rect(x, y, w, DISPLAY_H, COL_DISPLAY_BG);

    // Subtle separator line between display and buttons
    gfx_fill_rect(x, y + DISPLAY_H - 1, w, 1, 0xFF3A3A3C);

    // Operation indicator (top-left of display)
    if (calc.operation != OP_NONE) {
        const char* sym = calc_op_symbol(calc.operation);
        gfx_draw_string(x + 14, y + 12, sym, COL_OP_INDICATOR);
    }

    // Display text — right-aligned, scaled based on length
    int display_len = strlen(calc.display);
    int scale = 3;
    if (display_len > 6)  scale = 2;
    if (display_len > 10) scale = 2;
    if (display_len > 14) scale = 1;

    int text_pixel_w = display_len * 8 * scale;
    int text_x = x + w - MARGIN - text_pixel_w;
    int text_y = y + DISPLAY_H - 10 - 16 * scale;

    // Ensure text doesn't overflow left
    if (text_x < x + 8) text_x = x + 8;

    gfx_draw_string_scaled(text_x, text_y, calc.display, COL_DISPLAY_TEXT, scale);

    // ---- Draw buttons ----
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        calc_button_t* b = &buttons[i];
        int bx, by, bw, bh;
        calc_button_rect(b, &bx, &by, &bw, &bh);

        int abs_x = x + bx;
        int abs_y = y + by;

        // Choose button color based on type and active state
        uint32_t btn_color;
        uint32_t text_color;

        if (b->type == BTN_DIGIT) {
            btn_color = COL_DIGIT_BTN;
            text_color = COL_TEXT_LIGHT;
        } else if (b->type == BTN_OPERATOR) {
            // Check if this operator is currently active (pending)
            int is_active_op = 0;
            if (calc.operation != OP_NONE && calc.new_number) {
                if ((calc.operation == OP_ADD && b->id == BID_ADD) ||
                    (calc.operation == OP_SUB && b->id == BID_SUB) ||
                    (calc.operation == OP_MUL && b->id == BID_MUL) ||
                    (calc.operation == OP_DIV && b->id == BID_DIV)) {
                    is_active_op = 1;
                }
            }
            if (is_active_op) {
                // White background, orange text (macOS active operator style)
                btn_color = COL_OP_BTN_ACTIVE;
                text_color = COL_OP_BTN;
            } else {
                btn_color = COL_OP_BTN;
                text_color = COL_TEXT_LIGHT;
            }
        } else {
            // Function button
            btn_color = COL_FUNC_BTN;
            text_color = COL_TEXT_LIGHT;
        }

        // Draw the rounded button
        gfx_fill_rounded_rect(abs_x, abs_y, bw, bh, btn_color, bh / 2);

        // Draw button label centered
        int label_w = strlen(b->label) * 8;
        // For scaled text, use scale=2 for single-char labels on operator/function buttons
        if (b->type == BTN_OPERATOR && strlen(b->label) == 1) {
            int scaled_w = label_w * 2;
            int lx = abs_x + (bw - scaled_w) / 2;
            int ly = abs_y + (bh - 16 * 2) / 2;
            gfx_draw_string_scaled(lx, ly, b->label, text_color, 2);
        } else if (b->type == BTN_DIGIT && strlen(b->label) == 1) {
            // Digits rendered at scale 2
            int scaled_w = label_w * 2;
            int lx = abs_x + (bw - scaled_w) / 2;
            int ly = abs_y + (bh - 16 * 2) / 2;
            gfx_draw_string_scaled(lx, ly, b->label, text_color, 2);
        } else {
            // Function labels at scale 1
            int lx = abs_x + (bw - label_w) / 2;
            int ly = abs_y + (bh - 16) / 2;
            gfx_draw_string(lx, ly, b->label, text_color);
        }
    }
}

// ============================================================================
// MOUSE CALLBACK — handle button clicks
// ============================================================================
static void calculator_on_mouse(window_t* win, int mx, int my, int btn) {
    (void)win;
    if (btn != 1) return;  // Only handle left clicks

    // Check each button for hit
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        calc_button_t* b = &buttons[i];
        int bx, by, bw, bh;
        calc_button_rect(b, &bx, &by, &bw, &bh);

        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
            calc_handle_button(b->id);
            return;  // Only one button can be hit
        }
    }
}

// ============================================================================
// INPUT CALLBACK — handle keyboard input
// ============================================================================
static void calculator_on_input(window_t* win, int key) {
    (void)win;

    if (key == 0) return;

    // Digit keys
    if (key >= '0' && key <= '9') {
        calc_press_digit(key - '0');
        return;
    }

    // Operators
    if (key == '+') { calc_press_operator(OP_ADD); return; }
    if (key == '-') { calc_press_operator(OP_SUB); return; }
    if (key == '*') { calc_press_operator(OP_MUL); return; }
    if (key == '/') { calc_press_operator(OP_DIV); return; }

    // Equals
    if (key == '=' || key == '\n' || key == '\r') {
        calc_press_equals();
        return;
    }

    // Decimal point
    if (key == '.') {
        calc_press_decimal();
        return;
    }

    // Backspace
    if (key == '\b') {
        calc_press_backspace();
        return;
    }

    // Clear (Escape or 'c')
    if (key == 27 || key == 'c' || key == 'C') {
        calc_press_clear();
        return;
    }

    // Percent
    if (key == '%') {
        calc_press_percent();
        return;
    }
}

// ============================================================================
// SCROLL CALLBACK (not used, but prevents crash if registered)
// ============================================================================
static void calculator_on_scroll(window_t* win, int delta) {
    (void)win; (void)delta;
}

// ============================================================================
// APP ENTRY POINT
// ============================================================================
void init_calculator_app(void) {
    // Initialize calculator state
    calc_reset();

    // Create the window — non-resizable, fixed size like macOS calculator
    calc_window = fw_create_window("Calculator", CALC_WIN_W, CALC_WIN_H,
                                    calculator_on_paint,
                                    calculator_on_input,
                                    calculator_on_mouse);
    if (!calc_window) return;

    // Set minimum size = fixed size (prevent resizing)
    ((window_t*)calc_window)->min_w = CALC_WIN_W;
    ((window_t*)calc_window)->min_h = CALC_WIN_H;
    ((window_t*)calc_window)->max_w = CALC_WIN_W;
    ((window_t*)calc_window)->max_h = CALC_WIN_H;

    // Wire up scroll callback (prevent crash from scroll events)
    ((window_t*)calc_window)->scroll_callback = (void*)calculator_on_scroll;

    // Register in dock
    fw_register_dock("Calculator", 5, calc_window);

    // Debug
    s_printf("[Calculator] App initialized\n");
}
