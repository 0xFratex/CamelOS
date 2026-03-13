/**
 * CamelOS Native C Compiler
 * A simplified but functional C compiler for x86
 * 
 * This compiler can compile C code to NASM-compatible assembly
 * that runs on CamelOS.
 */

#ifndef C_COMPILER_H
#define C_COMPILER_H

#include "../../include/types.h"

/* ============================================================================
 * Compiler Configuration
 * ============================================================================ */

#define CC_MAX_SOURCE_SIZE      65536   /* Maximum source file size */
#define CC_MAX_TOKEN_LEN        256     /* Maximum token length */
#define CC_MAX_SYMBOLS          4096    /* Maximum symbols in table */
#define CC_MAX_SCOPES           64      /* Maximum nested scopes */
#define CC_MAX_AST_NODES        16384   /* Maximum AST nodes */
#define CC_MAX_STRING_LITERALS  1024    /* Maximum string literals */
#define CC_MAX_FUNCTIONS        256     /* Maximum functions */
#define CC_MAX_PARAMS           16      /* Maximum function parameters */
#define CC_MAX_LOCALS           64      /* Maximum local variables per function */
#define CC_MAX_ERRORS           64      /* Maximum errors before stopping */
#define CC_MAX_INCLUDES         16      /* Maximum include files */
#define CC_MAX_LABELS           1024    /* Maximum labels for code gen */
#define CC_OUTPUT_BUFFER_SIZE   262144  /* Output buffer size (256KB) */

/* ============================================================================
 * Token Types
 * ============================================================================ */

typedef enum {
    /* Special tokens */
    TOK_EOF = 0,
    TOK_ERROR,
    TOK_NEWLINE,
    
    /* Literals */
    TOK_INT_LITERAL,        /* 123, 0xFF, 0777 */
    TOK_CHAR_LITERAL,       /* 'a', '\n', '\0' */
    TOK_STRING_LITERAL,     /* "hello" */
    TOK_FLOAT_LITERAL,      /* 3.14 (future support) */
    
    /* Identifiers and keywords */
    TOK_IDENT,              /* variable/function names */
    
    /* Keywords - Types */
    TOK_KW_INT,
    TOK_KW_CHAR,
    TOK_KW_VOID,
    TOK_KW_SHORT,
    TOK_KW_LONG,
    TOK_KW_UNSIGNED,
    TOK_KW_SIGNED,
    TOK_KW_FLOAT,
    TOK_KW_DOUBLE,
    TOK_KW_STRUCT,
    TOK_KW_UNION,
    TOK_KW_ENUM,
    TOK_KW_TYPEDEF,
    TOK_KW_CONST,
    TOK_KW_VOLATILE,
    TOK_KW_STATIC,
    TOK_KW_EXTERN,
    TOK_KW_REGISTER,
    TOK_KW_AUTO,
    
    /* Keywords - Control flow */
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_DO,
    TOK_KW_SWITCH,
    TOK_KW_CASE,
    TOK_KW_DEFAULT,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_RETURN,
    TOK_KW_GOTO,
    
    /* Keywords - Other */
    TOK_KW_SIZEOF,
    TOK_KW_INCLUDE,         /* #include (preprocessor) */
    TOK_KW_DEFINE,          /* #define (preprocessor) */
    
    /* Operators - Arithmetic */
    TOK_PLUS,               /* + */
    TOK_MINUS,              /* - */
    TOK_STAR,               /* * */
    TOK_SLASH,              /* / */
    TOK_PERCENT,            /* % */
    
    /* Operators - Increment/Decrement */
    TOK_PLUSPLUS,           /* ++ */
    TOK_MINUSMINUS,         /* -- */
    
    /* Operators - Comparison */
    TOK_EQ,                 /* == */
    TOK_NE,                 /* != */
    TOK_LT,                 /* < */
    TOK_LE,                 /* <= */
    TOK_GT,                 /* > */
    TOK_GE,                 /* >= */
    
    /* Operators - Logical */
    TOK_AND,                /* && */
    TOK_OR,                 /* || */
    TOK_NOT,                /* ! */
    
    /* Operators - Bitwise */
    TOK_AMPERSAND,          /* & */
    TOK_PIPE,               /* | */
    TOK_CARET,              /* ^ */
    TOK_TILDE,              /* ~ */
    TOK_LSHIFT,             /* << */
    TOK_RSHIFT,             /* >> */
    
    /* Operators - Assignment */
    TOK_ASSIGN,             /* = */
    TOK_PLUS_ASSIGN,        /* += */
    TOK_MINUS_ASSIGN,       /* -= */
    TOK_STAR_ASSIGN,        /* *= */
    TOK_SLASH_ASSIGN,       /* /= */
    TOK_PERCENT_ASSIGN,     /* %= */
    TOK_AMP_ASSIGN,         /* &= */
    TOK_PIPE_ASSIGN,        /* |= */
    TOK_CARET_ASSIGN,       /* ^= */
    TOK_LSHIFT_ASSIGN,      /* <<= */
    TOK_RSHIFT_ASSIGN,      /* >>= */
    
    /* Operators - Other */
    TOK_QUESTION,           /* ? */
    TOK_COLON,              /* : */
    
    /* Delimiters */
    TOK_LPAREN,             /* ( */
    TOK_RPAREN,             /* ) */
    TOK_LBRACE,             /* { */
    TOK_RBRACE,             /* } */
    TOK_LBRACKET,           /* [ */
    TOK_RBRACKET,           /* ] */
    TOK_SEMICOLON,          /* ; */
    TOK_COMMA,              /* , */
    TOK_DOT,                /* . */
    TOK_ARROW,              /* -> */
    TOK_ELLIPSIS,           /* ... */
    
    /* Preprocessor */
    TOK_HASH,               /* # */
    TOK_PP_INCLUDE,
    TOK_PP_DEFINE,
    TOK_PP_IFDEF,
    TOK_PP_IFNDEF,
    TOK_PP_ENDIF,
    
    TOKEN_TYPE_COUNT
} token_type_t;

