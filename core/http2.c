// core/http2.c - HTTP/2 Protocol Implementation with Real HPACK Huffman Decoding
// Based on RFC 7540 (HTTP/2) and RFC 7541 (HPACK)
#include "http2.h"
#include "socket.h"
#include "dns.h"
#include "tls.h" 
#include "tls13.h"
#include "memory.h"
#include "string.h"
#include "../hal/cpu/timer.h"

// External declarations
extern void rtl8139_poll(void);
extern void s_printf(const char* fmt, ...); 

static int local_atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// ============================================================================
// HPACK STATIC TABLE (RFC 7541 Appendix A)
// ============================================================================
#define HPACK_STATIC_TABLE_SIZE 61

// ============================================================================
// HPACK HUFFMAN DECODING (RFC 7541 Appendix B)
// ============================================================================

typedef struct {
    uint32_t code;    // The bit pattern (LSB-aligned)
    uint8_t  bits;    // Number of bits in the code
    uint16_t symbol;  // Decoded symbol (0-255), 256 = EOS
} huffman_entry_t;

static const huffman_entry_t huffman_table[] = {
    { 0x1ff8, 13, 0 },
    { 0x7fffd8, 23, 1 },
    { 0xfffffe2, 28, 2 },
    { 0xfffffe3, 28, 3 },
    { 0xfffffe4, 28, 4 },
    { 0xfffffe5, 28, 5 },
    { 0xfffffe6, 28, 6 },
    { 0xfffffe7, 28, 7 },
    { 0xfffffe8, 28, 8 },
    { 0xffffea, 24, 9 },
    { 0x3ffffffc, 30, 10 },
    { 0xfffffe9, 28, 11 },
    { 0xfffffea, 28, 12 },
    { 0x3ffffffd, 30, 13 },
    { 0xfffffeb, 28, 14 },
    { 0xfffffec, 28, 15 },
    { 0xfffffed, 28, 16 },
    { 0xfffffee, 28, 17 },
    { 0xfffffef, 28, 18 },
    { 0xffffff0, 28, 19 },
    { 0xffffff1, 28, 20 },
    { 0xffffff2, 28, 21 },
    { 0x3ffffffe, 30, 22 },
    { 0xffffff3, 28, 23 },
    { 0xffffff4, 28, 24 },
    { 0xffffff5, 28, 25 },
    { 0xffffff6, 28, 26 },
    { 0xffffff7, 28, 27 },
    { 0xffffff8, 28, 28 },
    { 0xffffff9, 28, 29 },
    { 0xffffffa, 28, 30 },
    { 0xffffffb, 28, 31 },
    { 0x14, 6, 32 },       // ' '
    { 0x3f8, 10, 33 },     // '!'
    { 0x3f9, 10, 34 },     // '"'
    { 0xffa, 12, 35 },     // '#'
    { 0x1ff9, 13, 36 },    // '$'
    { 0x15, 6, 37 },       // '%'
    { 0xf8, 8, 38 },       // '&'
    { 0x7fa, 11, 39 },     // '''
    { 0x3fa, 10, 40 },     // '('
    { 0x3fb, 10, 41 },     // ')'
    { 0xf9, 8, 42 },       // '*'
    { 0x7fb, 11, 43 },     // '+'
    { 0xfa, 8, 44 },       // ','
    { 0x16, 6, 45 },       // '-'
    { 0x17, 6, 46 },       // '.'
    { 0x18, 6, 47 },       // '/'
    { 0x0, 5, 48 },        // '0'
    { 0x1, 5, 49 },        // '1'
    { 0x2, 5, 50 },        // '2'
    { 0x19, 6, 51 },       // '3'
    { 0x1a, 6, 52 },       // '4'
    { 0x1b, 6, 53 },       // '5'
    { 0x1c, 6, 54 },       // '6'
    { 0x1d, 6, 55 },       // '7'
    { 0x1e, 6, 56 },       // '8'
    { 0x1f, 6, 57 },       // '9'
    { 0x5c, 7, 58 },       // ':'
    { 0xfb, 8, 59 },       // ';'
    { 0x7ffc, 15, 60 },    // '<'
    { 0x20, 6, 61 },       // '='
    { 0xffb, 12, 62 },     // '>'
    { 0x3fc, 10, 63 },     // '?'
    { 0x1ffa, 13, 64 },    // '@'
    { 0x21, 6, 65 },       // 'A'
    { 0x5d, 7, 66 },       // 'B'
    { 0x5e, 7, 67 },       // 'C'
    { 0x5f, 7, 68 },       // 'D'
    { 0x60, 7, 69 },       // 'E'
    { 0x61, 7, 70 },       // 'F'
    { 0x62, 7, 71 },       // 'G'
    { 0x63, 7, 72 },       // 'H'
    { 0x64, 7, 73 },       // 'I'
    { 0x65, 7, 74 },       // 'J'
    { 0x66, 7, 75 },       // 'K'
    { 0x67, 7, 76 },       // 'L'
    { 0x68, 7, 77 },       // 'M'
    { 0x69, 7, 78 },       // 'N'
    { 0x6a, 7, 79 },       // 'O'
    { 0x6b, 7, 80 },       // 'P'
    { 0x6c, 7, 81 },       // 'Q'
    { 0x6d, 7, 82 },       // 'R'
    { 0x6e, 7, 83 },       // 'S'
    { 0x6f, 7, 84 },       // 'T'
    { 0x70, 7, 85 },       // 'U'
    { 0x71, 7, 86 },       // 'V'
    { 0x72, 7, 87 },       // 'W'
    { 0xfc, 8, 88 },       // 'X'
    { 0x73, 7, 89 },       // 'Y'
    { 0xfd, 8, 90 },       // 'Z'
    { 0x1ffb, 13, 91 },    // '['
    { 0x7fff0, 19, 92 },   // '\'
    { 0x1ffc, 13, 93 },    // ']'
    { 0x3ffc, 14, 94 },    // '^'
    { 0x22, 6, 95 },       // '_'
    { 0x7ffd, 15, 96 },    // '`'
    { 0x3, 5, 97 },        // 'a'
    { 0x23, 6, 98 },       // 'b'
    { 0x4, 5, 99 },        // 'c'
    { 0x24, 6, 100 },      // 'd'
    { 0x5, 5, 101 },       // 'e'
    { 0x25, 6, 102 },      // 'f'
    { 0x26, 6, 103 },      // 'g'
    { 0x27, 6, 104 },      // 'h'
    { 0x6, 5, 105 },       // 'i'
    { 0x74, 7, 106 },      // 'j'
    { 0x75, 7, 107 },      // 'k'
    { 0x28, 6, 108 },      // 'l'
    { 0x29, 6, 109 },      // 'm'
    { 0x2a, 6, 110 },      // 'n'
    { 0x7, 5, 111 },       // 'o'
    { 0x2b, 6, 112 },      // 'p'
    { 0x76, 7, 113 },      // 'q'
    { 0x2c, 6, 114 },      // 'r'
    { 0x8, 5, 115 },       // 's'
    { 0x9, 5, 116 },       // 't'
    { 0x2d, 6, 117 },      // 'u'
    { 0x77, 7, 118 },      // 'v'
    { 0x78, 7, 119 },      // 'w'
    { 0x79, 7, 120 },      // 'x'
    { 0x7a, 7, 121 },      // 'y'
    { 0x7b, 7, 122 },      // 'z'
    { 0x7ffe, 15, 123 },   // '{'
    { 0x7fc, 11, 124 },    // '|'
    { 0x3ffd, 14, 125 },   // '}'
    { 0x1ffd, 13, 126 },   // '~'
    { 0xffffffc, 28, 127 },
    { 0xfffe6, 20, 128 },
    { 0x3fffd2, 22, 129 },
    { 0xfffe7, 20, 130 },
    { 0xfffe8, 20, 131 },
    { 0x3fffd3, 22, 132 },
    { 0x3fffd4, 22, 133 },
    { 0x3fffd5, 22, 134 },
    { 0x7fffd9, 23, 135 },
    { 0x3fffd6, 22, 136 },
    { 0x7fffda, 23, 137 },
    { 0x7fffdb, 23, 138 },
    { 0x7fffdc, 23, 139 },
    { 0x7fffdd, 23, 140 },
    { 0x7fffde, 23, 141 },
    { 0xffffeb, 24, 142 },
    { 0x7fffdf, 23, 143 },
    { 0xffffec, 24, 144 },
    { 0xffffed, 24, 145 },
    { 0x3fffd7, 22, 146 },
    { 0x7fffe0, 23, 147 },
    { 0xffffee, 24, 148 },
    { 0x7fffe1, 23, 149 },
    { 0x7fffe2, 23, 150 },
    { 0x7fffe3, 23, 151 },
    { 0x1fffdc, 21, 152 },
    { 0x3fffd8, 22, 153 },
    { 0x7fffe5, 23, 154 },
    { 0x3fffd9, 22, 155 },
    { 0x7fffe6, 23, 156 },
    { 0x7fffe7, 23, 157 },
    { 0xffffef, 24, 158 },
    { 0x3fffda, 22, 159 },
    { 0x1fffdd, 21, 160 },
    { 0xfffe9, 20, 161 },
    { 0x3fffdb, 22, 162 },
    { 0x3fffdc, 22, 163 },
    { 0x7fffe8, 23, 164 },
    { 0x7fffe9, 23, 165 },
    { 0x1fffde, 21, 166 },
    { 0x7fffea, 23, 167 },
    { 0x3fffdd, 22, 168 },
    { 0x3fffde, 22, 169 },
    { 0xfffff0, 24, 170 },
    { 0x1fffdf, 21, 171 },
    { 0x3fffdf, 22, 172 },
    { 0x7fffeb, 23, 173 },
    { 0x7fffec, 23, 174 },
    { 0x1fffe0, 21, 175 },
    { 0x1fffe1, 21, 176 },
    { 0x3fffe0, 22, 177 },
    { 0x1fffe2, 21, 178 },
    { 0x7fffed, 23, 179 },
    { 0x3fffe1, 22, 180 },
    { 0x7fffee, 23, 181 },
    { 0x7fffef, 23, 182 },
    { 0xfffea, 20, 183 },
    { 0x3fffe2, 22, 184 },
    { 0x3fffe3, 22, 185 },
    { 0x3fffe4, 22, 186 },
    { 0x7ffff0, 23, 187 },
    { 0x3fffe5, 22, 188 },
    { 0x3fffe6, 22, 189 },
    { 0x7ffff1, 23, 190 },
    { 0x3ffffe0, 26, 191 },
    { 0x3ffffe1, 26, 192 },
    { 0xfffeb, 20, 193 },
    { 0x7fff1, 19, 194 },
    { 0x3fffe7, 22, 195 },
    { 0x7ffff2, 23, 196 },
    { 0x3fffe8, 22, 197 },
    { 0x1ffffec, 25, 198 },
    { 0x3ffffe2, 26, 199 },
    { 0x3ffffe3, 26, 200 },
    { 0x3ffffe4, 26, 201 },
    { 0x7ffffde, 27, 202 },
    { 0x7ffffdf, 27, 203 },
    { 0x3ffffe5, 26, 204 },
    { 0xfffff1, 24, 205 },
    { 0x1ffffed, 25, 206 },
    { 0x7fff2, 19, 207 },
    { 0x1fffe3, 21, 208 },
    { 0x3ffffe6, 26, 209 },
    { 0x7ffffe0, 27, 210 },
    { 0x7ffffe1, 27, 211 },
    { 0x3ffffe7, 26, 212 },
    { 0x7ffffe2, 27, 213 },
    { 0xfffff2, 24, 214 },
    { 0x1fffe4, 21, 215 },
    { 0x1fffe5, 21, 216 },
    { 0x3ffffe8, 26, 217 },
    { 0x3ffffe9, 26, 218 },
    { 0xffffffd, 28, 219 },
    { 0x7ffffe3, 27, 220 },
    { 0x7ffffe4, 27, 221 },
    { 0x7ffffe5, 27, 222 },
    { 0xfffec, 20, 223 },
    { 0xfffff3, 24, 224 },
    { 0xfffed, 20, 225 },
    { 0x1fffe6, 21, 226 },
    { 0x3fffe9, 22, 227 },
    { 0x1fffe7, 21, 228 },
    { 0x1fffe8, 21, 229 },
    { 0x7ffff3, 23, 230 },
    { 0x3fffea, 22, 231 },
    { 0x3fffeb, 22, 232 },
    { 0x1ffffee, 25, 233 },
    { 0x1ffffef, 25, 234 },
    { 0xfffff4, 24, 235 },
    { 0xfffff5, 24, 236 },
    { 0x3ffffea, 26, 237 },
    { 0x7ffff4, 23, 238 },
    { 0x3ffffeb, 26, 239 },
    { 0x7ffffe6, 27, 240 },
    { 0x3ffffec, 26, 241 },
    { 0x3ffffed, 26, 242 },
    { 0x7ffffe7, 27, 243 },
    { 0x7ffffe8, 27, 244 },
    { 0x7ffffe9, 27, 245 },
    { 0x7ffffea, 27, 246 },
    { 0x7ffffeb, 27, 247 },
    { 0xffffffe, 28, 248 },
    { 0x7ffffec, 27, 249 },
    { 0x7ffffed, 27, 250 },
    { 0x7ffffee, 27, 251 },
    { 0x7ffffef, 27, 252 },
    { 0x7fffff0, 27, 253 },
    { 0x3ffffee, 26, 254 },
    { 0x3fffffff, 30, 256 },  // EOS
};

