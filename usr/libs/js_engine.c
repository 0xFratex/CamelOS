// usr/libs/js_engine.c - Lightweight JavaScript Engine Implementation for Camel OS
// Integer-only version for CDL compatibility
#include "js_engine.h"
#include "../../sys/cdl_defs.h"

// Helper: string concatenate
static void js_strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = 0;
}

// Helper: string length
static int js_strlen(const char* s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

// Helper: string copy
static void js_strcpy(char* dest, const char* src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}

// Helper: memory set
static void js_memset(void* ptr, int val, int num) {
    unsigned char* p = (unsigned char*)ptr;
    for (int i = 0; i < num; i++) {
        p[i] = (unsigned char)val;
    }
}

// ============================================================================
// Token Types for Lexer
// ============================================================================
typedef enum {
    TOK_EOF,
    TOK_NUMBER,
    TOK_STRING,
    TOK_IDENTIFIER,
    TOK_KEYWORD,
    TOK_OPERATOR,
    TOK_PUNCTUATOR,
    TOK_ERROR
} token_type_t;

typedef struct {
    token_type_t type;
    char value[256];
    int number_value;
    int line;
    int column;
} token_t;

// Keywords
static const char* keywords[] = {
    "var", "let", "const", "function", "return", "if", "else", "for", "while",
    "do", "switch", "case", "break", "continue", "default", "true", "false", 
    "null", "undefined", "this", "new",
    NULL
};

// Operators
static const char* operators[] = {
    "+", "-", "*", "/", "%", "=", "+=", "-=", "*=", "/=",
    "==", "===", "!=", "!==", "<", ">", "<=", ">=", "&&", "||", "!",
    "++", "--", "?", ":",
    ".", ",", ";", "(", ")", "[", "]", "{", "}",
    NULL
};

// ============================================================================
// Lexer State
// ============================================================================
typedef struct {
    const char* source;
    int pos;
    int length;
    int line;
    int column;
    token_t current;
    token_t peek;
    int has_peek;
} lexer_t;

// ============================================================================
// Parser State
// ============================================================================
typedef struct {
    lexer_t lexer;
    js_engine_t* engine;
    int scope_level;
    char error[256];
} parser_t;

// ============================================================================
// Forward Declarations
// ============================================================================
static js_value_t* parse_expression(parser_t* parser);
static js_value_t* parse_statement(parser_t* parser);

// ============================================================================
// Value Management (Simplified - no dynamic allocation)
// ============================================================================

static js_value_t* alloc_value(js_engine_t* engine) {
    if (engine->value_count >= 256) {
        engine->has_error = 1;
        js_strcpy(engine->error_msg, "Out of value slots");
        return 0;
    }
    
    js_value_t* val = &engine->values[engine->value_count++];
    js_memset(val, 0, sizeof(js_value_t));
    val->type = JS_TYPE_UNDEFINED;
    val->ref_count = 1;
    return val;
}

js_value_t* js_new_undefined(js_engine_t* engine) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_UNDEFINED;
    }
    return val;
}

js_value_t* js_new_null(js_engine_t* engine) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_NULL;
    }
    return val;
}

js_value_t* js_new_boolean(js_engine_t* engine, int value) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_BOOLEAN;
        val->data.boolean = value ? 1 : 0;
    }
    return val;
}

js_value_t* js_new_number(js_engine_t* engine, int value) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_NUMBER;
        val->data.number = value;
    }
    return val;
}

js_value_t* js_new_string(js_engine_t* engine, const char* value) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_STRING;
        // Store string in value's string buffer (simplified)
        js_strcpy(val->data.string, value);
    }
    return val;
}

js_value_t* js_new_object(js_engine_t* engine) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_OBJECT;
        // Use static object storage
        static js_object_t static_objects[64];
        static int obj_idx = 0;
        if (obj_idx < 64) {
            val->data.object = &static_objects[obj_idx++];
            js_memset(val->data.object, 0, sizeof(js_object_t));
        }
    }
    return val;
}

js_value_t* js_new_array(js_engine_t* engine) {
    js_value_t* val = alloc_value(engine);
    if (val) {
        val->type = JS_TYPE_ARRAY;
        // Use static array storage
        static js_array_t static_arrays[64];
        static int arr_idx = 0;
        if (arr_idx < 64) {
            val->data.array = &static_arrays[arr_idx++];
            js_memset(val->data.array, 0, sizeof(js_array_t));
        }
    }
    return val;
}