/* ============================================================================
 * Token Structure
 * ============================================================================ */

typedef struct {
    token_type_t type;
    char value[CC_MAX_TOKEN_LEN];
    int int_value;              /* For integer literals */
    int line;                   /* Source line number */
    int column;                 /* Source column number */
} token_t;

/* ============================================================================
 * Type System
 * ============================================================================ */

typedef enum {
    TYPE_VOID = 0,
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_INT,
    TYPE_LONG,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_POINTER,
    TYPE_ARRAY,
    TYPE_FUNCTION,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ENUM,
    TYPE_TYPEDEF,
} type_kind_t;

typedef struct type_info type_info_t;

struct type_info {
    type_kind_t kind;
    int size;                   /* Size in bytes */
    int is_unsigned;            /* Unsigned flag */
    int is_const;               /* Const flag */
    int is_volatile;            /* Volatile flag */
    int is_static;              /* Static flag */
    int is_extern;              /* Extern flag */
    
    /* For pointers: the type being pointed to */
    type_info_t* base_type;
    
    /* For arrays: element count (-1 for unknown) */
    int array_size;
    
    /* For structs/unions: name */
    char type_name[64];
    
    /* For functions */
    struct {
        type_info_t* return_type;
        type_info_t* param_types[CC_MAX_PARAMS];
        char param_names[CC_MAX_PARAMS][32];
        int param_count;
        int is_variadic;
    } func;
};

/* ============================================================================
 * Symbol Table
 * ============================================================================ */

typedef enum {
    SYM_VARIABLE = 0,
    SYM_FUNCTION,
    SYM_TYPEDEF,
    SYM_STRUCT,
    SYM_ENUM,
    SYM_ENUM_VALUE,
    SYM_LABEL,
} symbol_kind_t;

typedef enum {
    SCOPE_GLOBAL = 0,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_STRUCT,
} scope_kind_t;

typedef struct symbol symbol_t;

struct symbol {
    char name[64];
    symbol_kind_t kind;
    type_info_t* type;
    int scope_level;
    int is_defined;             /* Has definition (not just declaration) */
    int is_param;               /* Is function parameter */
    
    /* For variables: storage location */
    int stack_offset;           /* Stack offset for locals */
    int is_global;              /* Is global variable */
    int global_offset;          /* Offset in data section */
    
    /* For functions */
    int func_body_generated;    /* Function body was generated */
    int param_count;
    