#define HUFFMAN_TABLE_SIZE (sizeof(huffman_table)/sizeof(huffman_table[0]))

static int hpack_huffman_decode(const uint8_t* input, size_t input_len,
                                 char* output, size_t max_output) {
    size_t out_idx = 0;
    uint32_t bits_accum = 0;
    int bits_count = 0;

    for (size_t i = 0; i < input_len && out_idx < max_output - 1; i++) {
        uint8_t byte = input[i];
        for (int bit_pos = 7; bit_pos >= 0; bit_pos--) {
            uint8_t bit = (byte >> bit_pos) & 1;
            bits_accum = (bits_accum << 1) | bit;
            bits_count++;

            // Try to match against the Huffman table
            int found = 0;
            for (size_t t = 0; t < HUFFMAN_TABLE_SIZE; t++) {
                if (huffman_table[t].bits == (uint8_t)bits_count &&
                    huffman_table[t].code == (bits_accum & ((1u << bits_count) - 1))) {
                    if (huffman_table[t].symbol == 256) {
                        // EOS — should not appear in valid data
                        output[out_idx] = '\0';
                        return (int)out_idx;
                    }
                    output[out_idx++] = (char)huffman_table[t].symbol;
                    bits_count = 0;
                    bits_accum = 0;
                    found = 1;
                    break;
                }
            }
            if (found) continue;

            // If we've accumulated 30 bits without a match, the data is corrupt
            if (bits_count >= 30) {
                output[out_idx] = '\0';
                return (int)out_idx;
            }
        }
    }

    // Trailing bits should be all 1s (EOS padding per RFC 7541 §5.2).
    // Validate: remaining bits must be <= 7 and all ones.
    if (bits_count > 0) {
        uint32_t mask = (1u << bits_count) - 1;
        if ((bits_accum & mask) != mask || bits_count > 7) {
            // Invalid padding — but still return what we decoded
        }
    }

    output[out_idx] = '\0';
    return (int)out_idx;
}