void js_value_ref(js_value_t* value) {
    if (value) {
        value->ref_count++;
    }
}

void js_value_unref(js_value_t* value) {
    if (value && --value->ref_count <= 0) {
        value->type = JS_TYPE_UNDEFINED;
    }
}

// ============================================================================
// Lexer Implementation
// ============================================================================

static void init_lexer(lexer_t* lexer, const char* source) {
    lexer->source = source;
    lexer->pos = 0;
    lexer->length = js_strlen(source);
    lexer->line = 1;
    lexer->column = 1;
    lexer->has_peek = 0;
}

static char peek_char(lexer_t* lexer) {
    if (lexer->pos >= lexer->length) return '\0';
    return lexer->source[lexer->pos];
}

static char next_char(lexer_t* lexer) {
    if (lexer->pos >= lexer->length) return '\0';
    char c = lexer->source[lexer->pos++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static void skip_whitespace(lexer_t* lexer) {
    while (1) {
        char c = peek_char(lexer);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            next_char(lexer);
        } else if (c == '/' && lexer->pos + 1 < lexer->length) {
            char next = lexer->source[lexer->pos + 1];
            if (next == '/') {
                while (peek_char(lexer) != '\n' && peek_char(lexer) != '\0') {
                    next_char(lexer);
                }
            } else if (next == '*') {
                next_char(lexer);
                next_char(lexer);
                while (1) {
                    char c2 = next_char(lexer);
                    if (c2 == '\0') break;
                    if (c2 == '*' && peek_char(lexer) == '/') {
                        next_char(lexer);
                        break;
                    }
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

static int is_keyword(const char* str) {
    for (int i = 0; keywords[i]; i++) {
        const char* k = keywords[i];
        const char* s = str;
        while (*k && *s && *k == *s) { k++; s++; }
        if (*k == '\0' && *s == '\0') return 1;
    }
    return 0;
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static token_t next_token(lexer_t* lexer) {
    skip_whitespace(lexer);
    
    token_t tok;
    tok.line = lexer->line;
    tok.column = lexer->column;
    
    char c = peek_char(lexer);
    
    if (c == '\0') {
        tok.type = TOK_EOF;
        tok.value[0] = '\0';
        return tok;
    }
    
    // Number
    if (is_digit(c)) {
        tok.type = TOK_NUMBER;
        int i = 0;
        int num = 0;
        while (is_digit(peek_char(lexer))) {
            num = num * 10 + (peek_char(lexer) - '0');
            tok.value[i++] = next_char(lexer);
        }
        tok.value[i] = '\0';
        tok.number_value = num;
        return tok;
    }
    
    // String
    if (c == '"' || c == '\'') {
        char quote = next_char(lexer);
        tok.type = TOK_STRING;
        int i = 0;
        while (peek_char(lexer) != quote && peek_char(lexer) != '\0') {
            char ch = next_char(lexer);
            if (ch == '\\' && peek_char(lexer) != '\0') {
                ch = next_char(lexer);
                switch (ch) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                }
            }
            tok.value[i++] = ch;
        }
        tok.value[i] = '\0';
        if (peek_char(lexer) == quote) next_char(lexer);
        return tok;
    }
    
    // Identifier or keyword
    if (is_alpha(c)) {
        tok.type = TOK_IDENTIFIER;
        int i = 0;
        while (is_alnum(peek_char(lexer))) {
            tok.value[i++] = next_char(lexer);
        }
        tok.value[i] = '\0';
        
        if (is_keyword(tok.value)) {
            tok.type = TOK_KEYWORD;
        }
        return tok;
    }
    
    // Operators and punctuators
    for (int len = 3; len >= 1; len--) {
        if (lexer->pos + len <= lexer->length) {
            char op[4] = {0};
            for (int i = 0; i < len; i++) {
                op[i] = lexer->source[lexer->pos + i];
            }
            for (int i = 0; operators[i]; i++) {
                if (str_eq(op, operators[i])) {
                    tok.type = TOK_OPERATOR;
                    js_strcpy(tok.value, op);
                    for (int j = 0; j < len; j++) next_char(lexer);
                    return tok;
                }
            }
        }
    }
    
    tok.type = TOK_ERROR;
    tok.value[0] = next_char(lexer);
    tok.value[1] = '\0';
    return tok;
}

static token_t* current_token(lexer_t* lexer) {
    if (!lexer->has_peek) {
        lexer->current = next_token(lexer);
    }
    return &lexer->current;
}

static void advance_token(lexer_t* lexer) {
    if (lexer->has_peek) {
        lexer->current = lexer->peek;
        lexer->has_peek = 0;
    } else {
        lexer->current = next_token(lexer);
    }
}

// ============================================================================
// Variable and Function Lookup
// ============================================================================

static js_variable_t* find_variable(js_engine_t* engine, const char* name) {
    for (int i = 0; i < engine->global_count; i++) {
        if (str_eq(engine->globals[i].name, name)) {
            return &engine->globals[i];
        }
    }
    return 0;
}

static js_function_t* find_function(js_engine_t* engine, const char* name) {
    for (int i = 0; i < engine->function_count; i++) {
        if (str_eq(engine->functions[i].name, name)) {
            return &engine->functions[i];
        }
    }
    return 0;
}

// ============================================================================
// Parser Implementation
// ============================================================================

static js_value_t* eval_binary_op(parser_t* parser, js_value_t* left, const char* op, js_value_t* right) {
    js_engine_t* engine = parser->engine;
    
    int l = (left && left->type == JS_TYPE_NUMBER) ? left->data.number : 0;
    int r = (right && right->type == JS_TYPE_NUMBER) ? right->data.number : 0;
    
    if (str_eq(op, "+")) {
        // String concatenation or addition
        if (left && left->type == JS_TYPE_STRING) {
            char result[512];
            js_strcpy(result, left->data.string);
            if (right && right->type == JS_TYPE_STRING) {
                js_strcat(result, right->data.string);
            } else if (right && right->type == JS_TYPE_NUMBER) {
                // Convert number to string
                char numbuf[16];
                int n = right->data.number;
                int i = 15;
                numbuf[i--] = '\0';
                if (n == 0) {
                    numbuf[i--] = '0';
                } else {
                    int neg = n < 0;
                    if (neg) n = -n;
                    while (n > 0) {
                        numbuf[i--] = '0' + (n % 10);
                        n /= 10;
                    }
                    if (neg) numbuf[i--] = '-';
                }
                js_strcat(result, &numbuf[i + 1]);
            }
            return js_new_string(engine, result);
        }
        return js_new_number(engine, l + r);
    }
    if (str_eq(op, "-")) return js_new_number(engine, l - r);
    if (str_eq(op, "*")) return js_new_number(engine, l * r);
    if (str_eq(op, "/")) return js_new_number(engine, r != 0 ? l / r : 0);
    if (str_eq(op, "%")) return js_new_number(engine, r != 0 ? l % r : 0);
    
    if (str_eq(op, "==") || str_eq(op, "===")) {
        if (!left || !right) return js_new_boolean(engine, left == right);
        if (left->type != right->type) return js_new_boolean(engine, 0);
        switch (left->type) {
            case JS_TYPE_NUMBER: return js_new_boolean(engine, left->data.number == right->data.number);
            case JS_TYPE_STRING: return js_new_boolean(engine, str_eq(left->data.string, right->data.string));
            case JS_TYPE_BOOLEAN: return js_new_boolean(engine, left->data.boolean == right->data.boolean);
            default: return js_new_boolean(engine, 0);
        }
    }
    
    if (str_eq(op, "!=") || str_eq(op, "!==")) {
        if (!left || !right) return js_new_boolean(engine, left != right);
        if (left->type != right->type) return js_new_boolean(engine, 1);
        switch (left->type) {
            case JS_TYPE_NUMBER: return js_new_boolean(engine, left->data.number != right->data.number);
            case JS_TYPE_STRING: return js_new_boolean(engine, !str_eq(left->data.string, right->data.string));
            case JS_TYPE_BOOLEAN: return js_new_boolean(engine, left->data.boolean != right->data.boolean);
            default: return js_new_boolean(engine, 1);
        }
    }
    
    if (str_eq(op, "<")) return js_new_boolean(engine, l < r);
    if (str_eq(op, ">")) return js_new_boolean(engine, l > r);
    if (str_eq(op, "<=")) return js_new_boolean(engine, l <= r);
    if (str_eq(op, ">=")) return js_new_boolean(engine, l >= r);
    
    if (str_eq(op, "&&")) {
        int lb = (left && left->type == JS_TYPE_BOOLEAN) ? left->data.boolean : (left && left->type == JS_TYPE_NUMBER && left->data.number);
        if (!lb) return js_new_boolean(engine, 0);
        int rb = (right && right->type == JS_TYPE_BOOLEAN) ? right->data.boolean : (right && right->type == JS_TYPE_NUMBER && right->data.number);
        return js_new_boolean(engine, rb);
    }
    
    if (str_eq(op, "||")) {
        int lb = (left && left->type == JS_TYPE_BOOLEAN) ? left->data.boolean : (left && left->type == JS_TYPE_NUMBER && left->data.number);
        if (lb) return js_new_boolean(engine, 1);
        int rb = (right && right->type == JS_TYPE_BOOLEAN) ? right->data.boolean : (right && right->type == JS_TYPE_NUMBER && right->data.number);
        return js_new_boolean(engine, rb);
    }
    
    return js_new_undefined(engine);
}

static js_value_t* parse_primary(parser_t* parser);
static js_value_t* parse_function_call(parser_t* parser, const char* name);

static js_value_t* parse_primary(parser_t* parser) {
    lexer_t* lexer = &parser->lexer;
    token_t* tok = current_token(lexer);
    
    if (tok->type == TOK_NUMBER) {
        advance_token(lexer);
        return js_new_number(parser->engine, tok->number_value);
    }
    
    if (tok->type == TOK_STRING) {
        advance_token(lexer);
        return js_new_string(parser->engine, tok->value);
    }
    
    if (tok->type == TOK_KEYWORD) {
        if (str_eq(tok->value, "true")) {
            advance_token(lexer);
            return js_new_boolean(parser->engine, 1);
        }
        if (str_eq(tok->value, "false")) {
            advance_token(lexer);
            return js_new_boolean(parser->engine, 0);
        }
        if (str_eq(tok->value, "null")) {
            advance_token(lexer);
            return js_new_null(parser->engine);
        }
        if (str_eq(tok->value, "undefined")) {
            advance_token(lexer);
            return js_new_undefined(parser->engine);
        }
        if (str_eq(tok->value, "var") || str_eq(tok->value, "let") || str_eq(tok->value, "const")) {
            advance_token(lexer);
            tok = current_token(lexer);
            if (tok->type == TOK_IDENTIFIER) {
                char name[64];
                js_strcpy(name, tok->value);
                advance_token(lexer);
                
                js_value_t* value = js_new_undefined(parser->engine);
                tok = current_token(lexer);
                if (tok->type == TOK_OPERATOR && str_eq(tok->value, "=")) {
                    advance_token(lexer);
                    value = parse_expression(parser);
                }
                
                if (parser->engine->global_count < JS_MAX_VARIABLES) {
                    js_variable_t* var = &parser->engine->globals[parser->engine->global_count++];
                    js_strcpy(var->name, name);
                    var->value = value;
                    var->scope_level = 0;
                }
                
                return value;
            }
        }
        if (str_eq(tok->value, "if")) {
            advance_token(lexer);
            tok = current_token(lexer);
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, "(")) {
                advance_token(lexer);
                js_value_t* condition = parse_expression(parser);
                tok = current_token(lexer);
                if (tok->type == TOK_OPERATOR && str_eq(tok->value, ")")) {
                    advance_token(lexer);
                }
                
                js_value_t* result = js_new_undefined(parser->engine);
                int cond_true = condition && 
                    ((condition->type == JS_TYPE_BOOLEAN && condition->data.boolean) ||
                     (condition->type == JS_TYPE_NUMBER && condition->data.number));
                
                if (cond_true) {
                    result = parse_statement(parser);
                } else {
                    parse_statement(parser);
                }
                
                tok = current_token(lexer);
                if (tok->type == TOK_KEYWORD && str_eq(tok->value, "else")) {
                    advance_token(lexer);
                    if (!cond_true) {
                        result = parse_statement(parser);
                    } else {
                        parse_statement(parser);
                    }
                }
                
                return result;
            }
        }
        if (str_eq(tok->value, "while")) {
            advance_token(lexer);
            tok = current_token(lexer);
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, "(")) {
                advance_token(lexer);
                int condition_start = lexer->pos;
                
                js_value_t* result = js_new_undefined(parser->engine);
                int max_iterations = 1000;
                int iterations = 0;
                
                while (iterations < max_iterations) {
                    lexer->pos = condition_start;
                    js_value_t* condition = parse_expression(parser);
                    tok = current_token(lexer);
                    if (tok->type == TOK_OPERATOR && str_eq(tok->value, ")")) {
                        advance_token(lexer);
                    }
                    
                    int cond_true = condition && 
                        ((condition->type == JS_TYPE_BOOLEAN && condition->data.boolean) ||
                         (condition->type == JS_TYPE_NUMBER && condition->data.number));
                    if (!cond_true) break;
                    
                    result = parse_statement(parser);
                    iterations++;
                }
                
                return result;
            }
        }
        if (str_eq(tok->value, "return")) {
            advance_token(lexer);
            return parse_expression(parser);
        }
    }
    
    if (tok->type == TOK_IDENTIFIER) {
        char name[128];
        js_strcpy(name, tok->value);
        advance_token(lexer);
        
        tok = current_token(lexer);
        if (tok->type == TOK_OPERATOR && str_eq(tok->value, "(")) {
            return parse_function_call(parser, name);
        }
        
        // Property access
        while (tok->type == TOK_OPERATOR && str_eq(tok->value, ".")) {
            advance_token(lexer);
            tok = current_token(lexer);
            if (tok->type == TOK_IDENTIFIER) {
                js_strcat(name, ".");
                js_strcat(name, tok->value);
                advance_token(lexer);
                tok = current_token(lexer);
            }
        }
        
        js_variable_t* var = find_variable(parser->engine, name);
        if (var) return var->value;
        
        js_function_t* func = find_function(parser->engine, name);
        if (func) {
            js_value_t* val = js_new_undefined(parser->engine);
            val->type = JS_TYPE_FUNCTION;
            val->data.function_id = func - parser->engine->functions;
            return val;
        }
        
        return js_new_undefined(parser->engine);
    }
    
    if (tok->type == TOK_OPERATOR) {
        if (str_eq(tok->value, "(")) {
            advance_token(lexer);
            js_value_t* result = parse_expression(parser);
            tok = current_token(lexer);
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, ")")) {
                advance_token(lexer);
            }
            return result;
        }
        if (str_eq(tok->value, "[")) {
            advance_token(lexer);
            js_value_t* arr = js_new_array(parser->engine);
            tok = current_token(lexer);
            while (tok->type != TOK_OPERATOR || !str_eq(tok->value, "]")) {
                js_value_t* elem = parse_expression(parser);
                js_array_push(parser->engine, arr, elem);
                tok = current_token(lexer);
                if (tok->type == TOK_OPERATOR && str_eq(tok->value, ",")) {
                    advance_token(lexer);
                    tok = current_token(lexer);
                }
            }
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, "]")) {
                advance_token(lexer);
            }
            return arr;
        }
        if (str_eq(tok->value, "{")) {
            advance_token(lexer);
            js_value_t* obj = js_new_object(parser->engine);
            tok = current_token(lexer);
            while (tok->type != TOK_OPERATOR || !str_eq(tok->value, "}")) {
                char key[64] = {0};
                if (tok->type == TOK_IDENTIFIER || tok->type == TOK_STRING) {
                    js_strcpy(key, tok->value);
                    advance_token(lexer);
                }
                tok = current_token(lexer);
                if (tok->type == TOK_OPERATOR && str_eq(tok->value, ":")) {
                    advance_token(lexer);
                    js_value_t* val = parse_expression(parser);
                    js_object_set(parser->engine, obj, key, val);
                }
                tok = current_token(lexer);
                if (tok->type == TOK_OPERATOR && str_eq(tok->value, ",")) {
                    advance_token(lexer);
                    tok = current_token(lexer);
                }
            }
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, "}")) {
                advance_token(lexer);
            }
            return obj;
        }
        if (str_eq(tok->value, "!")) {
            advance_token(lexer);
            js_value_t* operand = parse_primary(parser);
            int b = operand && operand->type == JS_TYPE_BOOLEAN ? operand->data.boolean : 
                   (operand && operand->type == JS_TYPE_NUMBER ? operand->data.number : 0);
            return js_new_boolean(parser->engine, !b);
        }
        if (str_eq(tok->value, "-")) {
            advance_token(lexer);
            js_value_t* operand = parse_primary(parser);
            int n = operand && operand->type == JS_TYPE_NUMBER ? operand->data.number : 0;
            return js_new_number(parser->engine, -n);
        }
    }
    
    advance_token(lexer);
    return js_new_undefined(parser->engine);
}