    /* For labels */
    int label_id;               /* Unique label ID */
    int is_defined_label;       /* Label has been defined */
    
    /* For enum values */
    int enum_value;
};

typedef struct {
    symbol_t symbols[CC_MAX_SYMBOLS];
    int symbol_count;
    int current_scope;
    scope_kind_t scope_kinds[CC_MAX_SCOPES];
    int scope_offsets[CC_MAX_SCOPES];  /* Stack offset at each scope */
    int current_stack_offset;
    int label_counter;          /* For generating unique labels */
} symbol_table_t;

/* ============================================================================
 * AST Node Types
 * ============================================================================ */

typedef enum {
    /* Program structure */
    AST_PROGRAM = 0,
    AST_FUNCTION_DECL,
    AST_FUNCTION_DEF,
    AST_VAR_DECL,
    AST_VAR_DEF,
    AST_PARAM_DECL,
    AST_STRUCT_DECL,
    AST_ENUM_DECL,
    AST_TYPEDEF_DECL,
    
    /* Statements */
    AST_COMPOUND_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_DO_WHILE_STMT,
    AST_FOR_STMT,
    AST_SWITCH_STMT,
    AST_CASE_STMT,
    AST_DEFAULT_STMT,
    AST_RETURN_STMT,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_GOTO_STMT,
    AST_LABEL_STMT,
    AST_EXPR_STMT,
    AST_NULL_STMT,
    
    /* Expressions */
    AST_CONST_INT,
    AST_CONST_CHAR,
    AST_CONST_STRING,
    AST_IDENT,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_ASSIGNMENT,
    AST_FUNC_CALL,
    AST_ARRAY_ACCESS,
    AST_MEMBER_ACCESS,          /* . operator */
    AST_PTR_MEMBER_ACCESS,      /* -> operator */
    AST_CONDITIONAL,            /* ?: operator */
    AST_CAST,
    AST_SIZEOF,
    AST_ADDRESS_OF,             /* & operator */
    AST_DEREFERENCE,            /* * operator (dereference) */
    AST_PRE_INCREMENT,          /* ++x */
    AST_PRE_DECREMENT,          /* --x */
    AST_POST_INCREMENT,         /* x++ */
    AST_POST_DECREMENT,         /* x-- */
    AST_INIT_LIST,              /* Array/struct initializer */
    AST_COMPOUND_LITERAL,       /* (type_name){...} */
    
    /* Other */
    AST_EMPTY,
    
    AST_NODE_TYPE_COUNT
} ast_node_type_t;

/* ============================================================================
 * AST Node Structure
 * ============================================================================ */

typedef struct ast_node ast_node_t;

struct ast_node {
    ast_node_type_t type;
    int line;
    int column;
    
    union {
        /* Program */
        struct {
            ast_node_t** declarations;
            int decl_count;
        } program;
        
        /* Function declaration/definition */
        struct {
            char name[64];
            type_info_t* return_type;
            ast_node_t** params;
            int param_count;
            ast_node_t* body;           /* NULL for declaration */
            int is_variadic;
            symbol_t* symbol;
        } func;
        
        /* Variable declaration/definition */
        struct {
            char name[64];
            type_info_t* var_type;
            ast_node_t* initializer;
            int array_size;             /* For array declarations */
            symbol_t* symbol;
        } var;
        
        /* Parameter declaration */
        struct {
            char name[64];
            type_info_t* param_type;
            int is_variadic;
        } param;
        
        /* Struct/Enum declaration */
        struct {
            char name[64];
            ast_node_t** members;
            int member_count;
        } struct_decl;
        
        /* Compound statement */
        struct {
            ast_node_t** statements;
            int stmt_count;
        } compound;
        
        /* If statement */
        struct {
            ast_node_t* condition;
            ast_node_t* then_branch;
            ast_node_t* else_branch;
        } if_stmt;
        
        /* While statement */
        struct {
            ast_node_t* condition;
            ast_node_t* body;
        } while_stmt;
        
        /* Do-while statement */
        struct {
            ast_node_t* body;
            ast_node_t* condition;
        } do_while;
        
        /* For statement */
        struct {
            ast_node_t* init;
            ast_node_t* condition;
            ast_node_t* update;
            ast_node_t* body;
        } for_stmt;
        