// ============================================================================
// HPACK STRING ENCODING/DECODING
// ============================================================================

int hpack_encode_string(uint8_t* output, const char* str, size_t max_len) {
    size_t len = strlen(str);
    if (len < 127) {
        if (len + 1 > max_len) return -1;
        output[0] = (uint8_t)(len & 0x7F);
        memcpy(output + 1, str, len);
        return (int)(len + 1);
    } else {
        if (len + 6 > max_len) return -1;
        output[0] = 0x7F;
        size_t rem = len - 127;
        int idx = 1;
        while (rem >= 128) {
            output[idx++] = (uint8_t)((rem & 0x7F) | 0x80);
            rem >>= 7;
        }
        output[idx++] = (uint8_t)rem;
        memcpy(output + idx, str, len);
        return (int)(idx + len);
    }
}

int hpack_decode_string(const uint8_t* input, size_t input_len,
                        char* str, size_t max_len, size_t* consumed) {
    if (input_len < 1) return -1;

    int is_huffman = (input[0] & 0x80) != 0;
    size_t str_len = input[0] & 0x7F;
    size_t header_len = 1;

    if (str_len == 0x7F) {
        int shift = 0;
        str_len = 0;
        uint8_t b;
        do {
            if (header_len >= input_len) return -1;
            b = input[header_len++];
            str_len += (size_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        str_len += 0x7F;
    }

    if (header_len + str_len > input_len) return -1;
    if (str_len >= max_len) return -1;

    if (is_huffman) {
        int decoded_len = hpack_huffman_decode(input + header_len, str_len, str, max_len);
        if (decoded_len < 0) {
            size_t copy_len = (str_len < max_len - 1) ? str_len : (max_len - 1);
            memcpy(str, input + header_len, copy_len);
            str[copy_len] = '\0';
        }
    } else {
        memcpy(str, input + header_len, str_len);
        str[str_len] = '\0';
    }

    if (consumed) *consumed = header_len + str_len;
    return 0;
}

// ============================================================================
// HPACK INTEGER ENCODING (RFC 7541 §5.1)
// ============================================================================

int hpack_encode_int(uint8_t* output, uint64_t value, int prefix_bits) {
    uint8_t prefix_mask = (uint8_t)((1 << prefix_bits) - 1);
    if (value < prefix_mask) {
        output[0] = (uint8_t)value;
        return 1;
    }
    output[0] = prefix_mask;
    value -= prefix_mask;
    int len = 1;
    while (value >= 128) {
        output[len++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    output[len++] = (uint8_t)value;
    return len;
}

int hpack_decode_int(const uint8_t* input, size_t input_len,
                     uint64_t* value, int prefix_bits, size_t* consumed) {
    if (input_len < 1) return -1;
    uint8_t prefix_mask = (uint8_t)((1 << prefix_bits) - 1);
    *value = input[0] & prefix_mask;
    if (*value < prefix_mask) {
        *consumed = 1;
        return 0;
    }
    int shift = 0;
    size_t i = 1;
    while (i < input_len) {
        uint8_t b = input[i];
        *value += (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        i++;
        if ((b & 0x80) == 0) break;
        if (shift > 63) return -1;
    }
    *consumed = i;
    return 0;
}

// ============================================================================
// HPACK DYNAMIC TABLE
// ============================================================================

void hpack_init_table(hpack_dynamic_table_t* table, size_t max_size) {
    memset(table, 0, sizeof(hpack_dynamic_table_t));
    table->max_size = max_size;
    table->current_size = 0;
    table->head = NULL;
    table->tail = NULL;
}

void hpack_free_table(hpack_dynamic_table_t* table) {
    hpack_dynamic_entry_t* entry = table->head;
    while (entry) {
        hpack_dynamic_entry_t* next = entry->next;
        if (entry->name) kfree(entry->name);
        if (entry->value) kfree(entry->value);
        kfree(entry);
        entry = next;
    }
    memset(table, 0, sizeof(hpack_dynamic_table_t));
}

int hpack_add_entry(hpack_dynamic_table_t* table, const char* name, const char* value) {
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    size_t entry_size = name_len + value_len + 32;

    while (table->tail && table->current_size + entry_size > table->max_size) {
        hpack_dynamic_entry_t* old = table->tail;
        table->current_size -= old->size;
        if (old->prev) {
            old->prev->next = NULL;
        } else {
            table->head = NULL;
        }
        table->tail = old->prev;
        if (old->name) kfree(old->name);
        if (old->value) kfree(old->value);
        kfree(old);
    }

    if (entry_size > table->max_size) {
        return -1;
    }

    hpack_dynamic_entry_t* entry = (hpack_dynamic_entry_t*)kmalloc(sizeof(hpack_dynamic_entry_t));
    if (!entry) return -1;
    entry->name = (char*)kmalloc(name_len + 1);
    entry->value = (char*)kmalloc(value_len + 1);
    if (!entry->name || !entry->value) {
        if (entry->name) kfree(entry->name);
        if (entry->value) kfree(entry->value);
        kfree(entry);
        return -1;
    }
    strcpy(entry->name, name);
    strcpy(entry->value, value);
    entry->size = entry_size;

    entry->next = table->head;
    entry->prev = NULL;
    if (table->head) {
        table->head->prev = entry;
    } else {
        table->tail = entry;
    }
    table->head = entry;
    table->current_size += entry_size;
    return 0;
}

// ============================================================================
// HPACK ENCODING
// ============================================================================

int hpack_encode(hpack_dynamic_table_t* table, const http2_header_t* headers, int count,
                 uint8_t* output, size_t output_max) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        const http2_header_t* h = &headers[i];
        size_t name_len = strlen(h->name);
        size_t value_len = strlen(h->value);

        // Check static table for indexed representation
        int indexed = 0;
        for (int j = 0; j < HPACK_STATIC_TABLE_SIZE; j++) {
            if (strcmp(h->name, hpack_static_table[j].name) == 0) {
                if (hpack_static_table[j].value[0] != '\0' &&
                    strcmp(h->value, hpack_static_table[j].value) == 0) {
                    if (pos + 2 > output_max) return -1;
                    output[pos++] = (uint8_t)(0x80 | (j + 1));
                    indexed = 1;
                    break;
                }
            }
        }

        if (!indexed) {
            // Literal header field with incremental indexing (0x40 prefix)
            size_t needed = 1 + 1 + name_len + 1 + value_len;
            if (pos + needed > output_max) return -1;
            output[pos++] = 0x40;
            output[pos++] = (uint8_t)(name_len & 0x7F);
            memcpy(output + pos, h->name, name_len);
            pos += name_len;
            output[pos++] = (uint8_t)(value_len & 0x7F);
            memcpy(output + pos, h->value, value_len);
            pos += value_len;
            hpack_add_entry(table, h->name, h->value);
        }
    }
    return (int)pos;
}

// ============================================================================
// HPACK DECODING
// ============================================================================

int hpack_decode(hpack_dynamic_table_t* table, const uint8_t* input, size_t input_len,
                 http2_header_t* headers, int max_headers, int* header_count) {
    size_t pos = 0;
    *header_count = 0;

    while (pos < input_len && *header_count < max_headers) {
        uint8_t b = input[pos];
        http2_header_t* h = &headers[*header_count];

        if (b & 0x80) {
            // Indexed header field
            uint64_t index;
            size_t consumed;
            if (hpack_decode_int(input + pos, input_len - pos, &index, 7, &consumed) < 0)
                return -1;
            if (index == 0) return -1;

            if (index <= HPACK_STATIC_TABLE_SIZE) {
                strcpy(h->name, hpack_static_table[index - 1].name);
                strcpy(h->value, hpack_static_table[index - 1].value);
            } else {
                int dyn_index = (int)(index - HPACK_STATIC_TABLE_SIZE - 1);
                hpack_dynamic_entry_t* entry = table->head;
                for (int i = 0; i < dyn_index && entry; i++)
                    entry = entry->next;
                if (!entry) return -1;
                strcpy(h->name, entry->name);
                strcpy(h->value, entry->value);
            }
            pos += consumed;
            h->is_indexed = 1;
            h->is_pseudo = (h->name[0] == ':');
            (*header_count)++;
        }
        else if ((b & 0xC0) == 0x40) {
            // Literal with incremental indexing
            uint64_t name_index = 0;
            size_t consumed;

            if ((b & 0x3F) != 0) {
                if (hpack_decode_int(input + pos, input_len - pos, &name_index, 6, &consumed) < 0)
                    return -1;
                pos += consumed;
                if (name_index <= HPACK_STATIC_TABLE_SIZE) {
                    strcpy(h->name, hpack_static_table[name_index - 1].name);
                } else {
                    int di = (int)(name_index - HPACK_STATIC_TABLE_SIZE - 1);
                    hpack_dynamic_entry_t* e = table->head;
                    for (int i = 0; i < di && e; i++) e = e->next;
                    if (!e) return -1;
                    strcpy(h->name, e->name);
                }
            } else {
                pos++;
                if (hpack_decode_string(input + pos, input_len - pos, h->name, sizeof(h->name), &consumed) < 0)
                    return -1;
                pos += consumed;
            }

            if (hpack_decode_string(input + pos, input_len - pos, h->value, sizeof(h->value), &consumed) < 0)
                return -1;
            pos += consumed;

            hpack_add_entry(table, h->name, h->value);
            h->is_indexed = 0;
            h->is_pseudo = (h->name[0] == ':');
            (*header_count)++;
        }
        else if ((b & 0xF0) == 0x00) {
            // Literal without indexing
            uint64_t name_index = 0;
            size_t consumed;

            if ((b & 0x0F) != 0) {
                if (hpack_decode_int(input + pos, input_len - pos, &name_index, 4, &consumed) < 0)
                    return -1;
                pos += consumed;
                if (name_index <= HPACK_STATIC_TABLE_SIZE) {
                    strcpy(h->name, hpack_static_table[name_index - 1].name);
                } else {
                    int di = (int)(name_index - HPACK_STATIC_TABLE_SIZE - 1);
                    hpack_dynamic_entry_t* e = table->head;
                    for (int i = 0; i < di && e; i++) e = e->next;
                    if (!e) return -1;
                    strcpy(h->name, e->name);
                }
            } else {
                pos++;
                if (hpack_decode_string(input + pos, input_len - pos, h->name, sizeof(h->name), &consumed) < 0)
                    return -1;
                pos += consumed;
            }

            if (hpack_decode_string(input + pos, input_len - pos, h->value, sizeof(h->value), &consumed) < 0)
                return -1;
            pos += consumed;

            h->is_indexed = 0;
            h->is_pseudo = (h->name[0] == ':');
            (*header_count)++;
        }
        else if ((b & 0xF0) == 0x10) {
            // Literal never indexed
            uint64_t name_index = 0;
            size_t consumed;

            if ((b & 0x0F) != 0) {
                if (hpack_decode_int(input + pos, input_len - pos, &name_index, 4, &consumed) < 0)
                    return -1;
                pos += consumed;
                if (name_index <= HPACK_STATIC_TABLE_SIZE) {
                    strcpy(h->name, hpack_static_table[name_index - 1].name);
                } else {
                    int di = (int)(name_index - HPACK_STATIC_TABLE_SIZE - 1);
                    hpack_dynamic_entry_t* e = table->head;
                    for (int i = 0; i < di && e; i++) e = e->next;
                    if (!e) return -1;
                    strcpy(h->name, e->name);
                }
            } else {
                pos++;
                if (hpack_decode_string(input + pos, input_len - pos, h->name, sizeof(h->name), &consumed) < 0)
                    return -1;
                pos += consumed;
            }

            if (hpack_decode_string(input + pos, input_len - pos, h->value, sizeof(h->value), &consumed) < 0)
                return -1;
            pos += consumed;

            h->is_indexed = 0;
            h->is_pseudo = (h->name[0] == ':');
            (*header_count)++;
        }
        else if ((b & 0xE0) == 0x20) {
            // Dynamic table size update
            uint64_t new_size;
            size_t consumed;
            if (hpack_decode_int(input + pos, input_len - pos, &new_size, 5, &consumed) < 0)
                return -1;
            pos += consumed;
            table->max_size = (size_t)new_size;
            while (table->tail && table->current_size > table->max_size) {
                hpack_dynamic_entry_t* old = table->tail;
                table->current_size -= old->size;
                if (old->prev) old->prev->next = NULL;
                else table->head = NULL;
                table->tail = old->prev;
                if (old->name) kfree(old->name);
                if (old->value) kfree(old->value);
                kfree(old);
            }
        }
        else {
            return -1;
        }
    }
    return 0;
}

// ============================================================================
// HTTP/2 FRAME FUNCTIONS
// ============================================================================

// Write to connection (handles TLS 1.2, TLS 1.3, or plain TCP)
static int http2_write(http2_connection_t* conn, const void* data, size_t len) {
    if (conn->use_tls && conn->tls_session) {
        if (conn->tls_version == 12) {
            return tls_write((tls_session_t*)conn->tls_session, data, len);
        } else {
            return tls13_write((tls13_session_t*)conn->tls_session, data, len);
        }
    }
    return k_sendto(conn->socket_fd, data, len, 0, NULL);
}

// ============================================================================
// BUFFERED READ — fixes the TLS record / HTTP/2 frame boundary mismatch.
//
// tls_read() consumes one entire TLS record per call, but
// http2_receive_frame needs to read exactly 9 bytes (frame header)
// then exactly N bytes (payload).  Without buffering, the bytes
// beyond the first 9 in a TLS record were silently discarded.
//
// This implementation caches leftover bytes in conn->read_cache so
// subsequent http2_read calls are served from the cache first.
// ============================================================================
static int http2_read(http2_connection_t* conn, void* buffer, size_t len) {
    uint8_t* out = (uint8_t*)buffer;
    size_t total = 0;

    while (total < len) {
        // Serve from cache first
        if (conn->read_cache_pos < conn->read_cache_len) {
            size_t avail = conn->read_cache_len - conn->read_cache_pos;
            size_t copy = (len - total < avail) ? (len - total) : avail;
            memcpy(out + total, conn->read_cache + conn->read_cache_pos, copy);
            conn->read_cache_pos += copy;
            total += copy;
            continue;
        }

        // Cache exhausted — read next TLS record (or TCP segment) into cache
        int n;
        if (conn->use_tls && conn->tls_session) {
            if (conn->tls_version == 12)
                n = tls_read((tls_session_t*)conn->tls_session,
                             conn->read_cache, sizeof(conn->read_cache));
            else
                n = tls13_read((tls13_session_t*)conn->tls_session,
                               conn->read_cache, sizeof(conn->read_cache));
        } else {
            n = k_recvfrom(conn->socket_fd, conn->read_cache,
                           sizeof(conn->read_cache), 0, NULL);
        }

        if (n <= 0) return (total > 0) ? (int)total : -1;
        conn->read_cache_len = (size_t)n;
        conn->read_cache_pos = 0;
    }
    return (int)total;
}

// ============================================================================
// SEND FRAME — sends header + payload in a SINGLE tls_write / k_sendto call.
//
// The old version made two separate http2_write calls (one for the 9-byte
// header, one for the payload), creating two TLS records per HTTP/2 frame.
// Google's server rejects this as a protocol violation.
// ============================================================================
static int http2_send_frame(http2_connection_t* conn, uint8_t type, uint8_t flags,
                            uint32_t stream_id, const uint8_t* payload, size_t payload_len) {
    size_t total = 9 + payload_len;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return -1;

    buf[0] = (uint8_t)((payload_len >> 16) & 0xFF);
    buf[1] = (uint8_t)((payload_len >> 8) & 0xFF);
    buf[2] = (uint8_t)(payload_len & 0xFF);
    buf[3] = type;
    buf[4] = flags;
    buf[5] = (uint8_t)((stream_id >> 24) & 0x7F);
    buf[6] = (uint8_t)((stream_id >> 16) & 0xFF);
    buf[7] = (uint8_t)((stream_id >> 8) & 0xFF);
    buf[8] = (uint8_t)(stream_id & 0xFF);

    if (payload_len > 0 && payload)
        memcpy(buf + 9, payload, payload_len);

    int ret = http2_write(conn, buf, total);
    kfree(buf);
    return (ret < 0) ? -1 : 0;
}

int http2_send_settings(http2_connection_t* conn) {
    uint8_t settings[36];
    int pos = 0;
    // HEADER_TABLE_SIZE = 4096
    settings[pos++] = 0x00; settings[pos++] = 0x01;
    settings[pos++] = 0x00; settings[pos++] = 0x00; settings[pos++] = 0x10; settings[pos++] = 0x00;
    // MAX_CONCURRENT_STREAMS = 100
    settings[pos++] = 0x00; settings[pos++] = 0x03;
    settings[pos++] = 0x00; settings[pos++] = 0x00; settings[pos++] = 0x00; settings[pos++] = 0x64;
    // INITIAL_WINDOW_SIZE = 65536
    settings[pos++] = 0x00; settings[pos++] = 0x04;
    settings[pos++] = 0x00; settings[pos++] = 0x01; settings[pos++] = 0x00; settings[pos++] = 0x00;
    // MAX_FRAME_SIZE = 16384
    settings[pos++] = 0x00; settings[pos++] = 0x05;
    settings[pos++] = 0x00; settings[pos++] = 0x00; settings[pos++] = 0x40; settings[pos++] = 0x00;
    return http2_send_frame(conn, HTTP2_FRAME_SETTINGS, 0, 0, settings, pos);
}

int http2_send_settings_ack(http2_connection_t* conn) {
    return http2_send_frame(conn, HTTP2_FRAME_SETTINGS, HTTP2_FLAG_ACK, 0, NULL, 0);
}

int http2_send_ping(http2_connection_t* conn, const uint8_t* data) {
    return http2_send_frame(conn, HTTP2_FRAME_PING, 0, 0, data, 8);
}

int http2_send_ping_ack(http2_connection_t* conn, const uint8_t* data) {
    return http2_send_frame(conn, HTTP2_FRAME_PING, HTTP2_FLAG_ACK, 0, data, 8);
}

int http2_send_headers(http2_connection_t* conn, uint32_t stream_id,
                       const http2_header_t* headers, int header_count, int end_stream) {
    uint8_t header_block[4096];
    int block_len = hpack_encode(&conn->encoder_table, headers, header_count,
                                 header_block, sizeof(header_block));
    if (block_len < 0) return -1;
    uint8_t flags = HTTP2_FLAG_END_HEADERS;
    if (end_stream) flags |= HTTP2_FLAG_END_STREAM;
    return http2_send_frame(conn, HTTP2_FRAME_HEADERS, flags, stream_id,
                            header_block, (size_t)block_len);
}

int http2_send_data(http2_connection_t* conn, uint32_t stream_id,
                    const uint8_t* data, size_t len, int end_stream) {
    uint8_t flags = end_stream ? HTTP2_FLAG_END_STREAM : 0;
    return http2_send_frame(conn, HTTP2_FRAME_DATA, flags, stream_id, data, len);
}

int http2_send_rst_stream(http2_connection_t* conn, uint32_t stream_id, uint32_t error_code) {
    uint8_t payload[4];
    payload[0] = (uint8_t)((error_code >> 24) & 0xFF);
    payload[1] = (uint8_t)((error_code >> 16) & 0xFF);
    payload[2] = (uint8_t)((error_code >> 8) & 0xFF);
    payload[3] = (uint8_t)(error_code & 0xFF);
    return http2_send_frame(conn, HTTP2_FRAME_RST_STREAM, 0, stream_id, payload, 4);
}

int http2_send_goaway(http2_connection_t* conn, uint32_t last_stream_id, uint32_t error_code) {
    uint8_t payload[8];
    payload[0] = (uint8_t)((last_stream_id >> 24) & 0x7F);
    payload[1] = (uint8_t)((last_stream_id >> 16) & 0xFF);
    payload[2] = (uint8_t)((last_stream_id >> 8) & 0xFF);
    payload[3] = (uint8_t)(last_stream_id & 0xFF);
    payload[4] = (uint8_t)((error_code >> 24) & 0xFF);
    payload[5] = (uint8_t)((error_code >> 16) & 0xFF);
    payload[6] = (uint8_t)((error_code >> 8) & 0xFF);
    payload[7] = (uint8_t)(error_code & 0xFF);
    return http2_send_frame(conn, HTTP2_FRAME_GOAWAY, 0, 0, payload, 8);
}

int http2_send_window_update(http2_connection_t* conn, uint32_t stream_id, uint32_t increment) {
    uint8_t payload[4];
    payload[0] = (uint8_t)((increment >> 24) & 0x7F);
    payload[1] = (uint8_t)((increment >> 16) & 0xFF);
    payload[2] = (uint8_t)((increment >> 8) & 0xFF);
    payload[3] = (uint8_t)(increment & 0xFF);
    return http2_send_frame(conn, HTTP2_FRAME_WINDOW_UPDATE, 0, stream_id, payload, 4);
}

// ============================================================================
// STREAM MANAGEMENT
// ============================================================================

http2_stream_t* http2_get_stream(http2_connection_t* conn, uint32_t stream_id) {
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == stream_id)
            return &conn->streams[i];
    }
    return NULL;
}

http2_stream_t* http2_create_stream(http2_connection_t* conn) {
    if (conn->stream_count >= HTTP2_MAX_STREAMS) return NULL;
    http2_stream_t* stream = &conn->streams[conn->stream_count++];
    memset(stream, 0, sizeof(http2_stream_t));
    stream->id = conn->next_stream_id;
    conn->next_stream_id += 2;
    stream->state = HTTP2_STREAM_IDLE;
    stream->send_window = conn->peer_initial_window_size;
    stream->recv_window = conn->settings_initial_window_size;
    return stream;
}

void http2_close_stream(http2_connection_t* conn, uint32_t stream_id) {
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == stream_id) {
            conn->streams[i].state = HTTP2_STREAM_CLOSED;
            if (conn->streams[i].data) {
                kfree(conn->streams[i].data);
                conn->streams[i].data = NULL;
            }
            break;
        }
    }
}

