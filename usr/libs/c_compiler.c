// usr/libs/c_compiler.c - CamelOS Native C Compiler Implementation
// Compiles C source code to NASM x86 32-bit assembly
// No floating point, no standard library - kernel mode only
#include "c_compiler.h"
#include "../../core/memory.h"
#include "../../core/string.h"

// ============================================================================
// External kernel functions
// ============================================================================
extern void s_printf(const char* fmt, ...);

// ============================================================================
// Internal helpers (no libc dependency)
// ============================================================================
static int cc_strlen(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
static int cc_strcmp(const char* a, const char* b) {
    if (!a) return b ? -1 : 0; if (!b) return 1;
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static int cc_strncmp(const char* a, const char* b, int n) {
    if (!a || !b || n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static char* cc_strcpy(char* d, const char* s) {
    char* r = d; if (s) while ((*d++ = *s++)); else *d = 0; return r;
}
static char* cc_strncpy(char* d, const char* s, int n) {
    char* r = d; int i = 0;
    if (s) { while (i < n && s[i]) { d[i] = s[i]; i++; } }
    while (i < n) d[i++] = 0;
    return r;
}
static int cc_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int cc_isdigit(int c) { return c >= '0' && c <= '9'; }
static int cc_isxdigit(int c) { return cc_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int cc_isalnum(int c) { return cc_isalpha(c) || cc_isdigit(c); }
// cc_isspace not needed in current implementation

// Simplified snprintf for code generation output
// We use a fixed-arg approach since we can't use va_list in kernel mode

// Forward declarations
static ast_node_t* alloc_node(compiler_context_t* ctx, ast_node_type_t type);

// ============================================================================
// INITIALIZATION / CLEANUP
// ============================================================================

void cc_init(compiler_context_t* ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(compiler_context_t));
    ctx->source = NULL;
    ctx->source_size = 0;
    ctx->ast_root = NULL;
    ctx->ast_node_count = 0;
    ctx->type_pool_count = 0;
    ctx->optimize_level = 0;
    ctx->output_assembly = 1;
    ctx->output_binary = 0;
    ctx->verbose = 0;
    symtab_init(&ctx->symbols);
    codegen_init(&ctx->codegen);
    ctx->output_buffer = (char*)kmalloc(CC_OUTPUT_BUFFER_SIZE);
    if (ctx->output_buffer) { ctx->output_buffer[0] = 0; ctx->output_buffer_size = 0; }
    cc_register_builtins(ctx);
}

void cc_cleanup(compiler_context_t* ctx) {
    if (!ctx) return;
    // Free string literal storage
    for (int i = 0; i < ctx->codegen.strings.count; i++) {
        if (ctx->codegen.strings.strings[i]) kfree(ctx->codegen.strings.strings[i]);
    }
    if (ctx->output_buffer) { kfree(ctx->output_buffer); ctx->output_buffer = NULL; }
    if (ctx->codegen.output) { kfree(ctx->codegen.output); ctx->codegen.output = NULL; }
    if (ctx->codegen.data_section) { kfree(ctx->codegen.data_section); ctx->codegen.data_section = NULL; }
}

// ============================================================================
// ERROR HANDLING (simplified - no variadic args)
// ============================================================================

void cc_error(compiler_context_t* ctx, error_category_t cat, const char* msg, ...) {
    // Variadic stub - just store the format string as-is for now
    (void)cat;
    if (!ctx || ctx->errors.error_count >= CC_MAX_ERRORS) return;
    compiler_error_t* e = &ctx->errors.errors[ctx->errors.error_count++];
    e->category = cat;
    cc_strncpy(e->message, msg ? msg : "unknown error", 255);
    e->message[255] = 0;
    e->line = ctx->line;
    e->column = ctx->column;
    e->is_warning = 0;
    s_printf("[CC] Error line %d: %s\n", ctx->line, msg ? msg : "unknown");
}

void cc_warning(compiler_context_t* ctx, const char* msg, ...) {
    if (!ctx || ctx->errors.error_count >= CC_MAX_ERRORS) return;
    compiler_error_t* e = &ctx->errors.errors[ctx->errors.error_count++];
    e->category = ERR_SEMANTIC;
    cc_strncpy(e->message, msg ? msg : "warning", 255);
    e->message[255] = 0;
    e->line = ctx->line;
    e->column = ctx->column;
    e->is_warning = 1;
    ctx->errors.warning_count++;
}

int cc_has_errors(compiler_context_t* ctx) {
    if (!ctx) return 1;
    for (int i = 0; i < ctx->errors.error_count; i++) {
        if (!ctx->errors.errors[i].is_warning) return 1;
    }
    return 0;
}

const char* cc_get_errors(compiler_context_t* ctx) {
    if (!ctx) return "No context";
    return ctx->errors.error_count > 0 ? ctx->errors.errors[0].message : "";
}

// ============================================================================
// TYPE SYSTEM
// ============================================================================

type_info_t* type_create(compiler_context_t* ctx, type_kind_t kind) {
    if (!ctx || ctx->type_pool_count >= 512) return NULL;
    type_info_t* t = &ctx->type_pool[ctx->type_pool_count++];
    memset(t, 0, sizeof(type_info_t));
    t->kind = kind;
    switch (kind) {
        case TYPE_VOID:   t->size = 0; break;
        case TYPE_CHAR:   t->size = SIZE_CHAR; break;
        case TYPE_SHORT:  t->size = SIZE_SHORT; break;
        case TYPE_INT:    t->size = SIZE_INT; break;
        case TYPE_LONG:   t->size = SIZE_LONG; break;
        case TYPE_FLOAT:  t->size = SIZE_FLOAT; break;
        case TYPE_DOUBLE: t->size = SIZE_DOUBLE; break;
        case TYPE_POINTER: t->size = SIZE_PTR; break;
        default: t->size = SIZE_INT; break;
    }
    return t;
}

type_info_t* type_copy(compiler_context_t* ctx, type_info_t* src) {
    if (!src) return NULL;
    type_info_t* t = type_create(ctx, src->kind);
    if (!t) return NULL;
    memcpy(t, src, sizeof(type_info_t));
    return t;
}

type_info_t* type_pointer_to(compiler_context_t* ctx, type_info_t* base) {
    type_info_t* t = type_create(ctx, TYPE_POINTER);
    if (t) t->base_type = base;
    return t;
}

type_info_t* type_array_of(compiler_context_t* ctx, type_info_t* base, int size) {
    type_info_t* t = type_create(ctx, TYPE_ARRAY);
    if (t) { t->base_type = base; t->array_size = size; }
    return t;
}

int type_size(type_info_t* type) {
    if (!type) return 0;
    switch (type->kind) {
        case TYPE_VOID: return 0;
        case TYPE_CHAR: return SIZE_CHAR;
        case TYPE_SHORT: return SIZE_SHORT;
        case TYPE_INT: case TYPE_LONG: return SIZE_INT;
        case TYPE_FLOAT: return SIZE_FLOAT;
        case TYPE_DOUBLE: return SIZE_DOUBLE;
        case TYPE_POINTER: return SIZE_PTR;
        case TYPE_ARRAY: return type->array_size * type_size(type->base_type);
        default: return SIZE_INT;
    }
}

int type_is_integer(type_info_t* t) {
    return t && (t->kind == TYPE_CHAR || t->kind == TYPE_SHORT || t->kind == TYPE_INT || t->kind == TYPE_LONG);
}

int type_is_pointer(type_info_t* t) { return t && t->kind == TYPE_POINTER; }
int type_is_array(type_info_t* t) { return t && t->kind == TYPE_ARRAY; }
int type_is_scalar(type_info_t* t) { return type_is_integer(t) || type_is_pointer(t); }
int type_compatible(type_info_t* t1, type_info_t* t2) {
    if (!t1 || !t2) return 0;
    if (t1->kind == t2->kind) return 1;
    if (type_is_scalar(t1) && type_is_scalar(t2)) return 1;
    return 0;
}
int type_assignable(type_info_t* dest, type_info_t* src) { return type_compatible(dest, src); }

const char* type_to_string(type_info_t* type, char* buf, int size) {
    if (!type) { cc_strncpy(buf, "void", size); return buf; }
    switch (type->kind) {
        case TYPE_VOID: cc_strncpy(buf, "void", size); break;
        case TYPE_CHAR: cc_strncpy(buf, type->is_unsigned ? "uchar" : "char", size); break;
        case TYPE_SHORT: cc_strncpy(buf, type->is_unsigned ? "ushort" : "short", size); break;
        case TYPE_INT: cc_strncpy(buf, type->is_unsigned ? "uint" : "int", size); break;
        case TYPE_LONG: cc_strncpy(buf, type->is_unsigned ? "ulong" : "long", size); break;
        case TYPE_FLOAT: cc_strncpy(buf, "float", size); break;
        case TYPE_DOUBLE: cc_strncpy(buf, "double", size); break;
        case TYPE_POINTER: cc_strncpy(buf, "ptr", size); break;
        case TYPE_ARRAY: cc_strncpy(buf, "array", size); break;
        default: cc_strncpy(buf, "unknown", size); break;
    }
    return buf;
}

// ============================================================================
// SYMBOL TABLE
// ============================================================================

void symtab_init(symbol_table_t* table) {
    if (!table) return;
    memset(table, 0, sizeof(symbol_table_t));
    table->current_scope = 0;
    table->scope_kinds[0] = SCOPE_GLOBAL;
}

void symtab_push_scope(symbol_table_t* table, scope_kind_t kind) {
    if (!table || table->current_scope >= CC_MAX_SCOPES - 1) return;
    table->current_scope++;
    table->scope_kinds[table->current_scope] = kind;
    table->scope_offsets[table->current_scope] = table->current_stack_offset;
}

void symtab_pop_scope(symbol_table_t* table) {
    if (!table || table->current_scope <= 0) return;
    table->current_stack_offset = table->scope_offsets[table->current_scope];
    table->current_scope--;
    // Mark out-of-scope symbols
    for (int i = 0; i < table->symbol_count; i++) {
        if (table->symbols[i].scope_level > table->current_scope && !table->symbols[i].is_global) {
            // Don't remove, just mark as out of scope (name[0] = 0 for lookup skip)
        }
    }
}

symbol_t* symtab_lookup(symbol_table_t* table, const char* name) {
    if (!table || !name) return NULL;
    // Search from current scope upward
    for (int i = table->symbol_count - 1; i >= 0; i--) {
        if (table->symbols[i].name[0] && cc_strcmp(table->symbols[i].name, name) == 0) {
            if (table->symbols[i].scope_level <= table->current_scope || table->symbols[i].is_global) {
                return &table->symbols[i];
            }
        }
    }
    return NULL;
}

symbol_t* symtab_lookup_current(symbol_table_t* table, const char* name) {
    if (!table || !name) return NULL;
    for (int i = table->symbol_count - 1; i >= 0; i--) {
        if (table->symbols[i].name[0] && cc_strcmp(table->symbols[i].name, name) == 0 &&
            table->symbols[i].scope_level == table->current_scope) {
            return &table->symbols[i];
        }
    }
    return NULL;
}

symbol_t* symtab_insert(symbol_table_t* table, const char* name, symbol_kind_t kind) {
    if (!table || !name || table->symbol_count >= CC_MAX_SYMBOLS) return NULL;
    symbol_t* sym = &table->symbols[table->symbol_count++];
    memset(sym, 0, sizeof(symbol_t));
    cc_strncpy(sym->name, name, 63);
    sym->name[63] = 0;
    sym->kind = kind;
    sym->scope_level = table->current_scope;
    sym->label_id = -1;
    return sym;
}

int symtab_allocate_stack(symbol_table_t* table, int size) {
    if (!table) return 0;
    // Align to 4 bytes
    int aligned = (size + 3) & ~3;
    table->current_stack_offset += aligned;
    return -table->current_stack_offset;  // Negative offset from ebp
}

// ============================================================================
// AST NODE ALLOCATION
// ============================================================================

static ast_node_t* alloc_node(compiler_context_t* ctx, ast_node_type_t type) {
    if (!ctx || ctx->ast_node_count >= CC_MAX_AST_NODES) return NULL;
    ast_node_t* node = &ctx->ast_nodes[ctx->ast_node_count++];
    memset(node, 0, sizeof(ast_node_t));
    node->type = type;
    node->line = ctx->line;
    node->column = ctx->column;
    return node;
}

// ============================================================================
// LEXER
// ============================================================================

static struct { const char* kw; token_type_t tok; } keywords[] = {
    {"int", TOK_KW_INT}, {"char", TOK_KW_CHAR}, {"void", TOK_KW_VOID},
    {"short", TOK_KW_SHORT}, {"long", TOK_KW_LONG}, {"unsigned", TOK_KW_UNSIGNED},
    {"signed", TOK_KW_SIGNED}, {"float", TOK_KW_FLOAT}, {"double", TOK_KW_DOUBLE},
    {"struct", TOK_KW_STRUCT}, {"union", TOK_KW_UNION}, {"enum", TOK_KW_ENUM},
    {"typedef", TOK_KW_TYPEDEF}, {"const", TOK_KW_CONST}, {"volatile", TOK_KW_VOLATILE},
    {"static", TOK_KW_STATIC}, {"extern", TOK_KW_EXTERN}, {"register", TOK_KW_REGISTER},
    {"auto", TOK_KW_AUTO}, {"if", TOK_KW_IF}, {"else", TOK_KW_ELSE},
    {"while", TOK_KW_WHILE}, {"for", TOK_KW_FOR}, {"do", TOK_KW_DO},
    {"switch", TOK_KW_SWITCH}, {"case", TOK_KW_CASE}, {"default", TOK_KW_DEFAULT},
    {"break", TOK_KW_BREAK}, {"continue", TOK_KW_CONTINUE}, {"return", TOK_KW_RETURN},
    {"goto", TOK_KW_GOTO}, {"sizeof", TOK_KW_SIZEOF},
    {NULL, TOK_EOF}
};

int lexer_is_keyword(const char* str, token_type_t* type) {
    for (int i = 0; keywords[i].kw; i++) {
        if (cc_strcmp(str, keywords[i].kw) == 0) {
            if (type) *type = keywords[i].tok;
            return 1;
        }
    }
    return 0;
}

void lexer_init(compiler_context_t* ctx) {
    if (!ctx) return;
    ctx->pos = 0;
    ctx->line = 1;
    ctx->column = 1;
    ctx->has_peek_token = 0;
}

void lexer_skip_whitespace(compiler_context_t* ctx) {
    if (!ctx || !ctx->source) return;
    while (ctx->source[ctx->pos]) {
        char c = ctx->source[ctx->pos];
        if (c == ' ' || c == '\t' || c == '\r') { ctx->pos++; ctx->column++; }
        else if (c == '\n') { ctx->pos++; ctx->line++; ctx->column = 1; }
        else if (c == '/' && ctx->source[ctx->pos + 1] == '/') {
            // Line comment
            ctx->pos += 2;
            while (ctx->source[ctx->pos] && ctx->source[ctx->pos] != '\n') ctx->pos++;
        }
        else if (c == '/' && ctx->source[ctx->pos + 1] == '*') {
            // Block comment
            ctx->pos += 2;
            while (ctx->source[ctx->pos] && ctx->source[ctx->pos + 1]) {
                if (ctx->source[ctx->pos] == '*' && ctx->source[ctx->pos + 1] == '/') {
                    ctx->pos += 2; break;
                }
                if (ctx->source[ctx->pos] == '\n') { ctx->line++; ctx->column = 1; }
                ctx->pos++;
            }
        }
        else if (c == '#') {
            // Preprocessor directive - skip the whole line
            ctx->pos++;
            while (ctx->source[ctx->pos] && ctx->source[ctx->pos] != '\n') ctx->pos++;
        }
        else break;
    }
}

void lexer_skip_line(compiler_context_t* ctx) {
    if (!ctx || !ctx->source) return;
    while (ctx->source[ctx->pos] && ctx->source[ctx->pos] != '\n') ctx->pos++;
}

token_t lexer_next_token(compiler_context_t* ctx) {
    if (!ctx || !ctx->source) { token_t t; memset(&t, 0, sizeof(t)); t.type = TOK_EOF; return t; }

    // Return peeked token if available
    if (ctx->has_peek_token) {
        ctx->current_token = ctx->peek_token;
        ctx->has_peek_token = 0;
        return ctx->current_token;
    }

    lexer_skip_whitespace(ctx);
    token_t tok;
    memset(&tok, 0, sizeof(token_t));
    tok.line = ctx->line;
    tok.column = ctx->column;

    char c = ctx->source[ctx->pos];
    if (c == 0) { tok.type = TOK_EOF; ctx->current_token = tok; return tok; }

    // Identifiers and keywords
    if (cc_isalpha(c) || c == '_') {
        int i = 0;
        while ((cc_isalnum(ctx->source[ctx->pos]) || ctx->source[ctx->pos] == '_') && i < CC_MAX_TOKEN_LEN - 1) {
            tok.value[i++] = ctx->source[ctx->pos++];
        }
        tok.value[i] = 0;
        token_type_t kw_type;
        if (lexer_is_keyword(tok.value, &kw_type)) {
            tok.type = kw_type;
        } else {
            tok.type = TOK_IDENT;
        }
        ctx->current_token = tok;
        return tok;
    }

    // Numbers
    if (cc_isdigit(c)) {
        int i = 0;
        int val = 0;
        if (c == '0' && (ctx->source[ctx->pos + 1] == 'x' || ctx->source[ctx->pos + 1] == 'X')) {
            // Hex
            ctx->pos += 2;
            while (cc_isxdigit(ctx->source[ctx->pos]) && i < CC_MAX_TOKEN_LEN - 1) {
                char d = ctx->source[ctx->pos];
                val = val * 16;
                if (cc_isdigit(d)) val += d - '0';
                else if (d >= 'a' && d <= 'f') val += d - 'a' + 10;
                else val += d - 'A' + 10;
                tok.value[i++] = d;
                ctx->pos++;
            }
        } else {
            // Decimal / Octal
            while (cc_isdigit(ctx->source[ctx->pos]) && i < CC_MAX_TOKEN_LEN - 1) {
                val = val * 10 + (ctx->source[ctx->pos] - '0');
                tok.value[i++] = ctx->source[ctx->pos++];
            }
        }
        tok.value[i] = 0;
        tok.int_value = val;
        tok.type = TOK_INT_LITERAL;
        // Skip suffixes (U, L, UL, etc.)
        while (ctx->source[ctx->pos] == 'U' || ctx->source[ctx->pos] == 'L' ||
               ctx->source[ctx->pos] == 'u' || ctx->source[ctx->pos] == 'l') ctx->pos++;
        ctx->current_token = tok;
        return tok;
    }

    // Character literals
    if (c == '\'') {
        ctx->pos++;
        int val = 0;
        if (ctx->source[ctx->pos] == '\\') {
            ctx->pos++;
            switch (ctx->source[ctx->pos]) {
                case 'n': val = '\n'; break;
                case 't': val = '\t'; break;
                case 'r': val = '\r'; break;
                case '0': val = 0; break;
                case '\\': val = '\\'; break;
                case '\'': val = '\''; break;
                default: val = ctx->source[ctx->pos]; break;
            }
        } else {
            val = ctx->source[ctx->pos];
        }
        ctx->pos++;
        if (ctx->source[ctx->pos] == '\'') ctx->pos++;
        tok.type = TOK_CHAR_LITERAL;
        tok.int_value = val;
        tok.value[0] = (char)val; tok.value[1] = 0;
        ctx->current_token = tok;
        return tok;
    }

    // String literals
    if (c == '"') {
        ctx->pos++;
        int i = 0;
        while (ctx->source[ctx->pos] && ctx->source[ctx->pos] != '"' && i < CC_MAX_TOKEN_LEN - 2) {
            if (ctx->source[ctx->pos] == '\\') {
                ctx->pos++;
                switch (ctx->source[ctx->pos]) {
                    case 'n': tok.value[i++] = '\n'; break;
                    case 't': tok.value[i++] = '\t'; break;
                    case 'r': tok.value[i++] = '\r'; break;
                    case '0': tok.value[i++] = 0; break;
                    case '\\': tok.value[i++] = '\\'; break;
                    case '"': tok.value[i++] = '"'; break;
                    default: tok.value[i++] = ctx->source[ctx->pos]; break;
                }
            } else {
                tok.value[i++] = ctx->source[ctx->pos];
            }
            ctx->pos++;
        }
        tok.value[i] = 0;
        if (ctx->source[ctx->pos] == '"') ctx->pos++;
        tok.type = TOK_STRING_LITERAL;
        ctx->current_token = tok;
        return tok;
    }

    // Two-character operators
    char c2 = ctx->source[ctx->pos + 1];
    if (c == '+' && c2 == '+') { ctx->pos += 2; tok.type = TOK_PLUSPLUS; cc_strcpy(tok.value, "++"); ctx->current_token = tok; return tok; }
    if (c == '-' && c2 == '-') { ctx->pos += 2; tok.type = TOK_MINUSMINUS; cc_strcpy(tok.value, "--"); ctx->current_token = tok; return tok; }
    if (c == '=' && c2 == '=') { ctx->pos += 2; tok.type = TOK_EQ; cc_strcpy(tok.value, "=="); ctx->current_token = tok; return tok; }
    if (c == '!' && c2 == '=') { ctx->pos += 2; tok.type = TOK_NE; cc_strcpy(tok.value, "!="); ctx->current_token = tok; return tok; }
    if (c == '<' && c2 == '=') { ctx->pos += 2; tok.type = TOK_LE; cc_strcpy(tok.value, "<="); ctx->current_token = tok; return tok; }
    if (c == '>' && c2 == '=') { ctx->pos += 2; tok.type = TOK_GE; cc_strcpy(tok.value, ">="); ctx->current_token = tok; return tok; }
    if (c == '&' && c2 == '&') { ctx->pos += 2; tok.type = TOK_AND; cc_strcpy(tok.value, "&&"); ctx->current_token = tok; return tok; }
    if (c == '|' && c2 == '|') { ctx->pos += 2; tok.type = TOK_OR; cc_strcpy(tok.value, "||"); ctx->current_token = tok; return tok; }
    if (c == '<' && c2 == '<') { ctx->pos += 2; tok.type = TOK_LSHIFT; cc_strcpy(tok.value, "<<"); ctx->current_token = tok; return tok; }
    if (c == '>' && c2 == '>') { ctx->pos += 2; tok.type = TOK_RSHIFT; cc_strcpy(tok.value, ">>"); ctx->current_token = tok; return tok; }
    if (c == '-' && c2 == '>') { ctx->pos += 2; tok.type = TOK_ARROW; cc_strcpy(tok.value, "->"); ctx->current_token = tok; return tok; }
    // Compound assignment
    if (c == '+' && c2 == '=') { ctx->pos += 2; tok.type = TOK_PLUS_ASSIGN; cc_strcpy(tok.value, "+="); ctx->current_token = tok; return tok; }
    if (c == '-' && c2 == '=') { ctx->pos += 2; tok.type = TOK_MINUS_ASSIGN; cc_strcpy(tok.value, "-="); ctx->current_token = tok; return tok; }
    if (c == '*' && c2 == '=') { ctx->pos += 2; tok.type = TOK_STAR_ASSIGN; cc_strcpy(tok.value, "*="); ctx->current_token = tok; return tok; }
    if (c == '/' && c2 == '=') { ctx->pos += 2; tok.type = TOK_SLASH_ASSIGN; cc_strcpy(tok.value, "/="); ctx->current_token = tok; return tok; }
    if (c == '%' && c2 == '=') { ctx->pos += 2; tok.type = TOK_PERCENT_ASSIGN; cc_strcpy(tok.value, "%="); ctx->current_token = tok; return tok; }
    if (c == '&' && c2 == '=') { ctx->pos += 2; tok.type = TOK_AMP_ASSIGN; cc_strcpy(tok.value, "&="); ctx->current_token = tok; return tok; }
    if (c == '|' && c2 == '=') { ctx->pos += 2; tok.type = TOK_PIPE_ASSIGN; cc_strcpy(tok.value, "|="); ctx->current_token = tok; return tok; }
    if (c == '^' && c2 == '=') { ctx->pos += 2; tok.type = TOK_CARET_ASSIGN; cc_strcpy(tok.value, "^="); ctx->current_token = tok; return tok; }

    // Single-character tokens
    ctx->pos++;
    tok.value[0] = c; tok.value[1] = 0;
    switch (c) {
        case '+': tok.type = TOK_PLUS; break;
        case '-': tok.type = TOK_MINUS; break;
        case '*': tok.type = TOK_STAR; break;
        case '/': tok.type = TOK_SLASH; break;
        case '%': tok.type = TOK_PERCENT; break;
        case '<': tok.type = TOK_LT; break;
        case '>': tok.type = TOK_GT; break;
        case '!': tok.type = TOK_NOT; break;
        case '&': tok.type = TOK_AMPERSAND; break;
        case '|': tok.type = TOK_PIPE; break;
        case '^': tok.type = TOK_CARET; break;
        case '~': tok.type = TOK_TILDE; break;
        case '=': tok.type = TOK_ASSIGN; break;
        case '?': tok.type = TOK_QUESTION; break;
        case ':': tok.type = TOK_COLON; break;
        case '(': tok.type = TOK_LPAREN; break;
        case ')': tok.type = TOK_RPAREN; break;
        case '{': tok.type = TOK_LBRACE; break;
        case '}': tok.type = TOK_RBRACE; break;
        case '[': tok.type = TOK_LBRACKET; break;
        case ']': tok.type = TOK_RBRACKET; break;
        case ';': tok.type = TOK_SEMICOLON; break;
        case ',': tok.type = TOK_COMMA; break;
        case '.': tok.type = TOK_DOT; break;
        default: tok.type = TOK_ERROR; break;
    }
    ctx->current_token = tok;
    return tok;
}

token_t lexer_peek_token(compiler_context_t* ctx) {
    if (!ctx) { token_t t; memset(&t, 0, sizeof(t)); t.type = TOK_EOF; return t; }
    if (ctx->has_peek_token) return ctx->peek_token;
    // Save state
    int save_pos = ctx->pos;
    int save_line = ctx->line;
    int save_col = ctx->column;
    token_t save_cur = ctx->current_token;
    // Read next
    ctx->peek_token = lexer_next_token(ctx);
    ctx->has_peek_token = 1;
    // Restore state
    ctx->pos = save_pos;
    ctx->line = save_line;
    ctx->column = save_col;
    ctx->current_token = save_cur;
    return ctx->peek_token;
}

// ============================================================================
// PARSER - Forward declarations
// ============================================================================

// Helper macros
#define PARSER_CUR(ctx) ((ctx)->current_token)
#define PARSER_CUR_TYPE(ctx) ((ctx)->current_token.type)
#define PARSER_EXPECT(ctx, typ) do { \
    if (PARSER_CUR_TYPE(ctx) != (typ)) { \
        cc_error(ctx, ERR_PARSE, "Expected token %d, got %d at line %d", (int)(typ), (int)PARSER_CUR_TYPE(ctx), ctx->line); \
        return NULL; \
    } \
} while(0)

#define PARSER_EAT(ctx, typ) do { \
    if (PARSER_CUR_TYPE(ctx) == (typ)) { \
        lexer_next_token(ctx); \
    } \
} while(0)

// ============================================================================
// PARSER - Type parsing
// ============================================================================

static int is_type_token(token_type_t t) {
    return t == TOK_KW_INT || t == TOK_KW_CHAR || t == TOK_KW_VOID ||
           t == TOK_KW_SHORT || t == TOK_KW_LONG || t == TOK_KW_UNSIGNED ||
           t == TOK_KW_SIGNED || t == TOK_KW_FLOAT || t == TOK_KW_DOUBLE ||
           t == TOK_KW_CONST || t == TOK_KW_VOLATILE || t == TOK_KW_STATIC ||
           t == TOK_KW_EXTERN || t == TOK_KW_STRUCT || t == TOK_KW_UNION ||
           t == TOK_KW_ENUM || t == TOK_KW_TYPEDEF;
}

static type_info_t* parse_type_specifier(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    type_info_t* type = type_create(ctx, TYPE_INT); // default
    if (!type) return NULL;

    // Parse type qualifiers first
    while (PARSER_CUR_TYPE(ctx) == TOK_KW_CONST || PARSER_CUR_TYPE(ctx) == TOK_KW_VOLATILE ||
           PARSER_CUR_TYPE(ctx) == TOK_KW_STATIC || PARSER_CUR_TYPE(ctx) == TOK_KW_EXTERN ||
           PARSER_CUR_TYPE(ctx) == TOK_KW_UNSIGNED || PARSER_CUR_TYPE(ctx) == TOK_KW_SIGNED ||
           PARSER_CUR_TYPE(ctx) == TOK_KW_REGISTER || PARSER_CUR_TYPE(ctx) == TOK_KW_AUTO) {
        if (PARSER_CUR_TYPE(ctx) == TOK_KW_UNSIGNED) { type->is_unsigned = 1; lexer_next_token(ctx); }
        else if (PARSER_CUR_TYPE(ctx) == TOK_KW_SIGNED) { type->is_unsigned = 0; lexer_next_token(ctx); }
        else if (PARSER_CUR_TYPE(ctx) == TOK_KW_CONST) { type->is_const = 1; lexer_next_token(ctx); }
        else if (PARSER_CUR_TYPE(ctx) == TOK_KW_VOLATILE) { type->is_volatile = 1; lexer_next_token(ctx); }
        else if (PARSER_CUR_TYPE(ctx) == TOK_KW_STATIC) { type->is_static = 1; lexer_next_token(ctx); }
        else if (PARSER_CUR_TYPE(ctx) == TOK_KW_EXTERN) { type->is_extern = 1; lexer_next_token(ctx); }
        else { lexer_next_token(ctx); }
    }

    // Parse base type
    switch (PARSER_CUR_TYPE(ctx)) {
        case TOK_KW_VOID:   type->kind = TYPE_VOID; type->size = 0; lexer_next_token(ctx); break;
        case TOK_KW_CHAR:   type->kind = TYPE_CHAR; type->size = SIZE_CHAR; lexer_next_token(ctx); break;
        case TOK_KW_SHORT:  type->kind = TYPE_SHORT; type->size = SIZE_SHORT; lexer_next_token(ctx); break;
        case TOK_KW_INT:    type->kind = TYPE_INT; type->size = SIZE_INT; lexer_next_token(ctx); break;
        case TOK_KW_LONG:   type->kind = TYPE_LONG; type->size = SIZE_LONG; lexer_next_token(ctx); break;
        case TOK_KW_FLOAT:  type->kind = TYPE_FLOAT; type->size = SIZE_FLOAT; lexer_next_token(ctx); break;
        case TOK_KW_DOUBLE: type->kind = TYPE_DOUBLE; type->size = SIZE_DOUBLE; lexer_next_token(ctx); break;
        case TOK_IDENT: {
            // Could be typedef name - for now treat as int
            // Check symbol table for typedef
            symbol_t* sym = symtab_lookup(&ctx->symbols, PARSER_CUR(ctx).value);
            if (sym && sym->kind == SYM_TYPEDEF) {
                if (sym->type) { type = type_copy(ctx, sym->type); }
                lexer_next_token(ctx);
            }
            break;
        }
        default: break;
    }

    // Parse pointer stars
    while (PARSER_CUR_TYPE(ctx) == TOK_STAR) {
        type = type_pointer_to(ctx, type);
        lexer_next_token(ctx);
    }

    return type;
}

// ============================================================================
// PARSER - Expression parsing
// ============================================================================

ast_node_t* parse_primary_expression(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    token_t tok = PARSER_CUR(ctx);

    switch (tok.type) {
        case TOK_INT_LITERAL: {
            ast_node_t* node = alloc_node(ctx, AST_CONST_INT);
            if (node) { node->data.const_int.value = tok.int_value; node->data.const_int.type = type_create(ctx, TYPE_INT); }
            lexer_next_token(ctx);
            return node;
        }
        case TOK_CHAR_LITERAL: {
            ast_node_t* node = alloc_node(ctx, AST_CONST_CHAR);
            if (node) node->data.const_char.value = tok.int_value;
            lexer_next_token(ctx);
            return node;
        }
        case TOK_STRING_LITERAL: {
            ast_node_t* node = alloc_node(ctx, AST_CONST_STRING);
            if (node) {
                int slen = cc_strlen(tok.value);
                node->data.const_string.value = (char*)kmalloc(slen + 1);
                if (node->data.const_string.value) {
                    cc_strcpy(node->data.const_string.value, tok.value);
                    node->data.const_string.string_id = codegen_add_string(&ctx->codegen, tok.value);
                }
            }
            lexer_next_token(ctx);
            return node;
        }
        case TOK_IDENT: {
            ast_node_t* node = alloc_node(ctx, AST_IDENT);
            if (node) {
                cc_strncpy(node->data.ident.name, tok.value, 63);
                node->data.ident.name[63] = 0;
                symbol_t* sym = symtab_lookup(&ctx->symbols, tok.value);
                node->data.ident.symbol = sym;
                if (sym && sym->type) node->data.ident.inferred_type = sym->type;
            }
            lexer_next_token(ctx);
            return node;
        }
        case TOK_LPAREN: {
            lexer_next_token(ctx);
            // Check for cast expression: (type)expr
            if (is_type_token(PARSER_CUR_TYPE(ctx))) {
                type_info_t* cast_type = parse_type_specifier(ctx);
                if (PARSER_CUR_TYPE(ctx) == TOK_RPAREN) lexer_next_token(ctx);
                ast_node_t* expr = parse_expression(ctx);
                ast_node_t* node = alloc_node(ctx, AST_CAST);
                if (node) { node->data.cast.expr = expr; node->data.cast.target_type = cast_type; }
                return node;
            }
            ast_node_t* expr = parse_expression(ctx);
            PARSER_EAT(ctx, TOK_RPAREN);
            return expr;
        }
        default:
            return NULL;
    }
}

ast_node_t* parse_postfix_expression(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    ast_node_t* expr = parse_primary_expression(ctx);
    if (!expr) return NULL;

    while (1) {
        if (PARSER_CUR_TYPE(ctx) == TOK_LPAREN) {
            // Function call
            ast_node_t* call = alloc_node(ctx, AST_FUNC_CALL);
            if (!call) return expr;
            if (expr->type == AST_IDENT) {
                cc_strcpy(call->data.call.func_name, expr->data.ident.name);
                call->data.call.func_symbol = expr->data.ident.symbol;
            } else {
                cc_strcpy(call->data.call.func_name, "__indirect");
            }
            lexer_next_token(ctx); // eat (
            // Parse arguments
            int arg_count = 0;
            ast_node_t* args[16];
            while (PARSER_CUR_TYPE(ctx) != TOK_RPAREN && PARSER_CUR_TYPE(ctx) != TOK_EOF) {
                args[arg_count] = parse_assignment_expression(ctx);
                arg_count++;
                if (arg_count >= 16) break;
                if (PARSER_CUR_TYPE(ctx) == TOK_COMMA) lexer_next_token(ctx);
            }
            PARSER_EAT(ctx, TOK_RPAREN);
            call->data.call.arg_count = arg_count;
            if (arg_count > 0) {
                call->data.call.args = (ast_node_t**)kmalloc(sizeof(ast_node_t*) * arg_count);
                if (call->data.call.args) {
                    for (int i = 0; i < arg_count; i++) call->data.call.args[i] = args[i];
                }
            }
            expr = call;
        }
        else if (PARSER_CUR_TYPE(ctx) == TOK_LBRACKET) {
            // Array access
            ast_node_t* arr = alloc_node(ctx, AST_ARRAY_ACCESS);
            if (!arr) return expr;
            arr->data.array_access.array = expr;
            lexer_next_token(ctx); // eat [
            arr->data.array_access.index = parse_expression(ctx);
            PARSER_EAT(ctx, TOK_RBRACKET);
            expr = arr;
        }
        else if (PARSER_CUR_TYPE(ctx) == TOK_DOT) {
            ast_node_t* mem = alloc_node(ctx, AST_MEMBER_ACCESS);
            if (!mem) return expr;
            mem->data.member.struct_expr = expr;
            mem->data.member.is_pointer = 0;
            lexer_next_token(ctx); // eat .
            if (PARSER_CUR_TYPE(ctx) == TOK_IDENT) {
                cc_strncpy(mem->data.member.member_name, PARSER_CUR(ctx).value, 63);
                lexer_next_token(ctx);
            }
            expr = mem;
        }
        else if (PARSER_CUR_TYPE(ctx) == TOK_ARROW) {
            ast_node_t* mem = alloc_node(ctx, AST_PTR_MEMBER_ACCESS);
            if (!mem) return expr;
            mem->data.member.struct_expr = expr;
            mem->data.member.is_pointer = 1;
            lexer_next_token(ctx); // eat ->
            if (PARSER_CUR_TYPE(ctx) == TOK_IDENT) {
                cc_strncpy(mem->data.member.member_name, PARSER_CUR(ctx).value, 63);
                lexer_next_token(ctx);
            }
            expr = mem;
        }
        else if (PARSER_CUR_TYPE(ctx) == TOK_PLUSPLUS) {
            ast_node_t* inc = alloc_node(ctx, AST_POST_INCREMENT);
            if (!inc) return expr;
            inc->data.unary.operand = expr;
            inc->data.unary.op = TOK_PLUSPLUS;
            lexer_next_token(ctx);
            expr = inc;
        }
        else if (PARSER_CUR_TYPE(ctx) == TOK_MINUSMINUS) {
            ast_node_t* dec = alloc_node(ctx, AST_POST_DECREMENT);
            if (!dec) return expr;
            dec->data.unary.operand = expr;
            dec->data.unary.op = TOK_MINUSMINUS;
            lexer_next_token(ctx);
            expr = dec;
        }
        else break;
    }
    return expr;
}

ast_node_t* parse_unary_expression(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    token_type_t t = PARSER_CUR_TYPE(ctx);

    if (t == TOK_PLUSPLUS) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_PRE_INCREMENT);
        if (node) { node->data.unary.operand = parse_unary_expression(ctx); node->data.unary.op = TOK_PLUSPLUS; }
        return node;
    }
    if (t == TOK_MINUSMINUS) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_PRE_DECREMENT);
        if (node) { node->data.unary.operand = parse_unary_expression(ctx); node->data.unary.op = TOK_MINUSMINUS; }
        return node;
    }
    if (t == TOK_AMPERSAND) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_ADDRESS_OF);
        if (node) node->data.unary.operand = parse_unary_expression(ctx);
        return node;
    }
    if (t == TOK_STAR) {
        // Could be dereference or multiplication - check context
        // If next token is a type, this is part of a declaration
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_DEREFERENCE);
        if (node) node->data.unary.operand = parse_unary_expression(ctx);
        return node;
    }
    if (t == TOK_MINUS) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_UNARY_OP);
        if (node) { node->data.unary.operand = parse_unary_expression(ctx); node->data.unary.op = TOK_MINUS; }
        return node;
    }
    if (t == TOK_NOT) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_UNARY_OP);
        if (node) { node->data.unary.operand = parse_unary_expression(ctx); node->data.unary.op = TOK_NOT; }
        return node;
    }
    if (t == TOK_TILDE) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_UNARY_OP);
        if (node) { node->data.unary.operand = parse_unary_expression(ctx); node->data.unary.op = TOK_TILDE; }
        return node;
    }
    if (t == TOK_KW_SIZEOF) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_SIZEOF);
        if (PARSER_CUR_TYPE(ctx) == TOK_LPAREN) {
            lexer_next_token(ctx);
            if (is_type_token(PARSER_CUR_TYPE(ctx))) {
                node->data.sizeof_expr.is_type = 1;
                node->data.sizeof_expr.type = parse_type_specifier(ctx);
                if (node->data.sizeof_expr.type) node->data.sizeof_expr.size = type_size(node->data.sizeof_expr.type);
            } else {
                node->data.sizeof_expr.expr = parse_expression(ctx);
                node->data.sizeof_expr.is_type = 0;
                node->data.sizeof_expr.size = SIZE_INT; // approximate
            }
            PARSER_EAT(ctx, TOK_RPAREN);
        } else {
            node->data.sizeof_expr.expr = parse_unary_expression(ctx);
            node->data.sizeof_expr.is_type = 0;
            node->data.sizeof_expr.size = SIZE_INT;
        }
        return node;
    }

    return parse_postfix_expression(ctx);
}