        /* Switch statement */
        struct {
            ast_node_t* expr;
            ast_node_t* body;
            symbol_t* break_label;
        } switch_stmt;
        
        /* Case statement */
        struct {
            ast_node_t* value;
            ast_node_t* body;
        } case_stmt;
        
        /* Return statement */
        struct {
            ast_node_t* value;
        } return_stmt;
        
        /* Goto statement */
        struct {
            char label_name[64];
            symbol_t* label;
        } goto_stmt;
        
        /* Label statement */
        struct {
            char name[64];
            ast_node_t* statement;
        } label_stmt;
        
        /* Expression statement */
        struct {
            ast_node_t* expr;
        } expr_stmt;
        
        /* Integer constant */
        struct {
            int value;
            type_info_t* type;
        } const_int;
        
        /* Character constant */
        struct {
            int value;
        } const_char;
        
        /* String constant */
        struct {
            char* value;
            int string_id;             /* Index into string table */
        } const_string;
        
        /* Identifier */
        struct {
            char name[64];
            symbol_t* symbol;
            type_info_t* inferred_type;
        } ident;
        
        /* Binary operation */
        struct {
            ast_node_t* left;
            ast_node_t* right;
            token_type_t op;
            type_info_t* result_type;
        } binary;
        
        /* Unary operation */
        struct {
            ast_node_t* operand;
            token_type_t op;
            type_info_t* result_type;
        } unary;
        
        /* Assignment */
        struct {
            ast_node_t* lvalue;
            ast_node_t* rvalue;
            token_type_t op;           /* =, +=, etc. */
        } assign;
        
        /* Function call */
        struct {
            char func_name[64];
            ast_node_t** args;
            int arg_count;
            symbol_t* func_symbol;
            type_info_t* return_type;
        } call;
        
        /* Array access */
        struct {
            ast_node_t* array;
            ast_node_t* index;
            type_info_t* element_type;
        } array_access;
        
        /* Member access (. or ->) */
        struct {
            ast_node_t* struct_expr;
            char member_name[64];
            int is_pointer;            /* True for -> */
            int member_offset;
            type_info_t* member_type;
        } member;
        
        /* Conditional expression (?:) */
        struct {
            ast_node_t* condition;
            ast_node_t* then_expr;
            ast_node_t* else_expr;
        } conditional;
        
        /* Cast */
        struct {
            ast_node_t* expr;
            type_info_t* target_type;
        } cast;
        
        /* Sizeof */
        struct {
            ast_node_t* expr;          /* Either expression or type name */
            type_info_t* type;
            int is_type;               /* sizeof(type) vs sizeof(expr) */
            int size;
        } sizeof_expr;
        
        /* Initializer list */
        struct {
            ast_node_t** elements;
            int elem_count;
        } init_list;
    } data;
};

/* ============================================================================
 * String Literal Table
 * ============================================================================ */

typedef struct {
    char* strings[CC_MAX_STRING_LITERALS];
    int lengths[CC_MAX_STRING_LITERALS];
    int count;
    char labels[CC_MAX_STRING_LITERALS][32];  /* Generated labels */
} string_table_t;

/* ============================================================================
 * Error Handling
 * ============================================================================ */

typedef enum {
    ERR_LEX = 0,
    ERR_PARSE,
    ERR_SEMANTIC,
    ERR_CODEGEN,
    ERR_INTERNAL
} error_category_t;

typedef struct {
    error_category_t category;
    char message[256];
    int line;
    int column;
    int is_warning;
} compiler_error_t;

typedef struct {
    compiler_error_t errors[CC_MAX_ERRORS];
    int error_count;
    int warning_count;
    int has_fatal_error;
} error_list_t;

/* ============================================================================
 * Code Generator State
 * ============================================================================ */

typedef struct {
    char* output;                   /* Output buffer */
    int output_size;
    int output_capacity;
    int indent_level;
    
    /* Data section */
    char* data_section;
    int data_size;
    int data_capacity;
    
    /* String labels */
    string_table_t strings;
    
    /* Current function context */
    char current_function[64];
    int local_stack_size;
    int max_stack_size;
    
    /* Label management */
    int label_counter;
    int break_label_stack[32];
    int continue_label_stack[32];
    int break_label_top;
    int continue_label_top;
    
    /* Global variable offsets */
    int global_data_offset;
    
} codegen_state_t;