// ============================================================================
// FRAME PROCESSING
// ============================================================================

int http2_process_frame(http2_connection_t* conn, const uint8_t* frame, size_t len) {
    if (len < 9) return -1;

    uint32_t payload_len = ((uint32_t)frame[0] << 16) | ((uint32_t)frame[1] << 8) | frame[2];
    uint8_t type = frame[3];
    uint8_t flags = frame[4];
    uint32_t stream_id = ((uint32_t)(frame[5] & 0x7F) << 24) | ((uint32_t)frame[6] << 16) |
                         ((uint32_t)frame[7] << 8) | frame[8];

    if (len < 9 + payload_len) return -1;
    const uint8_t* payload = frame + 9;

    switch (type) {
        case HTTP2_FRAME_SETTINGS:
            if (flags & HTTP2_FLAG_ACK) {
                // Settings ACK received
            } else {
                for (size_t i = 0; i + 6 <= payload_len; i += 6) {
                    uint16_t id = (uint16_t)((payload[i] << 8) | payload[i + 1]);
                    uint32_t value = ((uint32_t)payload[i + 2] << 24) | ((uint32_t)payload[i + 3] << 16) |
                                     ((uint32_t)payload[i + 4] << 8) | payload[i + 5];
                    switch (id) {
                        case HTTP2_SETTINGS_HEADER_TABLE_SIZE:
                            conn->peer_header_table_size = value;
                            conn->encoder_table.max_size = value;
                            break;
                        case HTTP2_SETTINGS_MAX_FRAME_SIZE:
                            conn->peer_max_frame_size = value;
                            break;
                        case HTTP2_SETTINGS_INITIAL_WINDOW_SIZE:
                            conn->peer_initial_window_size = value;
                            break;
                    }
                }
                http2_send_settings_ack(conn);
            }
            break;

        case HTTP2_FRAME_HEADERS: {
            http2_stream_t* stream = http2_get_stream(conn, stream_id);
            if (!stream) {
                if (stream_id % 2 == 0) {
                    stream = http2_create_stream(conn);
                    if (stream) stream->id = stream_id;
                }
            }
            if (stream) {
                size_t offset = 0;
                uint8_t pad_length = 0;
                if (flags & HTTP2_FLAG_PADDED) {
                    pad_length = payload[0];
                    offset = 1;
                }
                if (flags & HTTP2_FLAG_PRIORITY) {
                    offset += 5;
                }
                size_t header_block_len = payload_len - offset - pad_length;
                hpack_decode(&conn->decoder_table, payload + offset, header_block_len,
                             stream->headers, HTTP2_MAX_HEADERS, &stream->header_count);
                if (flags & HTTP2_FLAG_END_STREAM) {
                    stream->end_stream_received = 1;
                }
            }
            break;
        }

        case HTTP2_FRAME_DATA: {
            http2_stream_t* stream = http2_get_stream(conn, stream_id);
            if (stream) {
                size_t data_offset = 0;
                uint8_t pad_length = 0;
                if (flags & HTTP2_FLAG_PADDED) {
                    pad_length = payload[0];
                    data_offset = 1;
                }
                size_t data_len = payload_len - data_offset - pad_length;

                /* --- existing buffer growth + memcpy code stays the same --- */
                if (stream->data_len + data_len > stream->data_capacity) {
                    size_t new_cap = stream->data_capacity + data_len + 4096;
                    uint8_t* new_data = (uint8_t*)kmalloc(new_cap);
                    if (new_data) {
                        if (stream->data) {
                            memcpy(new_data, stream->data, stream->data_len);
                            kfree(stream->data);
                        }
                        stream->data = new_data;
                        stream->data_capacity = new_cap;
                    }
                }
                if (stream->data) {
                    memcpy(stream->data + stream->data_len, payload + data_offset, data_len);
                    stream->data_len += data_len;
                }

                /* =============================================
                * FIX: Replenish the flow-control window.
                *
                * Without this, the server's send window
                * (initially 65535) drains to zero after
                * ~65 KB of DATA and it stops sending.
                * We must tell the server "I consumed N
                * bytes, you may send N more" on BOTH the
                * stream and the connection.
                * ============================================= */
                if (data_len > 0) {
                    http2_send_window_update(conn, stream_id, (uint32_t)data_len);
                    http2_send_window_update(conn, 0,         (uint32_t)data_len);
                }

                if (flags & HTTP2_FLAG_END_STREAM) {
                    stream->end_stream_received = 1;
                }
            }
            break;
        }

        case HTTP2_FRAME_RST_STREAM:
            http2_close_stream(conn, stream_id);
            break;

        case HTTP2_FRAME_GOAWAY:
            conn->goaway_received = 1;
            break;

        case HTTP2_FRAME_PING:
            if (!(flags & HTTP2_FLAG_ACK)) {
                http2_send_ping_ack(conn, payload);
            }
            break;

        case HTTP2_FRAME_WINDOW_UPDATE:
            // TODO: update flow control window
            break;
    }
    return 0;
}