static js_value_t* parse_function_call(parser_t* parser, const char* name) {
    lexer_t* lexer = &parser->lexer;
    token_t* tok = current_token(lexer);
    
    if (!str_eq(tok->value, "(")) {
        return js_new_undefined(parser->engine);
    }
    advance_token(lexer);
    
    js_value_t* args[16];
    int argc = 0;
    
    tok = current_token(lexer);
    while (tok->type != TOK_OPERATOR || !str_eq(tok->value, ")")) {
        args[argc++] = parse_expression(parser);
        tok = current_token(lexer);
        if (tok->type == TOK_OPERATOR && str_eq(tok->value, ",")) {
            advance_token(lexer);
            tok = current_token(lexer);
        }
        if (argc >= 16) break;
    }
    
    if (tok->type == TOK_OPERATOR && str_eq(tok->value, ")")) {
        advance_token(lexer);
    }
    
    // Built-in functions
    if (str_eq(name, "console.log") || str_eq(name, "console.info")) {
        return js_console_log(argc, args);
    }
    if (str_eq(name, "console.error")) {
        return js_console_error(argc, args);
    }
    if (str_eq(name, "console.warn")) {
        return js_console_warn(argc, args);
    }
    if (str_eq(name, "document.getElementById")) {
        return js_document_getElementById(argc, args);
    }
    if (str_eq(name, "document.querySelector")) {
        return js_document_querySelector(argc, args);
    }
    if (str_eq(name, "alert") || str_eq(name, "window.alert")) {
        return js_window_alert(argc, args);
    }
    if (str_eq(name, "parseInt")) {
        if (argc > 0 && args[0]->type == JS_TYPE_STRING) {
            int val = 0;
            const char* p = args[0]->data.string;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            return js_new_number(parser->engine, val);
        }
        return js_new_number(parser->engine, 0);
    }
    if (str_eq(name, "String")) {
        if (argc > 0) return js_to_string(parser->engine, args[0]);
        return js_new_string(parser->engine, "");
    }
    if (str_eq(name, "Number")) {
        if (argc > 0) return js_to_number(parser->engine, args[0]);
        return js_new_number(parser->engine, 0);
    }
    if (str_eq(name, "Boolean")) {
        if (argc > 0) return js_to_boolean(parser->engine, args[0]);
        return js_new_boolean(parser->engine, 0);
    }
    
    // User-defined function
    js_function_t* func = find_function(parser->engine, name);
    if (func && func->is_native && func->native_fn) {
        return func->native_fn(argc, args);
    }
    
    return js_new_undefined(parser->engine);
}