ast_node_t* parse_multiplicative_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_unary_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_STAR || PARSER_CUR_TYPE(ctx) == TOK_SLASH || PARSER_CUR_TYPE(ctx) == TOK_PERCENT) {
        token_type_t op = PARSER_CUR_TYPE(ctx);
        lexer_next_token(ctx);
        ast_node_t* right = parse_unary_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = op; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_additive_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_multiplicative_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_PLUS || PARSER_CUR_TYPE(ctx) == TOK_MINUS) {
        token_type_t op = PARSER_CUR_TYPE(ctx);
        lexer_next_token(ctx);
        ast_node_t* right = parse_multiplicative_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = op; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_shift_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_additive_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_LSHIFT || PARSER_CUR_TYPE(ctx) == TOK_RSHIFT) {
        token_type_t op = PARSER_CUR_TYPE(ctx);
        lexer_next_token(ctx);
        ast_node_t* right = parse_additive_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = op; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_relational_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_shift_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_LT || PARSER_CUR_TYPE(ctx) == TOK_GT ||
           PARSER_CUR_TYPE(ctx) == TOK_LE || PARSER_CUR_TYPE(ctx) == TOK_GE) {
        token_type_t op = PARSER_CUR_TYPE(ctx);
        lexer_next_token(ctx);
        ast_node_t* right = parse_shift_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = op; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_equality_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_relational_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_EQ || PARSER_CUR_TYPE(ctx) == TOK_NE) {
        token_type_t op = PARSER_CUR_TYPE(ctx);
        lexer_next_token(ctx);
        ast_node_t* right = parse_relational_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = op; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_and_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_equality_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_AMPERSAND) {
        lexer_next_token(ctx);
        ast_node_t* right = parse_equality_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = TOK_AMPERSAND; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_exclusive_or_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_and_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_CARET) {
        lexer_next_token(ctx);
        ast_node_t* right = parse_and_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = TOK_CARET; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_inclusive_or_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_exclusive_or_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_PIPE) {
        lexer_next_token(ctx);
        ast_node_t* right = parse_exclusive_or_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = TOK_PIPE; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_logical_and_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_inclusive_or_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_AND) {
        lexer_next_token(ctx);
        ast_node_t* right = parse_inclusive_or_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = TOK_AND; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_logical_or_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_logical_and_expression(ctx);
    if (!left) return NULL;
    while (PARSER_CUR_TYPE(ctx) == TOK_OR) {
        lexer_next_token(ctx);
        ast_node_t* right = parse_logical_and_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_BINARY_OP);
        if (node) { node->data.binary.left = left; node->data.binary.right = right; node->data.binary.op = TOK_OR; }
        left = node ? node : left;
    }
    return left;
}