/* ============================================================================
 * Preprocessor State
 * ============================================================================ */

typedef struct {
    char defines[64][64];           /* Macro names */
    char values[64][256];           /* Macro values */
    int define_count;
    
    char include_paths[CC_MAX_INCLUDES][128];
    int include_path_count;
    
    int skip_depth;                 /* For #ifdef/#ifndef/#endif */
} preprocessor_t;

/* ============================================================================
 * Main Compiler Context
 * ============================================================================ */

typedef struct {
    /* Source code */
    char* source;
    int source_size;
    char source_file[128];
    
    /* Lexer state */
    int pos;
    int line;
    int column;
    token_t current_token;
    token_t peek_token;
    int has_peek_token;
    
    /* Symbol table */
    symbol_table_t symbols;
    
    /* AST */
    ast_node_t* ast_root;
    ast_node_t ast_nodes[CC_MAX_AST_NODES];
    int ast_node_count;
    
    /* Type pool (for reusing type_info_t structures) */
    type_info_t type_pool[512];
    int type_pool_count;
    
    /* Errors */
    error_list_t errors;
    
    /* Code generator */
    codegen_state_t codegen;
    
    /* Preprocessor */
    preprocessor_t preprocessor;
    
    /* Compiler options */
    int optimize_level;             /* 0-3 */
    int generate_debug_info;
    int output_assembly;            /* Output NASM assembly */
    int output_binary;              /* Output raw binary */
    int verbose;
    
    /* Output */
    char* output_buffer;
    int output_buffer_size;
    
} compiler_context_t;

/* ============================================================================
 * Compiler API Functions
 * ============================================================================ */

/* Initialization and cleanup */
void cc_init(compiler_context_t* ctx);
void cc_cleanup(compiler_context_t* ctx);

/* Compilation entry points */
int cc_compile_string(compiler_context_t* ctx, const char* source);
int cc_compile_file(compiler_context_t* ctx, const char* filename);

/* Get output */
const char* cc_get_output(compiler_context_t* ctx);
int cc_get_output_size(compiler_context_t* ctx);

/* Error handling */
void cc_error(compiler_context_t* ctx, error_category_t cat, const char* msg, ...);
void cc_warning(compiler_context_t* ctx, const char* msg, ...);
int cc_has_errors(compiler_context_t* ctx);
const char* cc_get_errors(compiler_context_t* ctx);

/* ============================================================================
 * Lexer API
 * ============================================================================ */

void lexer_init(compiler_context_t* ctx);
token_t lexer_next_token(compiler_context_t* ctx);
token_t lexer_peek_token(compiler_context_t* ctx);
void lexer_skip_whitespace(compiler_context_t* ctx);
void lexer_skip_line(compiler_context_t* ctx);
int lexer_is_keyword(const char* str, token_type_t* type);

/* ============================================================================
 * Parser API
 * ============================================================================ */

void parser_init(compiler_context_t* ctx);
ast_node_t* parser_parse(compiler_context_t* ctx);

/* Recursive descent parsing functions */
ast_node_t* parse_translation_unit(compiler_context_t* ctx);
ast_node_t* parse_external_declaration(compiler_context_t* ctx);
ast_node_t* parse_function_definition(compiler_context_t* ctx);
ast_node_t* parse_declaration(compiler_context_t* ctx);
ast_node_t* parse_statement(compiler_context_t* ctx);
ast_node_t* parse_compound_statement(compiler_context_t* ctx);
ast_node_t* parse_expression_statement(compiler_context_t* ctx);
ast_node_t* parse_selection_statement(compiler_context_t* ctx);
ast_node_t* parse_iteration_statement(compiler_context_t* ctx);
ast_node_t* parse_jump_statement(compiler_context_t* ctx);
ast_node_t* parse_expression(compiler_context_t* ctx);
ast_node_t* parse_assignment_expression(compiler_context_t* ctx);
ast_node_t* parse_conditional_expression(compiler_context_t* ctx);
ast_node_t* parse_logical_or_expression(compiler_context_t* ctx);
ast_node_t* parse_logical_and_expression(compiler_context_t* ctx);
ast_node_t* parse_inclusive_or_expression(compiler_context_t* ctx);
ast_node_t* parse_exclusive_or_expression(compiler_context_t* ctx);
ast_node_t* parse_and_expression(compiler_context_t* ctx);
ast_node_t* parse_equality_expression(compiler_context_t* ctx);
ast_node_t* parse_relational_expression(compiler_context_t* ctx);
ast_node_t* parse_shift_expression(compiler_context_t* ctx);
ast_node_t* parse_additive_expression(compiler_context_t* ctx);
ast_node_t* parse_multiplicative_expression(compiler_context_t* ctx);
ast_node_t* parse_unary_expression(compiler_context_t* ctx);
ast_node_t* parse_postfix_expression(compiler_context_t* ctx);
ast_node_t* parse_primary_expression(compiler_context_t* ctx);
type_info_t* parse_type_name(compiler_context_t* ctx);