static js_value_t* parse_expression(parser_t* parser) {
    lexer_t* lexer = &parser->lexer;
    
    js_value_t* left = parse_primary(parser);
    
    token_t* tok = current_token(lexer);
    while (tok->type == TOK_OPERATOR) {
        const char* op = tok->value;
        
        if (str_eq(op, "=")) {
            advance_token(lexer);
            js_value_t* right = parse_expression(parser);
            return right;
        }
        
        if (str_eq(op, "+=") || str_eq(op, "-=") || str_eq(op, "*=") || str_eq(op, "/=")) {
            advance_token(lexer);
            js_value_t* right = parse_expression(parser);
            return right;
        }
        
        if (str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") || str_eq(op, "/") ||
            str_eq(op, "%") || str_eq(op, "==") || str_eq(op, "===") ||
            str_eq(op, "!=") || str_eq(op, "!==") ||
            str_eq(op, "<") || str_eq(op, ">") || str_eq(op, "<=") || str_eq(op, ">=") ||
            str_eq(op, "&&") || str_eq(op, "||")) {
            advance_token(lexer);
            js_value_t* right = parse_primary(parser);
            left = eval_binary_op(parser, left, op, right);
            tok = current_token(lexer);
            continue;
        }
        
        if (str_eq(op, "?")) {
            advance_token(lexer);
            js_value_t* true_val = parse_expression(parser);
            tok = current_token(lexer);
            if (tok->type == TOK_OPERATOR && str_eq(tok->value, ":")) {
                advance_token(lexer);
                js_value_t* false_val = parse_expression(parser);
                int cond = left && left->type == JS_TYPE_BOOLEAN ? left->data.boolean :
                          (left && left->type == JS_TYPE_NUMBER ? left->data.number : 0);
                return cond ? true_val : false_val;
            }
        }
        
        break;
    }
    
    return left;
}