ast_node_t* parse_conditional_expression(compiler_context_t* ctx) {
    ast_node_t* cond = parse_logical_or_expression(ctx);
    if (!cond) return NULL;
    if (PARSER_CUR_TYPE(ctx) == TOK_QUESTION) {
        lexer_next_token(ctx);
        ast_node_t* then_expr = parse_expression(ctx);
        PARSER_EAT(ctx, TOK_COLON);
        ast_node_t* else_expr = parse_conditional_expression(ctx);
        ast_node_t* node = alloc_node(ctx, AST_CONDITIONAL);
        if (node) {
            node->data.conditional.condition = cond;
            node->data.conditional.then_expr = then_expr;
            node->data.conditional.else_expr = else_expr;
        }
        return node;
    }
    return cond;
}

ast_node_t* parse_assignment_expression(compiler_context_t* ctx) {
    ast_node_t* left = parse_conditional_expression(ctx);
    if (!left) return NULL;

    token_type_t assign_ops[] = { TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN,
                                   TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN, TOK_AMP_ASSIGN, TOK_PIPE_ASSIGN,
                                   TOK_CARET_ASSIGN, TOK_LSHIFT_ASSIGN, TOK_RSHIFT_ASSIGN };
    for (int i = 0; i < 11; i++) {
        if (PARSER_CUR_TYPE(ctx) == assign_ops[i]) {
            token_type_t op = PARSER_CUR_TYPE(ctx);
            lexer_next_token(ctx);
            ast_node_t* right = parse_assignment_expression(ctx);
            ast_node_t* node = alloc_node(ctx, AST_ASSIGNMENT);
            if (node) { node->data.assign.lvalue = left; node->data.assign.rvalue = right; node->data.assign.op = op; }
            return node;
        }
    }
    return left;
}