int http2_receive_frame(http2_connection_t* conn) {
    uint8_t header[9];
    int received = http2_read(conn, header, 9);
    if (received < 9) return -1;

    uint32_t payload_len = ((uint32_t)header[0] << 16) |
                           ((uint32_t)header[1] << 8) | header[2];
    if (payload_len > HTTP2_MAX_FRAME_SIZE) return -1;

    // Use a single allocation for header+payload instead of two
    uint8_t* frame = (uint8_t*)kmalloc(9 + payload_len);
    if (!frame) return -1;
    memcpy(frame, header, 9);

    if (payload_len > 0) {
        received = http2_read(conn, frame + 9, payload_len);
        if (received < (int)payload_len) {
            kfree(frame);
            return -1;
        }
    }

    int result = http2_process_frame(conn, frame, 9 + payload_len);
    kfree(frame);
    return result;
}

// ============================================================================
// HTTP/2 CONNECTION — init over existing TLS session (used by browser)
// ============================================================================
http2_connection_t* http2_init_over_tls(void* tls_session, int sockfd,
                                         const char* host, uint16_t port)
{
    if (!tls_session || sockfd < 0) return NULL;

    http2_connection_t* conn =
        (http2_connection_t*)kmalloc(sizeof(http2_connection_t));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(http2_connection_t));

    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port      = port;
    conn->use_tls   = 1;
    conn->tls_version = 12;
    conn->tls_session = (void*)tls_session;
    conn->socket_fd   = sockfd;

    // Default settings (RFC 7540 §6.5.2)
    conn->settings_header_table_size      = HTTP2_DEFAULT_HEADER_TABLE_SIZE;
    conn->settings_enable_push            = HTTP2_DEFAULT_ENABLE_PUSH;
    conn->settings_max_concurrent_streams = HTTP2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->settings_initial_window_size    = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->settings_max_frame_size         = HTTP2_DEFAULT_MAX_FRAME_SIZE;
    conn->settings_max_header_list_size   = HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE;

    conn->peer_header_table_size      = HTTP2_DEFAULT_HEADER_TABLE_SIZE;
    conn->peer_max_frame_size         = HTTP2_DEFAULT_MAX_FRAME_SIZE;
    conn->peer_initial_window_size    = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;

    conn->send_window = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->recv_window = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;

    conn->next_stream_id = 1;

    // HPACK tables
    hpack_init_table(&conn->encoder_table, HTTP2_DEFAULT_HEADER_TABLE_SIZE);
    hpack_init_table(&conn->decoder_table, HTTP2_DEFAULT_HEADER_TABLE_SIZE);

    // Send the HTTP/2 connection preface (RFC 7540 §3.5)
    if (http2_write(conn, HTTP2_PREFACE, HTTP2_PREFACE_LEN) < 0) {
        s_printf("[HTTP2] Failed to send connection preface\n");
        hpack_free_table(&conn->encoder_table);
        hpack_free_table(&conn->decoder_table);
        kfree(conn);
        return NULL;
    }

    // Send our SETTINGS
    if (http2_send_settings(conn) < 0) {
        s_printf("[HTTP2] Failed to send SETTINGS\n");
        hpack_free_table(&conn->encoder_table);
        hpack_free_table(&conn->decoder_table);
        kfree(conn);
        return NULL;
    }

    // Wait for the server's SETTINGS frame
    uint32_t start = get_tick_count();
    int got_settings = 0;

    while (!got_settings) {
        if (get_tick_count() - start > 10000) {
            s_printf("[HTTP2] Timeout waiting for server SETTINGS\n");
            hpack_free_table(&conn->encoder_table);
            hpack_free_table(&conn->decoder_table);
            kfree(conn);
            return NULL;
        }

        if (http2_receive_frame(conn) < 0) {
            for (int p = 0; p < 4; p++) rtl8139_poll();
            extern void tcp_retransmit_check(void);
            tcp_retransmit_check();
            continue;
        }

        got_settings = 1;
    }

    // Send SETTINGS ACK (in case process_frame didn't)
    http2_send_settings_ack(conn);

    conn->established = 1;
    s_printf("[HTTP2] Connection established over TLS 1.2 (ALPN h2)\n");
    return conn;
}