static js_value_t* parse_statement(parser_t* parser) {
    lexer_t* lexer = &parser->lexer;
    token_t* tok = current_token(lexer);
    
    // Block
    if (tok->type == TOK_OPERATOR && str_eq(tok->value, "{")) {
        advance_token(lexer);
        js_value_t* result = js_new_undefined(parser->engine);
        tok = current_token(lexer);
        while (tok->type != TOK_OPERATOR || !str_eq(tok->value, "}")) {
            if (tok->type == TOK_EOF) break;
            result = parse_statement(parser);
            tok = current_token(lexer);
        }
        if (tok->type == TOK_OPERATOR && str_eq(tok->value, "}")) {
            advance_token(lexer);
        }
        return result;
    }
    
    js_value_t* result = parse_expression(parser);
    
    tok = current_token(lexer);
    if (tok->type == TOK_OPERATOR && str_eq(tok->value, ";")) {
        advance_token(lexer);
    }
    
    return result;
}

// ============================================================================
// Type Conversion
// ============================================================================

js_value_t* js_to_string(js_engine_t* engine, js_value_t* value) {
    if (!value) return js_new_string(engine, "undefined");
    
    switch (value->type) {
        case JS_TYPE_UNDEFINED: return js_new_string(engine, "undefined");
        case JS_TYPE_NULL: return js_new_string(engine, "null");
        case JS_TYPE_BOOLEAN: return js_new_string(engine, value->data.boolean ? "true" : "false");
        case JS_TYPE_NUMBER: {
            char buf[32];
            int n = value->data.number;
            int i = 30;
            buf[31] = '\0';
            if (n == 0) {
                buf[i--] = '0';
            } else {
                int neg = n < 0;
                if (neg) n = -n;
                while (n > 0) {
                    buf[i--] = '0' + (n % 10);
                    n /= 10;
                }
                if (neg) buf[i--] = '-';
            }
            return js_new_string(engine, &buf[i + 1]);
        }
        case JS_TYPE_STRING:
            js_value_ref(value);
            return value;
        case JS_TYPE_OBJECT:
        case JS_TYPE_ARRAY:
            return js_new_string(engine, "[object Object]");
        default:
            return js_new_string(engine, "undefined");
    }
}