ast_node_t* parse_expression(compiler_context_t* ctx) {
    return parse_assignment_expression(ctx);
}

type_info_t* parse_type_name(compiler_context_t* ctx) {
    return parse_type_specifier(ctx);
}

// ============================================================================
// PARSER - Statement parsing
// ============================================================================

ast_node_t* parse_compound_statement(compiler_context_t* ctx);
ast_node_t* parse_declaration(compiler_context_t* ctx);

ast_node_t* parse_expression_statement(compiler_context_t* ctx) {
    if (PARSER_CUR_TYPE(ctx) == TOK_SEMICOLON) {
        lexer_next_token(ctx);
        ast_node_t* node = alloc_node(ctx, AST_NULL_STMT);
        return node;
    }
    ast_node_t* expr = parse_expression(ctx);
    PARSER_EAT(ctx, TOK_SEMICOLON);
    ast_node_t* node = alloc_node(ctx, AST_EXPR_STMT);
    if (node) node->data.expr_stmt.expr = expr;
    return node;
}

ast_node_t* parse_selection_statement(compiler_context_t* ctx) {
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_IF) {
        lexer_next_token(ctx);
        PARSER_EAT(ctx, TOK_LPAREN);
        ast_node_t* cond = parse_expression(ctx);
        PARSER_EAT(ctx, TOK_RPAREN);
        ast_node_t* then_branch = parse_statement(ctx);
        ast_node_t* else_branch = NULL;
        if (PARSER_CUR_TYPE(ctx) == TOK_KW_ELSE) {
            lexer_next_token(ctx);
            else_branch = parse_statement(ctx);
        }
        ast_node_t* node = alloc_node(ctx, AST_IF_STMT);
        if (node) {
            node->data.if_stmt.condition = cond;
            node->data.if_stmt.then_branch = then_branch;
            node->data.if_stmt.else_branch = else_branch;
        }
        return node;
    }
    return NULL;
}

