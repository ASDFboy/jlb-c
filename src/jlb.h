// jlb-c: the C interpreter for the .jlb language.
// Everything shared between the files is declared here.
//
// It follows the Rust interpreter (jlb-rust) closely: the same rules, the same
// error messages and the same output, so both pass the same test programs.

#ifndef JLB_H
#define JLB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JLB_VERSION "0.1.0"

// ---------- memory and text helpers (util.c) ----------

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/// A growable byte buffer (always kept 0-terminated).
typedef struct {
    char *p;
    size_t len, cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_put(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_vprintf(Buf *b, const char *fmt, va_list ap);
/// Takes the buffer's text (the caller frees it); the buffer is left empty.
char *buf_take(Buf *b);
void buf_utf8(Buf *b, uint32_t cp);
/// Pads with spaces to `width` characters (not bytes).
void buf_pad(Buf *b, const char *s, size_t width);

/// Decodes one character at s[*i] (moving *i past it). Bad bytes count as one character each.
uint32_t utf8_next(const char *s, size_t len, size_t *i);
size_t utf8_count(const char *s, size_t len);
/// Byte offset of the character number `k` (0-based).
size_t utf8_offset(const char *s, size_t len, size_t k);

/// An immutable, reference-counted piece of text. `nchars` counts characters.
typedef struct Str {
    int rc;
    uint32_t hash;
    size_t len;
    size_t nchars;
    char s[];
} Str;

Str *str_new(const char *p, size_t len);
Str *str_c(const char *s);
Str *str_fmt(const char *fmt, ...);
Str *str_from_buf(Buf *b);
static inline Str *str_ref(Str *s) {
    if (s) s->rc++;
    return s;
}
void str_unref(Str *s);
bool str_eq(const Str *a, const Str *b);
bool str_is(const Str *a, const char *c);
uint32_t hash_bytes(const char *p, size_t n);

// ---------- errors ----------

typedef struct Value Value;

/// A recorded variable for a crash report.
typedef struct {
    Str *name;
    Value *value; // owned (one value)
} Var;

typedef struct {
    Var *v;
    size_t n, cap;
} VarList;

typedef struct {
    char *name;
    int line;
    VarList vars;
} ErrFrame;

typedef struct ErrorInfo {
    char *msg;
    int line;
    char *file;
    char *path;
    bool captured;
    bool traced;
    ErrFrame *frames;
    size_t nframes;
    char **frame_paths;
    VarList outside;
} ErrorInfo;

/// FAULT_CANCELLED: the task was stopped with CANCEL and unwinds. FAULT_TASK_ERROR: an
/// error in a task that nothing was AWAITing; it stops the program and TRY doesn't catch it.
enum { FAULT_NONE, FAULT_ERROR, FAULT_END, FAULT_CANCELLED, FAULT_TASK_ERROR };

/// The current error. A function that fails sets this and returns false (or -1).
extern int g_fault;
extern ErrorInfo *g_err;

/// Sets an error with a message (printf-style). Always returns false.
bool fail(const char *fmt, ...);
bool fail_str(char *msg); // takes the string
/// Takes the current error (the caller owns it) and clears it.
ErrorInfo *take_error(void);
void error_free(ErrorInfo *e);
char *report(const ErrorInfo *e);

// ---------- numbers (num.c) ----------

/// A whole number of any size, 0 or more, in base 1,000,000,000 (lowest part first).
typedef struct {
    uint32_t *d;
    size_t n;
} Nat;

typedef struct Ratio {
    int rc;
    bool neg;
    Nat num, den;
    /// den is made only of 2s and 5s, so it's a plain decimal.
    bool plain;
} Ratio;

typedef enum { N_INT, N_BIG, N_FLOAT } NKind;

/// Exact whole numbers that fit in 64 bits, exact fractions and big numbers, or approximate floats.
typedef struct {
    NKind k;
    union {
        int64_t i;
        Ratio *r;
        double f;
    };
} Number;

typedef enum { ROUND_HALF, ROUND_FLOOR, ROUND_CEIL, ROUND_TRUNC } Rounding;

static inline Number num_int(int64_t i) {
    Number n;
    n.k = N_INT;
    n.i = i;
    return n;
}
Number num_retain(Number n);
void num_release(Number n);

bool num_from_f64(double x, Number *out);
bool num_from_f64_rounded(double x, size_t d, Number *out);
bool num_parse_literal(const char *s, Number *out);
/// Text that counts as a number: 25, -25, 2.5, 1e3, 0xFF. Returns false (no error) if not.
bool num_parse_text(const char *s, size_t len, Number *out);

bool num_is_float(Number n);
bool num_is_integer(Number n);
bool num_is_zero(Number n);
bool num_is_neg(Number n);
bool num_is_int(Number n, int64_t v);
int64_t num_to_i64_sat(Number n);
double num_to_f64(Number n);
int num_cmp(Number a, Number b);
static inline bool num_eq(Number a, Number b) {
    return num_cmp(a, b) == 0;
}
Number num_neg(Number n);
Number num_abs(Number n);
bool num_add(Number a, Number b, Number *out);
bool num_sub(Number a, Number b, Number *out);
bool num_mul(Number a, Number b, Number *out);
bool num_div(Number a, Number b, Number *out);
bool num_quot(Number a, Number b, Number *out);
bool num_rem(Number a, Number b, Number *out);
bool num_pow(Number a, Number b, Number *out);
bool num_root(Number a, Number b, Number *out);
bool num_round(Number n, size_t d, Rounding mode, Number *out);
size_t num_decimal_places(Number n);
/// "0xFF"; NULL when it isn't a whole number. The caller frees it.
char *num_to_hex(Number n);
char *num_to_eng(Number n); // NULL on error
bool num_pi(size_t d, Number *out);
/// The number as text (the caller frees it).
char *num_str(Number n);
void num_write(Buf *b, Number n);

// ---------- values (value.c) ----------

typedef struct Table Table;
typedef struct Function Function;
typedef struct Slot Slot;

typedef enum { V_NULL, V_NUM, V_TEXT, V_BOOL, V_TABLE, V_FUNC, V_BUILTIN, V_TASK, V_IMAGE, V_WINDOW, V_FONT } VType;

struct Value {
    VType t;
    union {
        Number num;
        Str *s;
        bool b;
        struct {
            Table *tab;
            uint64_t id;
        } tb;
        Function *fn;
        int bi; // which built-in command
        struct {
            Str *name;
            int id;
        } task; // a function running side by side (START)
        struct {
            void *p; // GfxImage or GfxFont (counted references)
            int id;  // which window
        } gfx;      // graphics.c
    };
};

/// A variable or table entry. `@` sharing makes two names point at the same slot.
struct Slot {
    int rc;
    bool constant; // made by CONST: nothing in it can change
    const char *ty; // the type it was declared with (NUMBER score), or NULL
    Value v;
};

typedef struct {
    Str *key;
    Slot *slot;
} Entry;

/// Names in the order they were added, with a hash index.
typedef struct {
    Entry *e;
    size_t n, cap;
    uint32_t *idx; // entry index + 1, 0 = empty
    size_t icap;
} OrdMap;

struct Table {
    int rc;
    Slot **items;
    size_t n, cap;
    OrdMap named;
    /// Set on error tables, so PRINT(err) shows the full report.
    Str *report;
};

/// A file's global variables.
typedef struct Env {
    int rc;
    OrdMap map;
} Env;

typedef struct Source {
    int rc;
    char *name;
    char *dir;
} Source;

typedef struct FuncDecl FuncDecl;

struct Function {
    int rc;
    FuncDecl *decl;
    Env *env;
    Source *src;
};

extern const Value NULL_VALUE;

Value v_num(Number n);
Value v_int(int64_t i);
Value v_text(Str *s); // takes the reference
Value v_textc(const char *s);
Value v_textn(const char *s, size_t n);
Value v_bool(bool b);
Value v_table(Table *t); // takes the reference, gives a new identity
Value v_retain(Value v);
void v_release(Value v);
/// `=` copies: a table keeps its data (copy-on-write) but gets a new identity.
Value v_copy(Value v); // takes v
const char *v_type(Value v);
bool v_is_func(Value v);
uint64_t new_id(void);

Slot *slot_new(Value v); // takes v
/// A constant slot with a frozen copy of v: tables inside get constant slots of their own.
Slot *slot_const(Value v); // takes v
static inline Slot *slot_ref(Slot *s) {
    s->rc++;
    return s;
}
void slot_unref(Slot *s);

void om_init(OrdMap *m);
void om_free(OrdMap *m);
Slot *om_get(const OrdMap *m, const char *k, size_t klen); // borrowed
static inline Slot *om_gets(const OrdMap *m, const Str *k) {
    return om_get(m, k->s, k->len);
}
/// Adds or replaces. Takes the slot reference; retains the key.
void om_insert(OrdMap *m, Str *key, Slot *s);
/// Removes; gives back the slot (the caller releases it) or NULL.
Slot *om_remove(OrdMap *m, const char *k, size_t klen);
void om_deep_clone(const OrdMap *src, OrdMap *dst);

Table *table_new(void);
void table_unref(Table *t);
void table_push(Table *t, Value v); // takes v
void table_push_slot(Table *t, Slot *s);
void table_insert_slot(Table *t, size_t i, Slot *s);
void table_remove(Table *t, size_t i);
/// The table in this value, made private first if others share it (copy-on-write).
Table *table_mut(Value *v);

Env *env_new(void);
void env_unref(Env *e);
Source *source_new(const char *name, const char *dir);
void source_unref(Source *s);
Function *func_new(FuncDecl *d, Env *env, Source *src);
void func_unref(Function *f);

void write_value(Buf *b, Value v, bool quote, int depth);
char *display(Value v);
char *repr(Value v);
void quote_text(Buf *b, const char *s, size_t len);
bool is_ident(const char *s, size_t len);

void varlist_push(VarList *l, Str *name, Value v);
void varlist_free(VarList *l);
void vars_of(const OrdMap *m, VarList *out);

// ---------- the syntax tree (lex.c, parse.c) ----------

typedef enum {
    OP_NONE,
    OP_EQ3N, OP_EQ3, OP_EQ2N, OP_NANDS, OP_NORS, OP_DOTS, OP_ROOT_SET, OP_QUOT_SET, OP_LE2, OP_GE2,
    OP_EQ2, OP_NE_BAD, OP_LE, OP_GE, OP_ANDS, OP_ORS, OP_NAND, OP_NOR, OP_XNOR, OP_ROOT, OP_QUOT,
    OP_POW_SET, OP_REM_SET, OP_ADD_SET, OP_SUB_SET, OP_MUL_SET, OP_DIV_SET, OP_SHARE_SET, OP_SET,
    OP_LT, OP_GT, OP_AND, OP_OR, OP_XOR, OP_POW, OP_REM, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_AT,
    OP_NOT, OP_LPAREN, OP_RPAREN, OP_LBRACK, OP_RBRACK, OP_LBRACE, OP_RBRACE, OP_COMMA, OP_DOT,
    OP_COLON, OP_SEMI,
    OP_COUNT
} Op;

extern const char *const OP_TEXT[OP_COUNT];

typedef enum { T_NUM, T_STR, T_NAME, T_KW, T_OP, T_NEWLINE, T_EOF } TokKind;

typedef struct {
    TokKind k;
    Number num;
    Str *s;       // T_STR, T_NAME
    int kw;       // T_KW: index into WORDS
    Op op;        // T_OP
    int line;
    size_t pos;   // characters from the start of the file
} Token;

typedef struct {
    Token *t;
    size_t n, cap;
} Tokens;

/// Keywords, then built-in commands.
extern const char *const WORDS[];
extern const int N_KEYWORDS;
extern const int N_WORDS;
int find_word(const char *w, size_t n);
/// The index of a word (keyword or built-in) in WORDS, by name.
int word_id(const char *w);

typedef struct {
    char *msg;
    int line;
    long pos; // -1 when unknown
} SyntaxError;

bool lex(const char *src, size_t len, Tokens *out, SyntaxError *err);
void tokens_free(Tokens *t);

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
    Stmt **s;
    int n;
} Block;

typedef enum {
    E_NUM, E_TEXT, E_BOOL, E_NULL, E_VAR, E_BUILTIN, E_TABLE, E_FIELD, E_INDEX, E_SLICE, E_CALL,
    E_NEG, E_NOT, E_BINARY, E_FUNC, E_UNDEFINE, E_INSERT, E_DROP,
    E_START // START f(x): x is the call
} ExprKind;

struct Expr {
    ExprKind k;
    Op op;
    bool b;
    int bi;          // E_BUILTIN
    Number num;
    Str *name;       // E_TEXT, E_VAR, E_FIELD
    Expr *x, *y, *z, *w;
    Expr **args;     // E_TABLE items, E_CALL / E_INSERT / E_DROP inputs
    Str **keys;      // E_TABLE: names (NULL for numbered items)
    int nargs;
    FuncDecl *fn;
};

typedef struct {
    Str *name;
    bool shared;
    bool rest;
    Expr *def;
    const char *ty; // DEF f(NUMBER n): the type the input must have (NULL is always allowed)
} Param;

struct FuncDecl {
    Str *name; // NULL for FUNC (...) { } values
    Param *params;
    int nparams;
    Block body;
};

typedef enum {
    S_EXPR, S_ASSIGN, S_LOCAL, S_CONST, S_IF, S_WHILE, S_REPEAT, S_FOR_IN, S_FOR_TO, S_BREAK, S_SKIP,
    S_RETURN, S_END, S_FUNC, S_TRY, S_GOTO, S_POINT, S_IMPORT, S_LOAD
} StmtKind;

struct Stmt {
    StmtKind k;
    int line;
    Op op;              // S_ASSIGN
    Expr *e, *e2, *e3, *e4;
    Str *name, *name2;  // loop variables, LOCAL, TRY's FAIL name, GOTO/POINT, file names
    bool shared, shared2, has2;
    bool global;        // GLOBAL before an S_ASSIGN, S_FUNC or S_CONST: the file's outside name
    const char *ty;     // NUMBER score = 0 (or TEXT, TABLE ...): before an S_EXPR name, S_ASSIGN, S_LOCAL or S_CONST
    Block body, body2;  // loops, TRY/FAIL, ELSE (body2) ...
    Expr **conds;       // S_IF: each condition
    Block *blocks;      // S_IF: each branch
    int nconds;
    bool has_else;
    FuncDecl *fn;
};

bool parse_program(Tokens *t, Block *out, SyntaxError *err);
/// Parses one expression (for the debugger-free C version, used by nothing else yet).
bool is_lvalue(const Expr *e);
/// Lexes and parses text (for files and Quick-try).
bool parse_text(const char *src, size_t len, Block *out, SyntaxError *err);

// ---------- operators (ops.c) ----------

bool truth(Value v, bool *out);
bool binop(Op op, Value a, Value b, Value *out); // borrows a and b
bool negate(Value v, Value *out);
bool compare_op(Op op, Value a, Value b, bool *out);
bool loose_eq(Value a, Value b);
bool strict_eq(Value a, Value b);
bool match_eq(Value a, Value b);
bool math_num(Value v, const char *op, Number *out);

// ---------- the interpreter (interp.c, builtins.c) ----------

typedef struct {
    OrdMap *s;
    int n, cap;
} Scopes;

typedef struct {
    Function *func; // NULL at a file's top level
    Scopes scopes;  // for functions, scopes.s[0] holds its locals
    Env *env;
    int line;
    Source *src;
} Frame;

typedef struct {
    char *path;
    Value v;
} Loaded;

typedef struct Interp {
    Frame *frames;
    int nframes, cap;
    double start;
    double last_delta;
    uint64_t rng;
    bool ide;
    bool repl;
    char **imported;
    int nimported;
    Loaded *loaded;
    int nloaded;
    // Set by RETURN and GOTO while their flow travels up.
    Value ret;
    Str *goto_name;
    int goto_line;
    // Tasks (see tasks.c): none until the first START. Task 0 is the program itself.
    struct Task *tasks;
    int ntasks, tcap;
    int current;       // which task is running
    unsigned steps;    // statements it has run this turn
    int task_fault;    // an error (or END) from a task, waiting for the program's own code
    ErrorInfo *task_err;
} Interp;

typedef enum { F_ERR = -1, F_NORMAL = 0, F_BREAK, F_SKIP, F_RETURN, F_GOTO } Flow;

void interp_init(Interp *it);
int run_main(Interp *it, const char *path);
void start_session(Interp *it, const char *dir);
/// Runs a Quick-try snippet: 1 = a value (in *out), 0 = nothing to show, -1 = error/END.
int run_snippet(Interp *it, const char *text, Value *out);
void interp_free(Interp *it);
bool set_var(Interp *it, Str *name, Value v); // takes v
bool call_builtin(Interp *it, int bi, Value *args, int n, Value *out);
double now_seconds(void);

/// Text written by the program (stdout) and whether it's sent right away.
void out_write(const char *s, size_t n);
void out_flush(void);

extern const char MARK;
void json_str(Buf *b, const char *s);
char *report_json(const ErrorInfo *e);

int run_check(const char *target);

// ---------- tasks (tasks.c) ----------

/// START: runs func with its inputs (takes the slots) as a new task.
bool task_start(Interp *it, Function *func, Slot **vals, int nvals, int line, Value *out);
/// Counts a statement; every so many, the next task gets a turn.
bool task_tick(Interp *it);
bool task_sleep(Interp *it, double secs);
bool task_await(Interp *it, Value v, Value *out);
bool task_done(Interp *it, Value v, bool *out);
bool task_cancel(Interp *it, Value v);
/// The program's own code has ended: lets its tasks finish.
bool tasks_finish(Interp *it);
/// After an error in Quick-try: stops every task still going.
void tasks_cancel_all(Interp *it);
/// Frames and calls, for tasks.c.
void frame_push(Interp *it, Function *f, Env *env, int line, Source *src);
void frame_pop(Interp *it);
bool call_prepared(Interp *it, Function *func, Slot **vals, int nvals, Value *out);
int run_repl(void);

// ---------- graphics (graphics.c, with the shared core in gfx/) ----------

/// The first graphics command in WORDS; the rest follow it.
extern const int GFX_FIRST;
bool gfx_builtin(Interp *it, int bi, Value *a, int n, Value *out);
/// DRAW_TO(name): drawing goes into the image in that variable.
bool gfx_draw_to_slot(Interp *it, Slot *s);
/// img.width and the like.
bool gfx_field(Value base, const Str *name, Value *out);
/// After the program's code: windows stay open (and their events work) until closed.
bool gfx_finish(Interp *it);
void gfx_value_retain(Value v);
void gfx_value_release(Value v);
void gfx_value_write(Buf *b, Value v);

#endif