js_value_t* js_to_number(js_engine_t* engine, js_value_t* value) {
    if (!value) return js_new_number(engine, 0);
    
    switch (value->type) {
        case JS_TYPE_UNDEFINED:
        case JS_TYPE_NULL:
            return js_new_number(engine, 0);
        case JS_TYPE_BOOLEAN:
            return js_new_number(engine, value->data.boolean ? 1 : 0);
        case JS_TYPE_NUMBER:
            js_value_ref(value);
            return value;
        case JS_TYPE_STRING: {
            int num = 0;
            const char* p = value->data.string;
            int neg = 0;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            return js_new_number(engine, neg ? -num : num);
        }
        default:
            return js_new_number(engine, 0);
    }
}

js_value_t* js_to_boolean(js_engine_t* engine, js_value_t* value) {
    if (!value) return js_new_boolean(engine, 0);
    
    switch (value->type) {
        case JS_TYPE_UNDEFINED:
        case JS_TYPE_NULL:
            return js_new_boolean(engine, 0);
        case JS_TYPE_BOOLEAN:
            js_value_ref(value);
            return value;
        case JS_TYPE_NUMBER:
            return js_new_boolean(engine, value->data.number != 0);
        case JS_TYPE_STRING:
            return js_new_boolean(engine, value->data.string[0] != '\0');
        case JS_TYPE_OBJECT:
        case JS_TYPE_ARRAY:
            return js_new_boolean(engine, 1);
        default:
            return js_new_boolean(engine, 0);
    }
}