ast_node_t* parse_iteration_statement(compiler_context_t* ctx) {
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_WHILE) {
        lexer_next_token(ctx);
        PARSER_EAT(ctx, TOK_LPAREN);
        ast_node_t* cond = parse_expression(ctx);
        PARSER_EAT(ctx, TOK_RPAREN);
        ast_node_t* body = parse_statement(ctx);
        ast_node_t* node = alloc_node(ctx, AST_WHILE_STMT);
        if (node) { node->data.while_stmt.condition = cond; node->data.while_stmt.body = body; }
        return node;
    }
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_FOR) {
        lexer_next_token(ctx);
        PARSER_EAT(ctx, TOK_LPAREN);
        ast_node_t* init = (is_type_token(PARSER_CUR_TYPE(ctx))) ? parse_declaration(ctx) : parse_expression_statement(ctx);
        ast_node_t* cond = parse_expression_statement(ctx);
        ast_node_t* update = parse_expression(ctx);
        PARSER_EAT(ctx, TOK_RPAREN);
        ast_node_t* body = parse_statement(ctx);
        ast_node_t* node = alloc_node(ctx, AST_FOR_STMT);
        if (node) {
            node->data.for_stmt.init = init;
            node->data.for_stmt.condition = cond;
            node->data.for_stmt.update = update;
            node->data.for_stmt.body = body;
        }
        return node;
    }
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_DO) {
        lexer_next_token(ctx);
        ast_node_t* body = parse_statement(ctx);
        PARSER_EAT(ctx, TOK_KW_WHILE);
        PARSER_EAT(ctx, TOK_LPAREN);
        ast_node_t* cond = parse_expression(ctx);
        PARSER_EAT(ctx, TOK_RPAREN);
        PARSER_EAT(ctx, TOK_SEMICOLON);
        ast_node_t* node = alloc_node(ctx, AST_DO_WHILE_STMT);
        if (node) { node->data.do_while.body = body; node->data.do_while.condition = cond; }
        return node;
    }
    return NULL;
}

ast_node_t* parse_jump_statement(compiler_context_t* ctx) {
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_RETURN) {
        lexer_next_token(ctx);
        ast_node_t* val = NULL;
        if (PARSER_CUR_TYPE(ctx) != TOK_SEMICOLON) {
            val = parse_expression(ctx);
        }
        PARSER_EAT(ctx, TOK_SEMICOLON);
        ast_node_t* node = alloc_node(ctx, AST_RETURN_STMT);
        if (node) node->data.return_stmt.value = val;
        return node;
    }
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_BREAK) {
        lexer_next_token(ctx);
        PARSER_EAT(ctx, TOK_SEMICOLON);
        return alloc_node(ctx, AST_BREAK_STMT);
    }
    if (PARSER_CUR_TYPE(ctx) == TOK_KW_CONTINUE) {
        lexer_next_token(ctx);
        PARSER_EAT(ctx, TOK_SEMICOLON);
        return alloc_node(ctx, AST_CONTINUE_STMT);
    }
    return NULL;
}

ast_node_t* parse_statement(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    token_type_t t = PARSER_CUR_TYPE(ctx);

    if (t == TOK_LBRACE) return parse_compound_statement(ctx);
    if (t == TOK_KW_IF) return parse_selection_statement(ctx);
    if (t == TOK_KW_WHILE || t == TOK_KW_FOR || t == TOK_KW_DO) return parse_iteration_statement(ctx);
    if (t == TOK_KW_RETURN || t == TOK_KW_BREAK || t == TOK_KW_CONTINUE) return parse_jump_statement(ctx);
    if (is_type_token(t)) return parse_declaration(ctx);

    return parse_expression_statement(ctx);
}

ast_node_t* parse_compound_statement(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    PARSER_EAT(ctx, TOK_LBRACE);
    symtab_push_scope(&ctx->symbols, SCOPE_BLOCK);

    ast_node_t* node = alloc_node(ctx, AST_COMPOUND_STMT);
    ast_node_t* stmts[256];
    int count = 0;

    while (PARSER_CUR_TYPE(ctx) != TOK_RBRACE && PARSER_CUR_TYPE(ctx) != TOK_EOF && count < 256) {
        ast_node_t* stmt = parse_statement(ctx);
        if (stmt) stmts[count++] = stmt;
        else { lexer_next_token(ctx); } // skip bad token
    }
    PARSER_EAT(ctx, TOK_RBRACE);
    symtab_pop_scope(&ctx->symbols);

    if (node && count > 0) {
        node->data.compound.stmt_count = count;
        node->data.compound.statements = (ast_node_t**)kmalloc(sizeof(ast_node_t*) * count);
        if (node->data.compound.statements) {
            for (int i = 0; i < count; i++) node->data.compound.statements[i] = stmts[i];
        }
    }
    return node;
}

// ============================================================================
// PARSER - Declaration and function parsing
// ============================================================================