/* ============================================================================
 * Symbol Table API
 * ============================================================================ */

void symtab_init(symbol_table_t* table);
void symtab_push_scope(symbol_table_t* table, scope_kind_t kind);
void symtab_pop_scope(symbol_table_t* table);
symbol_t* symtab_lookup(symbol_table_t* table, const char* name);
symbol_t* symtab_lookup_current(symbol_table_t* table, const char* name);
symbol_t* symtab_insert(symbol_table_t* table, const char* name, symbol_kind_t kind);
int symtab_allocate_stack(symbol_table_t* table, int size);

/* ============================================================================
 * Type System API
 * ============================================================================ */

type_info_t* type_create(compiler_context_t* ctx, type_kind_t kind);
type_info_t* type_copy(compiler_context_t* ctx, type_info_t* src);
type_info_t* type_pointer_to(compiler_context_t* ctx, type_info_t* base);
type_info_t* type_array_of(compiler_context_t* ctx, type_info_t* base, int size);
int type_size(type_info_t* type);
int type_is_integer(type_info_t* type);
int type_is_pointer(type_info_t* type);
int type_is_array(type_info_t* type);
int type_is_scalar(type_info_t* type);
int type_compatible(type_info_t* t1, type_info_t* t2);
int type_assignable(type_info_t* dest, type_info_t* src);
const char* type_to_string(type_info_t* type, char* buf, int size);

/* ============================================================================
 * Code Generator API
 * ============================================================================ */

void codegen_init(codegen_state_t* cg);
void codegen_cleanup(codegen_state_t* cg);
int codegen_generate(compiler_context_t* ctx, ast_node_t* ast);

/* Code emission */
void emit(codegen_state_t* cg, const char* fmt, ...);
void emit_data(codegen_state_t* cg, const char* fmt, ...);
void emit_label(codegen_state_t* cg, const char* label);
void emit_indent(codegen_state_t* cg);

/* Expression code generation */
int codegen_expr(compiler_context_t* ctx, ast_node_t* node, int dest_reg);
void codegen_lvalue(compiler_context_t* ctx, ast_node_t* node);

/* Statement code generation */
void codegen_statement(compiler_context_t* ctx, ast_node_t* node);
void codegen_compound(compiler_context_t* ctx, ast_node_t* node);
void codegen_if(compiler_context_t* ctx, ast_node_t* node);
void codegen_while(compiler_context_t* ctx, ast_node_t* node);
void codegen_for(compiler_context_t* ctx, ast_node_t* node);
void codegen_return(compiler_context_t* ctx, ast_node_t* node);

/* Declaration code generation */
void codegen_function(compiler_context_t* ctx, ast_node_t* node);
void codegen_global_var(compiler_context_t* ctx, ast_node_t* node);

/* Label management */
int codegen_new_label(codegen_state_t* cg);
void codegen_push_break_label(codegen_state_t* cg, int label);
void codegen_pop_break_label(codegen_state_t* cg);
int codegen_get_break_label(codegen_state_t* cg);
void codegen_push_continue_label(codegen_state_t* cg, int label);
void codegen_pop_continue_label(codegen_state_t* cg);
int codegen_get_continue_label(codegen_state_t* cg);

/* String management */
int codegen_add_string(codegen_state_t* cg, const char* str);
const char* codegen_get_string_label(codegen_state_t* cg, int id);