// ============================================================================
// HTTP/2 CONNECTION — standalone (creates own socket + TLS)
// ============================================================================

http2_connection_t* http2_connect(const char* host, uint16_t port, int use_tls) {
    http2_connection_t* conn = (http2_connection_t*)kmalloc(sizeof(http2_connection_t));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(http2_connection_t));

    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;
    conn->use_tls = use_tls;

    conn->settings_header_table_size = HTTP2_DEFAULT_HEADER_TABLE_SIZE;
    conn->settings_enable_push = HTTP2_DEFAULT_ENABLE_PUSH;
    conn->settings_max_concurrent_streams = HTTP2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->settings_initial_window_size = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->settings_max_frame_size = HTTP2_DEFAULT_MAX_FRAME_SIZE;
    conn->settings_max_header_list_size = HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE;
    conn->peer_header_table_size = HTTP2_DEFAULT_HEADER_TABLE_SIZE;
    conn->peer_max_frame_size = HTTP2_DEFAULT_MAX_FRAME_SIZE;
    conn->peer_initial_window_size = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->send_window = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->recv_window = HTTP2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->next_stream_id = 1;

    hpack_init_table(&conn->encoder_table, HTTP2_DEFAULT_HEADER_TABLE_SIZE);
    hpack_init_table(&conn->decoder_table, HTTP2_DEFAULT_HEADER_TABLE_SIZE);

    conn->socket_fd = k_socket(AF_INET, SOCK_STREAM, 0);
    if (conn->socket_fd < 0) {
        kfree(conn);
        return NULL;
    }

    char ip_str[32];
    if (dns_resolve(host, ip_str, sizeof(ip_str)) < 0) {
        k_close(conn->socket_fd);
        kfree(conn);
        return NULL;
    }

    uint32_t ip = 0;
    char* dot = ip_str;
    for (int i = 0; i < 4; i++) {
        ip = (ip << 8) | (uint32_t)local_atoi(dot);
        dot = strchr(dot, '.');
        if (dot) dot++;
    }

    sockaddr_in_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ip;

    if (k_connect(conn->socket_fd, &addr) < 0) {
        k_close(conn->socket_fd);
        kfree(conn);
        return NULL;
    }

    if (use_tls) {
        tls13_session_t* tls = tls13_create_session();
        if (tls) {
            if (tls13_connect(tls, host, port) == 0) {
                conn->tls_session = tls;
                conn->tls_version = 13;
            } else {
                tls13_destroy_session(tls);
                k_close(conn->socket_fd);
                kfree(conn);
                return NULL;
            }
        }
    }

    // Send HTTP/2 connection preface
    if (http2_write(conn, HTTP2_PREFACE, HTTP2_PREFACE_LEN) < 0) {
        http2_close(conn);
        return NULL;
    }

    if (http2_send_settings(conn) < 0) {
        http2_close(conn);
        return NULL;
    }

    if (http2_receive_frame(conn) < 0) {
        http2_close(conn);
        return NULL;
    }

    conn->established = 1;
    return conn;
}