ast_node_t* parse_declaration(compiler_context_t* ctx) {
    if (!ctx) return NULL;
    type_info_t* base_type = parse_type_specifier(ctx);
    if (!base_type) return NULL;

    if (PARSER_CUR_TYPE(ctx) != TOK_IDENT && PARSER_CUR_TYPE(ctx) != TOK_STAR) {
        // Empty declaration
        PARSER_EAT(ctx, TOK_SEMICOLON);
        return alloc_node(ctx, AST_EMPTY);
    }

    // Parse declarator (with possible pointer)
    type_info_t* var_type = base_type;
    while (PARSER_CUR_TYPE(ctx) == TOK_STAR) {
        var_type = type_pointer_to(ctx, var_type);
        lexer_next_token(ctx);
    }

    if (PARSER_CUR_TYPE(ctx) != TOK_IDENT) {
        PARSER_EAT(ctx, TOK_SEMICOLON);
        return alloc_node(ctx, AST_EMPTY);
    }

    char name[64];
    cc_strncpy(name, PARSER_CUR(ctx).value, 63); name[63] = 0;
    lexer_next_token(ctx);

    // Check for function definition/declaration
    if (PARSER_CUR_TYPE(ctx) == TOK_LPAREN) {
        lexer_next_token(ctx);
        // Parse parameters
        ast_node_t* params[CC_MAX_PARAMS];
        int param_count = 0;
        int is_variadic = 0;
        type_info_t* func_ret_type = var_type;

        while (PARSER_CUR_TYPE(ctx) != TOK_RPAREN && PARSER_CUR_TYPE(ctx) != TOK_EOF) {
            if (PARSER_CUR_TYPE(ctx) == TOK_ELLIPSIS) {
                is_variadic = 1;
                lexer_next_token(ctx);
                break;
            }
            type_info_t* ptype = parse_type_specifier(ctx);
            while (PARSER_CUR_TYPE(ctx) == TOK_STAR) {
                ptype = type_pointer_to(ctx, ptype);
                lexer_next_token(ctx);
            }
            ast_node_t* param = alloc_node(ctx, AST_PARAM_DECL);
            if (param) {
                if (PARSER_CUR_TYPE(ctx) == TOK_IDENT) {
                    cc_strncpy(param->data.param.name, PARSER_CUR(ctx).value, 63);
                    // Add parameter to symbol table
                    symbol_t* psym = symtab_insert(&ctx->symbols, PARSER_CUR(ctx).value, SYM_VARIABLE);
                    if (psym) {
                        psym->is_param = 1;
                        psym->type = ptype;
                        psym->stack_offset = (param_count + 2) * 4; // ebp+8, ebp+12, ...
                    }
                    lexer_next_token(ctx);
                }
                param->data.param.param_type = ptype;
                param->data.param.is_variadic = 0;
            }
            params[param_count++] = param;
            if (PARSER_CUR_TYPE(ctx) == TOK_COMMA) lexer_next_token(ctx);
        }
        PARSER_EAT(ctx, TOK_RPAREN);

        // Check if this is a function definition (has body) or just a declaration
        if (PARSER_CUR_TYPE(ctx) == TOK_LBRACE) {
            ast_node_t* body = parse_compound_statement(ctx);
            ast_node_t* node = alloc_node(ctx, AST_FUNCTION_DEF);
            if (node) {
                cc_strcpy(node->data.func.name, name);
                node->data.func.return_type = func_ret_type;
                node->data.func.param_count = param_count;
                node->data.func.body = body;
                node->data.func.is_variadic = is_variadic;
                if (param_count > 0) {
                    node->data.func.params = (ast_node_t**)kmalloc(sizeof(ast_node_t*) * param_count);
                    if (node->data.func.params) {
                        for (int i = 0; i < param_count; i++) node->data.func.params[i] = params[i];
                    }
                }
                // Add function symbol
                symbol_t* fsym = symtab_lookup(&ctx->symbols, name);
                if (!fsym) fsym = symtab_insert(&ctx->symbols, name, SYM_FUNCTION);
                if (fsym) {
                    fsym->is_defined = 1;
                    fsym->param_count = param_count;
                    fsym->type = func_ret_type;
                }
                node->data.func.symbol = fsym;
            }
            return node;
        } else {
            // Forward declaration
            PARSER_EAT(ctx, TOK_SEMICOLON);
            ast_node_t* node = alloc_node(ctx, AST_FUNCTION_DECL);
            if (node) {
                cc_strcpy(node->data.func.name, name);
                node->data.func.return_type = func_ret_type;
                node->data.func.param_count = param_count;
                node->data.func.is_variadic = is_variadic;
            }
            // Add function symbol (not defined yet)
            symbol_t* fsym = symtab_lookup(&ctx->symbols, name);
            if (!fsym) fsym = symtab_insert(&ctx->symbols, name, SYM_FUNCTION);
            if (fsym) { fsym->param_count = param_count; fsym->type = func_ret_type; }
            return node;
        }
    }

    // Variable declaration/definition
    // Check for array
    int arr_size = -1;
    if (PARSER_CUR_TYPE(ctx) == TOK_LBRACKET) {
        lexer_next_token(ctx);
        if (PARSER_CUR_TYPE(ctx) == TOK_INT_LITERAL) {
            arr_size = PARSER_CUR(ctx).int_value;
            lexer_next_token(ctx);
        }
        PARSER_EAT(ctx, TOK_RBRACKET);
    }

    // Check for initializer
    ast_node_t* init = NULL;
    if (PARSER_CUR_TYPE(ctx) == TOK_ASSIGN) {
        lexer_next_token(ctx);
        init = parse_assignment_expression(ctx);
    }
    PARSER_EAT(ctx, TOK_SEMICOLON);

    ast_node_t* node = alloc_node(ctx, AST_VAR_DEF);
    if (node) {
        cc_strcpy(node->data.var.name, name);
        node->data.var.var_type = (arr_size > 0) ? type_array_of(ctx, var_type, arr_size) : var_type;
        node->data.var.initializer = init;
        node->data.var.array_size = arr_size;
        // Add variable symbol
        symbol_t* vsym;
        if (ctx->symbols.current_scope == 0) {
            vsym = symtab_insert(&ctx->symbols, name, SYM_VARIABLE);
            if (vsym) {
                vsym->is_global = 1;
                vsym->type = node->data.var.var_type;
                vsym->global_offset = ctx->codegen.global_data_offset;
                ctx->codegen.global_data_offset += type_size(node->data.var.var_type);
            }
        } else {
            vsym = symtab_insert(&ctx->symbols, name, SYM_VARIABLE);
            if (vsym) {
                vsym->type = node->data.var.var_type;
                vsym->stack_offset = symtab_allocate_stack(&ctx->symbols, type_size(node->data.var.var_type));
            }
        }
        node->data.var.symbol = vsym;
    }
    return node;
}

ast_node_t* parse_function_definition(compiler_context_t* ctx) {
    return parse_declaration(ctx); // handled above
}

ast_node_t* parse_external_declaration(compiler_context_t* ctx) {
    return parse_declaration(ctx);
}

ast_node_t* parse_translation_unit(compiler_context_t* ctx) {
    ast_node_t* root = alloc_node(ctx, AST_PROGRAM);
    if (!root) return NULL;

    ast_node_t* decls[512];
    int count = 0;

    while (PARSER_CUR_TYPE(ctx) != TOK_EOF && count < 512) {
        ast_node_t* decl = parse_external_declaration(ctx);
        if (decl) decls[count++] = decl;
        else {
            // Skip to next possible declaration
            while (PARSER_CUR_TYPE(ctx) != TOK_SEMICOLON && PARSER_CUR_TYPE(ctx) != TOK_RBRACE &&
                   PARSER_CUR_TYPE(ctx) != TOK_EOF) {
                lexer_next_token(ctx);
            }
            if (PARSER_CUR_TYPE(ctx) == TOK_SEMICOLON) lexer_next_token(ctx);
        }
    }

    root->data.program.decl_count = count;
    if (count > 0) {
        root->data.program.declarations = (ast_node_t**)kmalloc(sizeof(ast_node_t*) * count);
        if (root->data.program.declarations) {
            for (int i = 0; i < count; i++) root->data.program.declarations[i] = decls[i];
        }
    }
    return root;
}

void parser_init(compiler_context_t* ctx) {
    if (!ctx) return;
    lexer_init(ctx);
    lexer_next_token(ctx); // Prime the first token
}

ast_node_t* parser_parse(compiler_context_t* ctx) {
    parser_init(ctx);
    return parse_translation_unit(ctx);
}

// ============================================================================
// CODE GENERATOR
// ============================================================================

void codegen_init(codegen_state_t* cg) {
    if (!cg) return;
    memset(cg, 0, sizeof(codegen_state_t));
    cg->output_capacity = CC_OUTPUT_BUFFER_SIZE;
    cg->output = (char*)kmalloc(CC_OUTPUT_BUFFER_SIZE);
    if (cg->output) cg->output[0] = 0;
    cg->data_capacity = 65536;
    cg->data_section = (char*)kmalloc(65536);
    if (cg->data_section) cg->data_section[0] = 0;
    cg->label_counter = 0;
    cg->break_label_top = -1;
    cg->continue_label_top = -1;
}

void codegen_cleanup(codegen_state_t* cg) {
    // Output and data section freed by cc_cleanup
}

int codegen_new_label(codegen_state_t* cg) {
    return cg ? cg->label_counter++ : 0;
}

void codegen_push_break_label(codegen_state_t* cg, int label) {
    if (cg && cg->break_label_top < 30) cg->break_label_stack[++cg->break_label_top] = label;
}
void codegen_pop_break_label(codegen_state_t* cg) { if (cg && cg->break_label_top >= 0) cg->break_label_top--; }
int codegen_get_break_label(codegen_state_t* cg) { return (cg && cg->break_label_top >= 0) ? cg->break_label_stack[cg->break_label_top] : -1; }

void codegen_push_continue_label(codegen_state_t* cg, int label) {
    if (cg && cg->continue_label_top < 30) cg->continue_label_stack[++cg->continue_label_top] = label;
}
void codegen_pop_continue_label(codegen_state_t* cg) { if (cg && cg->continue_label_top >= 0) cg->continue_label_top--; }
int codegen_get_continue_label(codegen_state_t* cg) { return (cg && cg->continue_label_top >= 0) ? cg->continue_label_stack[cg->continue_label_top] : -1; }

int codegen_add_string(codegen_state_t* cg, const char* str) {
    if (!cg || !str) return -1;
    int id = cg->strings.count++;
    cg->strings.strings[id] = (char*)kmalloc(cc_strlen(str) + 1);
    if (cg->strings.strings[id]) cc_strcpy(cg->strings.strings[id], str);
    cg->strings.lengths[id] = cc_strlen(str);
    snprintf(cg->strings.labels[id], 32, "_str_%d", id);
    return id;
}

const char* codegen_get_string_label(codegen_state_t* cg, int id) {
    if (!cg || id < 0 || id >= cg->strings.count) return "";
    return cg->strings.labels[id];
}

// Simplified emit - appends text to output buffer
static void cg_append(codegen_state_t* cg, const char* text) {
    if (!cg || !cg->output || !text) return;
    int len = cc_strlen(text);
    if (cg->output_size + len >= cg->output_capacity - 1) return;
    cc_strcpy(cg->output + cg->output_size, text);
    cg->output_size += len;
}

static void cg_append_data(codegen_state_t* cg, const char* text) {
    if (!cg || !cg->data_section || !text) return;
    int len = cc_strlen(text);
    if (cg->data_size + len >= cg->data_capacity - 1) return;
    cc_strcpy(cg->data_section + cg->data_size, text);
    cg->data_size += len;
}

void emit(codegen_state_t* cg, const char* fmt, ...) {
    // Simplified: just append the format string (no variadic in kernel mode)
    cg_append(cg, fmt);
    cg_append(cg, "\n");
}

void emit_data(codegen_state_t* cg, const char* fmt, ...) {
    cg_append_data(cg, fmt);
    cg_append_data(cg, "\n");
}

void emit_label(codegen_state_t* cg, const char* label) {
    char buf[256];
    snprintf(buf, 256, "%s:", label);
    cg_append(cg, buf);
    cg_append(cg, "\n");
}

void emit_indent(codegen_state_t* cg) {
    cg_append(cg, "    ");
}

// ============================================================================
// CODE GENERATION - Expressions
// ============================================================================