/* ============================================================================
 * Standard Library Declarations
 * ============================================================================ */

/* Register built-in functions and types */
void cc_register_builtins(compiler_context_t* ctx);

/* Built-in function declarations */
typedef struct {
    const char* name;
    const char* return_type;
    const char* params;     /* Format: "int,char*,..." or "" for void */
    int is_variadic;
} builtin_func_t;

/* List of supported built-in functions */
static const builtin_func_t cc_builtins[] = {
    /* I/O functions */
    {"printf",      "int",   "const char*,...", 1},
    {"putchar",     "int",   "int", 0},
    {"puts",        "int",   "const char*", 0},
    {"getchar",     "int",   "void", 0},
    
    /* Memory functions */
    {"malloc",      "void*", "unsigned int", 0},
    {"free",        "void",  "void*", 0},
    {"realloc",     "void*", "void*,unsigned int", 0},
    {"calloc",      "void*", "unsigned int,unsigned int", 0},
    
    /* String functions */
    {"strlen",      "unsigned int", "const char*", 0},
    {"strcpy",      "char*", "char*,const char*", 0},
    {"strcat",      "char*", "char*,const char*", 0},
    {"strcmp",      "int",   "const char*,const char*", 0},
    {"strncpy",     "char*", "char*,const char*,unsigned int", 0},
    {"strncmp",     "int",   "const char*,const char*,unsigned int", 0},
    {"strchr",      "char*", "const char*,int", 0},
    {"strrchr",     "char*", "const char*,int", 0},
    {"memcpy",      "void*", "void*,const void*,unsigned int", 0},
    {"memset",      "void*", "void*,int,unsigned int", 0},
    {"memmove",     "void*", "void*,const void*,unsigned int", 0},
    
    /* Memory functions */
    {"memcmp",      "int",   "const void*,const void*,unsigned int", 0},
    
    /* Conversion functions */
    {"atoi",        "int",   "const char*", 0},
    {"itoa",        "char*", "int,char*", 0},
    
    /* Character functions */
    {"isalpha",     "int",   "int", 0},
    {"isdigit",     "int",   "int", 0},
    {"isalnum",     "int",   "int", 0},
    {"isspace",     "int",   "int", 0},
    {"toupper",     "int",   "int", 0},
    {"tolower",     "int",   "int", 0},
    
    /* CamelOS-specific functions */
    {"print",       "void",  "const char*", 0},
    {"exit",        "void",  "int", 0},
    
    {NULL, NULL, NULL, 0}  /* Sentinel */
};

/* ============================================================================
 * Utility Macros
 * ============================================================================ */

#define CC_DEBUG(ctx, msg, ...) do { \
    if ((ctx)->verbose) { \
        char _buf[256]; \
        sprintf(_buf, "[DEBUG] " msg "\n", ##__VA_ARGS__); \
        /* Could output to debug console here */ \
    } \
} while(0)

#define CC_ASSERT(ctx, cond, msg) do { \
    if (!(cond)) { \
        cc_error(ctx, ERR_INTERNAL, "Assertion failed: %s at %s:%d", msg, __FILE__, __LINE__); \
    } \
} while(0)

/* Register allocation for x86 */
#define REG_EAX 0
#define REG_ECX 1
#define REG_EDX 2
#define REG_EBX 3
#define REG_ESP 4
#define REG_EBP 5
#define REG_ESI 6
#define REG_EDI 7

/* Size information for types */
#define SIZE_CHAR   1
#define SIZE_SHORT  2
#define SIZE_INT    4
#define SIZE_LONG   4
#define SIZE_PTR    4
#define SIZE_FLOAT  4
#define SIZE_DOUBLE 8

/* ============================================================================
 * CDL Integration (for CamelOS)
 * ============================================================================ */

#ifdef CDL_INTEGRATION
#include "../../sys/cdl_defs.h"

/* CDL entry point for using compiler as a library */
cdl_exports_t* c_compiler_cdl_entry(kernel_api_t* api);

/* High-level compilation function using kernel API */
int cc_compile_to_file(compiler_context_t* ctx, const char* source, const char* output_path);
int cc_compile_and_run(compiler_context_t* ctx, const char* source);
#endif

#endif /* C_COMPILER_H */