// ============================================================================
// Object Operations
// ============================================================================

void js_object_set(js_engine_t* engine, js_value_t* obj, const char* key, js_value_t* value) {
    if (!obj || obj->type != JS_TYPE_OBJECT || !obj->data.object) return;
    
    js_object_t* object = obj->data.object;
    
    for (int i = 0; i < object->property_count; i++) {
        if (str_eq(object->properties[i].key, key)) {
            object->properties[i].value = value;
            return;
        }
    }
    
    if (object->property_count < 16) {
        js_strcpy(object->properties[object->property_count].key, key);
        object->properties[object->property_count].value = value;
        object->property_count++;
    }
}

js_value_t* js_object_get(js_engine_t* engine, js_value_t* obj, const char* key) {
    if (!obj || obj->type != JS_TYPE_OBJECT || !obj->data.object) {
        return js_new_undefined(engine);
    }
    
    js_object_t* object = obj->data.object;
    
    for (int i = 0; i < object->property_count; i++) {
        if (str_eq(object->properties[i].key, key)) {
            return object->properties[i].value;
        }
    }
    
    return js_new_undefined(engine);
}

// ============================================================================
// Array Operations
// ============================================================================

void js_array_push(js_engine_t* engine, js_value_t* arr, js_value_t* value) {
    if (!arr || arr->type != JS_TYPE_ARRAY || !arr->data.array) return;
    
    js_array_t* array = arr->data.array;
    
    if (array->length < JS_MAX_ARRAY_SIZE) {
        array->elements[array->length++] = value;
    }
}