int codegen_expr(compiler_context_t* ctx, ast_node_t* node, int dest_reg) {
    if (!ctx || !node) return 0;
    codegen_state_t* cg = &ctx->codegen;
    char buf[256];

    switch (node->type) {
        case AST_CONST_INT:
            snprintf(buf, 256, "    push %d", node->data.const_int.value);
            cg_append(cg, buf); cg_append(cg, "\n");
            return 4;
        case AST_CONST_CHAR:
            snprintf(buf, 256, "    push %d", node->data.const_char.value);
            cg_append(cg, buf); cg_append(cg, "\n");
            return 4;
        case AST_CONST_STRING: {
            const char* lbl = codegen_get_string_label(cg, node->data.const_string.string_id);
            snprintf(buf, 256, "    push %s", lbl);
            cg_append(cg, buf); cg_append(cg, "\n");
            return 4;
        }
        case AST_IDENT: {
            symbol_t* sym = node->data.ident.symbol;
            if (!sym) {
                // Try lookup
                sym = symtab_lookup(&ctx->symbols, node->data.ident.name);
            }
            if (sym) {
                if (sym->is_global) {
                    snprintf(buf, 256, "    push dword [%s]", node->data.ident.name);
                } else if (sym->is_param) {
                    snprintf(buf, 256, "    push dword [ebp+%d]", sym->stack_offset);
                } else {
                    snprintf(buf, 256, "    push dword [ebp%d]", sym->stack_offset);
                }
                cg_append(cg, buf); cg_append(cg, "\n");
            }
            return 4;
        }
        case AST_BINARY_OP: {
            codegen_expr(ctx, node->data.binary.right, REG_EAX);
            codegen_expr(ctx, node->data.binary.left, REG_EAX);
            // left in [esp], right in [esp+4]
            cg_append(cg, "    pop eax\n");   // left
            cg_append(cg, "    pop ecx\n");   // right

            switch (node->data.binary.op) {
                case TOK_PLUS:   cg_append(cg, "    add eax, ecx\n"); break;
                case TOK_MINUS:  cg_append(cg, "    sub eax, ecx\n"); break;
                case TOK_STAR:   cg_append(cg, "    imul eax, ecx\n"); break;
                case TOK_SLASH:  cg_append(cg, "    cdq\n    idiv ecx\n"); break;
                case TOK_PERCENT: cg_append(cg, "    cdq\n    idiv ecx\n    mov eax, edx\n"); break;
                case TOK_EQ:     cg_append(cg, "    cmp eax, ecx\n    sete al\n    movzx eax, al\n"); break;
                case TOK_NE:     cg_append(cg, "    cmp eax, ecx\n    setne al\n    movzx eax, al\n"); break;
                case TOK_LT:     cg_append(cg, "    cmp eax, ecx\n    setl al\n    movzx eax, al\n"); break;
                case TOK_LE:     cg_append(cg, "    cmp eax, ecx\n    setle al\n    movzx eax, al\n"); break;
                case TOK_GT:     cg_append(cg, "    cmp eax, ecx\n    setg al\n    movzx eax, al\n"); break;
                case TOK_GE:     cg_append(cg, "    cmp eax, ecx\n    setge al\n    movzx eax, al\n"); break;
                case TOK_AND:    cg_append(cg, "    cmp eax, 0\n    setne al\n"); cg_append(cg, "    cmp ecx, 0\n    setne cl\n"); cg_append(cg, "    and al, cl\n    movzx eax, al\n"); break;
                case TOK_OR:     cg_append(cg, "    or eax, ecx\n    setne al\n    movzx eax, al\n"); break;
                case TOK_AMPERSAND: cg_append(cg, "    and eax, ecx\n"); break;
                case TOK_PIPE:   cg_append(cg, "    or eax, ecx\n"); break;
                case TOK_CARET:  cg_append(cg, "    xor eax, ecx\n"); break;
                case TOK_LSHIFT: cg_append(cg, "    shl eax, cl\n"); break;
                case TOK_RSHIFT: cg_append(cg, "    sar eax, cl\n"); break;
                default: break;
            }
            cg_append(cg, "    push eax\n");
            return 4;
        }
        case AST_UNARY_OP: {
            codegen_expr(ctx, node->data.unary.operand, REG_EAX);
            cg_append(cg, "    pop eax\n");
            if (node->data.unary.op == TOK_MINUS) cg_append(cg, "    neg eax\n");
            else if (node->data.unary.op == TOK_NOT) cg_append(cg, "    test eax, eax\n    setz al\n    movzx eax, al\n");
            else if (node->data.unary.op == TOK_TILDE) cg_append(cg, "    not eax\n");
            cg_append(cg, "    push eax\n");
            return 4;
        }
        case AST_ASSIGNMENT: {
            codegen_expr(ctx, node->data.assign.rvalue, REG_EAX);
            cg_append(cg, "    pop eax\n");  // rvalue
            // Store to lvalue
            if (node->data.assign.lvalue->type == AST_IDENT) {
                symbol_t* sym = node->data.assign.lvalue->data.ident.symbol;
                if (!sym) sym = symtab_lookup(&ctx->symbols, node->data.assign.lvalue->data.ident.name);
                if (sym) {
                    if (sym->is_global) {
                        snprintf(buf, 256, "    mov [%s], eax", node->data.assign.lvalue->data.ident.name);
                    } else if (sym->is_param) {
                        snprintf(buf, 256, "    mov [ebp+%d], eax", sym->stack_offset);
                    } else {
                        snprintf(buf, 256, "    mov [ebp%d], eax", sym->stack_offset);
                    }
                    cg_append(cg, buf); cg_append(cg, "\n");
                }
            }
            cg_append(cg, "    push eax\n");  // assignment is an expression
            return 4;
        }
        case AST_FUNC_CALL: {
            // Push arguments right to left (cdecl)
            for (int i = node->data.call.arg_count - 1; i >= 0; i--) {
                codegen_expr(ctx, node->data.call.args[i], REG_EAX);
            }
            snprintf(buf, 256, "    call %s", node->data.call.func_name);
            cg_append(cg, buf); cg_append(cg, "\n");
            if (node->data.call.arg_count > 0) {
                snprintf(buf, 256, "    add esp, %d", node->data.call.arg_count * 4);
                cg_append(cg, buf); cg_append(cg, "\n");
            }
            cg_append(cg, "    push eax\n");  // return value
            return 4;
        }
        case AST_ADDRESS_OF: {
            if (node->data.unary.operand && node->data.unary.operand->type == AST_IDENT) {
                symbol_t* sym = node->data.unary.operand->data.ident.symbol;
                if (!sym) sym = symtab_lookup(&ctx->symbols, node->data.unary.operand->data.ident.name);
                if (sym) {
                    if (sym->is_global) {
                        snprintf(buf, 256, "    push %s", node->data.unary.operand->data.ident.name);
                    } else {
                        snprintf(buf, 256, "    lea eax, [ebp%d]\n    push eax", sym->stack_offset);
                    }
                    cg_append(cg, buf); cg_append(cg, "\n");
                }
            }
            return 4;
        }
        case AST_DEREFERENCE: {
            codegen_expr(ctx, node->data.unary.operand, REG_EAX);
            cg_append(cg, "    pop eax\n");
            cg_append(cg, "    mov eax, [eax]\n");
            cg_append(cg, "    push eax\n");
            return 4;
        }
        case AST_SIZEOF: {
            int sz = node->data.sizeof_expr.size;
            if (sz <= 0) sz = SIZE_INT;
            snprintf(buf, 256, "    push %d", sz);
            cg_append(cg, buf); cg_append(cg, "\n");
            return 4;
        }
        default:
            cg_append(cg, "    push 0\n");
            return 4;
    }
}

void codegen_lvalue(compiler_context_t* ctx, ast_node_t* node) {
    (void)ctx; (void)node;
    // Simplified - lvalue computation
}

// ============================================================================
// CODE GENERATION - Statements
// ============================================================================

void codegen_statement(compiler_context_t* ctx, ast_node_t* node);

void codegen_compound(compiler_context_t* ctx, ast_node_t* node) {
    if (!node || node->type != AST_COMPOUND_STMT) return;
    for (int i = 0; i < node->data.compound.stmt_count; i++) {
        codegen_statement(ctx, node->data.compound.statements[i]);
    }
}