void http2_close(http2_connection_t* conn) {
    if (!conn) return;
    if (conn->established && !conn->goaway_sent) {
        http2_send_goaway(conn, conn->last_stream_id, HTTP2_ERROR_NO_ERROR);
        conn->goaway_sent = 1;
    }
    if (conn->tls_session) {
        if (conn->tls_version == 13) {
            tls13_close((tls13_session_t*)conn->tls_session);
            tls13_destroy_session((tls13_session_t*)conn->tls_session);
        }
        // TLS 1.2 sessions are owned by the browser — don't close here
    }
    if (conn->socket_fd >= 0) {
        k_close(conn->socket_fd);
    }
    hpack_free_table(&conn->encoder_table);
    hpack_free_table(&conn->decoder_table);
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].data) kfree(conn->streams[i].data);
    }
    kfree(conn);
}

// ============================================================================
// HTTP/2 REQUEST
// ============================================================================

int http2_request(http2_connection_t* conn, const char* method, const char* path,
                  const http2_header_t* extra_headers, int extra_header_count,
                  const uint8_t* body, size_t body_len,
                  uint8_t* response, size_t* response_len,
                  http2_header_t* response_headers, int* response_header_count) {
    if (!conn || !conn->established) return -1;

    http2_stream_t* stream = http2_create_stream(conn);
    if (!stream) return -1;

    http2_header_t headers[16];
    int header_count = 0;

    // Pseudo-headers (must come first per RFC 7540 §8.1.2.3)
    strcpy(headers[header_count].name, ":method");
    strcpy(headers[header_count].value, method);
    headers[header_count].is_pseudo = 1;
    header_count++;

    strcpy(headers[header_count].name, ":path");
    strcpy(headers[header_count].value, path);
    headers[header_count].is_pseudo = 1;
    header_count++;

    strcpy(headers[header_count].name, ":scheme");
    strcpy(headers[header_count].value, conn->use_tls ? "https" : "http");
    headers[header_count].is_pseudo = 1;
    header_count++;

    strcpy(headers[header_count].name, ":authority");
    strcpy(headers[header_count].value, conn->host);
    headers[header_count].is_pseudo = 1;
    header_count++;

    for (int i = 0; i < extra_header_count && header_count < 14; i++) {
        headers[header_count++] = extra_headers[i];
    }

    strcpy(headers[header_count].name, "user-agent");
    strcpy(headers[header_count].value, "CamelOS/1.0 (HTTP/2)");
    header_count++;

    int end_stream = (body == NULL || body_len == 0);
    if (http2_send_headers(conn, stream->id, headers, header_count, end_stream) < 0)
        return -1;

    if (body && body_len > 0) {
        if (http2_send_data(conn, stream->id, body, body_len, 1) < 0)
            return -1;
    }

    // Receive response frames until END_STREAM
    // Receive response frames until END_STREAM
    uint32_t start = get_tick_count();
    uint32_t last_activity = start;

    while (!stream->end_stream_received) {
        if (conn->goaway_received) break;

        // Timeout: 5000 ticks of SILENCE, not total elapsed
        if (get_tick_count() - last_activity > 5000) return -1;   // ← CHANGED

        if (http2_receive_frame(conn) < 0) {
            for (int p = 0; p < 4; p++) rtl8139_poll();
            extern void tcp_retransmit_check(void);
            tcp_retransmit_check();
            if (conn->goaway_received) break;
            if (get_tick_count() - last_activity > 5000) return -1; // ← CHANGED
        } else {
            last_activity = get_tick_count();  // ← NEW: reset on success
        }
    }

    // If we stopped due to GOAWAY without END_STREAM, check if we
    // already have data — the server may have sent all DATA frames
    // before the GOAWAY.
    if (!stream->end_stream_received && conn->goaway_received) {
        if (stream->data_len > 0) {
            // We have data — treat as successful
            stream->end_stream_received = 1;
        } else {
            return -1;
        }
    }

    if (response && response_len && stream->data) {
        size_t copy_len = (stream->data_len < *response_len) ? stream->data_len : *response_len;
        memcpy(response, stream->data, copy_len);
        *response_len = copy_len;
    }

    if (response_headers && response_header_count) {
        int copy_count = (stream->header_count < *response_header_count) ?
                         stream->header_count : *response_header_count;
        for (int i = 0; i < copy_count; i++)
            response_headers[i] = stream->headers[i];
        *response_header_count = copy_count;
    }

    if (stream->id > conn->last_stream_id)
        conn->last_stream_id = stream->id;

    return 0;
}

int http2_get(http2_connection_t* conn, const char* path,
              http2_header_t* request_headers, int request_header_count,
              uint8_t* response, size_t* response_len,
              http2_header_t* response_headers, int* response_header_count) {
    return http2_request(conn, "GET", path, request_headers, request_header_count,
                         NULL, 0, response, response_len, response_headers, response_header_count);
}

int http2_post(http2_connection_t* conn, const char* path,
               http2_header_t* request_headers, int request_header_count,
               const uint8_t* body, size_t body_len,
               uint8_t* response, size_t* response_len,
               http2_header_t* response_headers, int* response_header_count) {
    return http2_request(conn, "POST", path, request_headers, request_header_count,
                         body, body_len, response, response_len, response_headers, response_header_count);
}