js_value_t* js_array_get(js_engine_t* engine, js_value_t* arr, int index) {
    if (!arr || arr->type != JS_TYPE_ARRAY || !arr->data.array) {
        return js_new_undefined(engine);
    }
    
    js_array_t* array = arr->data.array;
    
    if (index >= 0 && index < array->length) {
        return array->elements[index];
    }
    
    return js_new_undefined(engine);
}

int js_array_length(js_engine_t* engine, js_value_t* arr) {
    if (!arr || arr->type != JS_TYPE_ARRAY || !arr->data.array) return 0;
    return arr->data.array->length;
}

// ============================================================================
// Engine API
// ============================================================================

void js_init(js_engine_t* engine) {
    js_memset(engine, 0, sizeof(js_engine_t));
}

js_value_t* js_eval(js_engine_t* engine, const char* code) {
    parser_t parser;
    parser.engine = engine;
    parser.scope_level = 0;
    parser.error[0] = '\0';
    
    init_lexer(&parser.lexer, code);
    
    js_value_t* result = js_new_undefined(engine);
    
    while (current_token(&parser.lexer)->type != TOK_EOF) {
        result = parse_statement(&parser);
        if (engine->has_error) break;
    }
    
    return result;
}

int js_register_native(js_engine_t* engine, const char* name, 
                       js_value_t* (*fn)(int argc, js_value_t** args)) {
    if (engine->function_count >= JS_MAX_FUNCTIONS) return -1;
    
    js_function_t* func = &engine->functions[engine->function_count++];
    js_strcpy(func->name, name);
    func->body = 0;
    func->param_count = 0;
    func->is_native = 1;
    func->native_fn = fn;
    
    return 0;
}

js_value_t* js_get_global(js_engine_t* engine, const char* name) {
    js_variable_t* var = find_variable(engine, name);
    if (var) return var->value;
    return 0;
}

void js_set_global(js_engine_t* engine, const char* name, js_value_t* value) {
    for (int i = 0; i < engine->global_count; i++) {
        if (str_eq(engine->globals[i].name, name)) {
            engine->globals[i].value = value;
            return;
        }
    }
    
    if (engine->global_count < JS_MAX_VARIABLES) {
        js_strcpy(engine->globals[engine->global_count].name, name);
        engine->globals[engine->global_count].value = value;
        engine->globals[engine->global_count].scope_level = 0;
        engine->global_count++;
    }
}

const char* js_get_error(js_engine_t* engine) {
    return engine->error_msg;
}

void js_clear_error(js_engine_t* engine) {
    engine->has_error = 0;
    engine->error_msg[0] = '\0';
}

// ============================================================================
// Browser DOM Bindings - Stubs
// ============================================================================

js_value_t* js_console_log(int argc, js_value_t** args) {
    if (argc > 0) {
        // Output handled by browser
    }
    return js_new_undefined(0);
}

js_value_t* js_console_error(int argc, js_value_t** args) {
    return js_console_log(argc, args);
}

js_value_t* js_console_warn(int argc, js_value_t** args) {
    return js_console_log(argc, args);
}

js_value_t* js_document_getElementById(int argc, js_value_t** args) {
    return js_new_null(0);
}

js_value_t* js_document_querySelector(int argc, js_value_t** args) {
    return js_new_null(0);
}

js_value_t* js_document_querySelectorAll(int argc, js_value_t** args) {
    return js_new_array(0);
}

js_value_t* js_window_alert(int argc, js_value_t** args) {
    return js_new_undefined(0);
}

js_value_t* js_window_setTimeout(int argc, js_value_t** args) {
    return js_new_number(0, 0);
}

js_value_t* js_window_setInterval(int argc, js_value_t** args) {
    return js_new_number(0, 0);
}

void js_register_dom_api(js_engine_t* engine) {
    js_value_t* console = js_new_object(engine);
    js_set_global(engine, "console", console);
    
    js_value_t* document = js_new_object(engine);
    js_set_global(engine, "document", document);
    
    js_value_t* window = js_new_object(engine);
    js_set_global(engine, "window", window);
}