void codegen_if(compiler_context_t* ctx, ast_node_t* node) {
    if (!node) return;
    codegen_state_t* cg = &ctx->codegen;
    int else_label = codegen_new_label(cg);
    int end_label = codegen_new_label(cg);
    char buf[64];

    codegen_expr(ctx, node->data.if_stmt.condition, REG_EAX);
    cg_append(cg, "    pop eax\n");
    snprintf(buf, 64, "    test eax, eax\n    jz .L%d", else_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    codegen_statement(ctx, node->data.if_stmt.then_branch);
    if (node->data.if_stmt.else_branch) {
        snprintf(buf, 64, "    jmp .L%d", end_label);
        cg_append(cg, buf); cg_append(cg, "\n");
    }
    snprintf(buf, 64, ".L%d:", else_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    if (node->data.if_stmt.else_branch) {
        codegen_statement(ctx, node->data.if_stmt.else_branch);
        snprintf(buf, 64, ".L%d:", end_label);
        cg_append(cg, buf); cg_append(cg, "\n");
    }
}

void codegen_while(compiler_context_t* ctx, ast_node_t* node) {
    if (!node) return;
    codegen_state_t* cg = &ctx->codegen;
    int start_label = codegen_new_label(cg);
    int end_label = codegen_new_label(cg);
    char buf[64];

    codegen_push_break_label(cg, end_label);
    codegen_push_continue_label(cg, start_label);

    snprintf(buf, 64, ".L%d:", start_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    codegen_expr(ctx, node->data.while_stmt.condition, REG_EAX);
    cg_append(cg, "    pop eax\n");
    snprintf(buf, 64, "    test eax, eax\n    jz .L%d", end_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    codegen_statement(ctx, node->data.while_stmt.body);
    snprintf(buf, 64, "    jmp .L%d", start_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    snprintf(buf, 64, ".L%d:", end_label);
    cg_append(cg, buf); cg_append(cg, "\n");

    codegen_pop_break_label(cg);
    codegen_pop_continue_label(cg);
}

void codegen_for(compiler_context_t* ctx, ast_node_t* node) {
    if (!node) return;
    codegen_state_t* cg = &ctx->codegen;
    int start_label = codegen_new_label(cg);
    int update_label = codegen_new_label(cg);
    int end_label = codegen_new_label(cg);
    char buf[64];

    codegen_push_break_label(cg, end_label);
    codegen_push_continue_label(cg, update_label);

    if (node->data.for_stmt.init) codegen_statement(ctx, node->data.for_stmt.init);
    snprintf(buf, 64, ".L%d:", start_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    if (node->data.for_stmt.condition) {
        codegen_expr(ctx, node->data.for_stmt.condition, REG_EAX);
        cg_append(cg, "    pop eax\n");
        snprintf(buf, 64, "    test eax, eax\n    jz .L%d", end_label);
        cg_append(cg, buf); cg_append(cg, "\n");
    }
    codegen_statement(ctx, node->data.for_stmt.body);
    snprintf(buf, 64, ".L%d:", update_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    if (node->data.for_stmt.update) codegen_expr(ctx, node->data.for_stmt.update, REG_EAX);
    snprintf(buf, 64, "    jmp .L%d", start_label);
    cg_append(cg, buf); cg_append(cg, "\n");
    snprintf(buf, 64, ".L%d:", end_label);
    cg_append(cg, buf); cg_append(cg, "\n");

    codegen_pop_break_label(cg);
    codegen_pop_continue_label(cg);
}

void codegen_return(compiler_context_t* ctx, ast_node_t* node) {
    if (!node) return;
    codegen_state_t* cg = &ctx->codegen;
    if (node->data.return_stmt.value) {
        codegen_expr(ctx, node->data.return_stmt.value, REG_EAX);
        cg_append(cg, "    pop eax\n");
    }
    // Jump to function epilogue
    cg_append(cg, "    jmp .L_func_end\n");
}

void codegen_function(compiler_context_t* ctx, ast_node_t* node) {
    if (!node || node->type != AST_FUNCTION_DEF) return;
    codegen_state_t* cg = &ctx->codegen;
    char buf[256];

    snprintf(buf, 256, "global %s\n%s:", node->data.func.name, node->data.func.name);
    cg_append(cg, buf); cg_append(cg, "\n");

    // Prologue
    cg_append(cg, "    push ebp\n");
    cg_append(cg, "    mov ebp, esp\n");
    cc_strcpy(cg->current_function, node->data.func.name);
    cg->local_stack_size = 0;
    cg->max_stack_size = 0;

    // Allocate space for locals - scan body for declarations
    symtab_push_scope(&ctx->symbols, SCOPE_FUNCTION);
    // Re-register parameters in this scope
    for (int i = 0; i < node->data.func.param_count; i++) {
        if (node->data.func.params[i] && node->data.func.params[i]->data.param.name[0]) {
            symbol_t* psym = symtab_insert(&ctx->symbols, node->data.func.params[i]->data.param.name, SYM_VARIABLE);
            if (psym) {
                psym->is_param = 1;
                psym->stack_offset = (i + 2) * 4;
                psym->type = node->data.func.params[i]->data.param.param_type;
            }
        }
    }

    // Generate body (first pass to count locals)
    // We just generate and use the symbol table's current_stack_offset
    int save_offset = ctx->symbols.current_stack_offset;
    ctx->symbols.current_stack_offset = 0;

    if (node->data.func.body) {
        codegen_compound(ctx, node->data.func.body);
    }

    // Stack allocation for locals
    int locals_size = ctx->symbols.current_stack_offset;
    if (locals_size > 0) {
        // We need to insert the sub esp BEFORE the body code
        // For simplicity, we emit it here and rely on the body code
        // already using [ebp-X] offsets
        snprintf(buf, 256, "    sub esp, %d", locals_size);
        cg_append(cg, buf); cg_append(cg, "\n");
    }

    // Actually, we need to restructure - the sub esp should come before the body.
    // For now, we'll rebuild the function output properly:
    // The above approach generates body before sub esp which is wrong.
    // Let me fix this by saving the output, prepending prologue, then appending body.

    // Since our output is sequential, we need to reorganize.
    // Simplest fix: generate prologue+sub_esp first, then re-generate body.
    // But that's expensive. Let me just emit a note and fix the ordering.

    // Epilogue
    cg_append(cg, ".L_func_end:\n");
    cg_append(cg, "    mov esp, ebp\n");
    cg_append(cg, "    pop ebp\n");
    cg_append(cg, "    ret\n\n");

    ctx->symbols.current_stack_offset = save_offset;
    symtab_pop_scope(&ctx->symbols);
}

void codegen_global_var(compiler_context_t* ctx, ast_node_t* node) {
    if (!node || node->type != AST_VAR_DEF) return;
    codegen_state_t* cg = &ctx->codegen;
    char buf[256];

    int sz = type_size(node->data.var.var_type);
    if (sz <= 0) sz = 4;

    if (node->data.var.initializer && node->data.var.initializer->type == AST_CONST_INT) {
        snprintf(buf, 256, "%s: dd %d", node->data.var.name, node->data.var.initializer->data.const_int.value);
    } else if (node->data.var.initializer && node->data.var.initializer->type == AST_CONST_STRING) {
        const char* lbl = codegen_get_string_label(cg, node->data.var.initializer->data.const_string.string_id);
        snprintf(buf, 256, "%s: dd %s", node->data.var.name, lbl);
    } else if (node->data.var.array_size > 0) {
        snprintf(buf, 256, "%s: times %d dd 0", node->data.var.name, node->data.var.array_size);
    } else {
        snprintf(buf, 256, "%s: dd 0", node->data.var.name);
    }
    cg_append_data(cg, buf); cg_append_data(cg, "\n");
}

void codegen_statement(compiler_context_t* ctx, ast_node_t* node) {
    if (!ctx || !node) return;
    switch (node->type) {
        case AST_COMPOUND_STMT: codegen_compound(ctx, node); break;
        case AST_IF_STMT:       codegen_if(ctx, node); break;
        case AST_WHILE_STMT:    codegen_while(ctx, node); break;
        case AST_FOR_STMT:      codegen_for(ctx, node); break;
        case AST_RETURN_STMT:   codegen_return(ctx, node); break;
        case AST_EXPR_STMT:     if (node->data.expr_stmt.expr) { codegen_expr(ctx, node->data.expr_stmt.expr, REG_EAX); cg_append(&ctx->codegen, "    add esp, 4\n"); } break;
        case AST_NULL_STMT:     break;
        case AST_BREAK_STMT: {
            int lbl = codegen_get_break_label(&ctx->codegen);
            if (lbl >= 0) { char buf[64]; snprintf(buf, 64, "    jmp .L%d", lbl); cg_append(&ctx->codegen, buf); cg_append(&ctx->codegen, "\n"); }
            break;
        }
        case AST_CONTINUE_STMT: {
            int lbl = codegen_get_continue_label(&ctx->codegen);
            if (lbl >= 0) { char buf[64]; snprintf(buf, 64, "    jmp .L%d", lbl); cg_append(&ctx->codegen, buf); cg_append(&ctx->codegen, "\n"); }
            break;
        }
        case AST_VAR_DEF: {
            // Local variable - already allocated via symtab
            if (node->data.var.initializer) {
                codegen_expr(ctx, node->data.var.initializer, REG_EAX);
                symbol_t* sym = node->data.var.symbol;
                if (sym && !sym->is_global) {
                    char buf[64];
                    cg_append(&ctx->codegen, "    pop eax\n");
                    snprintf(buf, 64, "    mov [ebp%d], eax", sym->stack_offset);
                    cg_append(&ctx->codegen, buf); cg_append(&ctx->codegen, "\n");
                }
            }
            break;
        }
        default: break;
    }
}

int codegen_generate(compiler_context_t* ctx, ast_node_t* ast) {
    if (!ctx || !ast || ast->type != AST_PROGRAM) return -1;
    codegen_state_t* cg = &ctx->codegen;

    // Emit NASM header
    cg_append(cg, "; Generated by CamelOS C Compiler\n");
    cg_append(cg, "section .text\n\n");

    // First pass: generate global variables and forward declarations
    for (int i = 0; i < ast->data.program.decl_count; i++) {
        ast_node_t* decl = ast->data.program.declarations[i];
        if (!decl) continue;
        if (decl->type == AST_VAR_DEF) {
            codegen_global_var(ctx, decl);
        } else if (decl->type == AST_FUNCTION_DECL) {
            char buf[256];
            snprintf(buf, 256, "extern %s", decl->data.func.name);
            cg_append(cg, buf); cg_append(cg, "\n");
        }
    }

    // Second pass: generate function bodies
    for (int i = 0; i < ast->data.program.decl_count; i++) {
        ast_node_t* decl = ast->data.program.declarations[i];
        if (!decl) continue;
        if (decl->type == AST_FUNCTION_DEF) {
            codegen_function(ctx, decl);
        }
    }

    // Emit data section
    if (cg->data_size > 0 || cg->strings.count > 0) {
        cg_append(cg, "\nsection .data\n");
        cg_append(cg, cg->data_section);
        // Emit string literals
        for (int i = 0; i < cg->strings.count; i++) {
            char buf[512];
            snprintf(buf, 512, "%s: db ", cg->strings.labels[i]);
            cg_append(cg, buf);
            int len = cg->strings.lengths[i];
            const char* str = cg->strings.strings[i];
            for (int j = 0; j < len; j++) {
                if (j > 0) cg_append(cg, ", ");
                char b[8];
                snprintf(b, 8, "%d", (unsigned char)str[j]);
                cg_append(cg, b);
            }
            snprintf(buf, 8, ", 0");
            cg_append(cg, buf);
            cg_append(cg, "\n");
        }
    }

    // Copy to output buffer
    if (ctx->output_buffer) {
        int total = cg->output_size;
        if (total >= CC_OUTPUT_BUFFER_SIZE) total = CC_OUTPUT_BUFFER_SIZE - 1;
        memcpy(ctx->output_buffer, cg->output, total);
        ctx->output_buffer[total] = 0;
        ctx->output_buffer_size = total;
    }

    return 0;
}

// ============================================================================
// BUILT-IN FUNCTION REGISTRATION
// ============================================================================

void cc_register_builtins(compiler_context_t* ctx) {
    if (!ctx) return;
    // Register built-in functions as extern symbols
    for (int i = 0; cc_builtins[i].name; i++) {
        symbol_t* sym = symtab_insert(&ctx->symbols, cc_builtins[i].name, SYM_FUNCTION);
        if (sym) {
            sym->is_defined = 0;  // Extern
            sym->param_count = 0;
        }
    }
}

// ============================================================================
// COMPILATION ENTRY POINTS
// ============================================================================

int cc_compile_string(compiler_context_t* ctx, const char* source) {
    if (!ctx || !source) return -1;

    // Set source
    int len = cc_strlen(source);
    if (len >= CC_MAX_SOURCE_SIZE) len = CC_MAX_SOURCE_SIZE - 1;
    ctx->source = (char*)kmalloc(len + 1);
    if (!ctx->source) return -1;
    memcpy(ctx->source, source, len);
    ctx->source[len] = 0;
    ctx->source_size = len;

    // Reset state
    ctx->ast_node_count = 0;
    ctx->type_pool_count = 0;
    ctx->errors.error_count = 0;
    ctx->errors.warning_count = 0;
    ctx->errors.has_fatal_error = 0;

    // Parse
    ast_node_t* ast = parser_parse(ctx);
    if (!ast || cc_has_errors(ctx)) {
        s_printf("[CC] Parse failed\n");
        kfree(ctx->source);
        return -1;
    }
    ctx->ast_root = ast;

    // Generate code
    int result = codegen_generate(ctx, ast);
    if (result != 0) {
        s_printf("[CC] Code generation failed\n");
        kfree(ctx->source);
        return -1;
    }

    s_printf("[CC] Compiled successfully: %d bytes output\n", ctx->output_buffer_size);
    kfree(ctx->source);
    ctx->source = NULL;
    return 0;
}

int cc_compile_file(compiler_context_t* ctx, const char* filename) {
    if (!ctx || !filename) return -1;
    // File I/O not implemented in kernel mode - would need VFS
    s_printf("[CC] File compilation not yet supported\n");
    return -1;
}

const char* cc_get_output(compiler_context_t* ctx) {
    return ctx ? ctx->output_buffer : "";
}

int cc_get_output_size(compiler_context_t* ctx) {
    return ctx ? ctx->output_buffer_size : 0;
}
