#include "lume.h"

/* Static type checker for Lume. Runs once after parsing, before execution.
 *
 * Design:
 *   - int / float / string / bool / null / Result are primitives (TY_*).
 *   - `T[]` is a list type; `type Name = { f: T, ... }` declares structs.
 *   - Unannotated parameters/returns/handlers are TY_ANY ("loose") and are
 *     never constrained; this is what lets route/tool callbacks accept the
 *     framework's request objects.
 *   - All types are nullable: null is assignable everywhere.
 *   - `{ ok: ... }` / `{ err: ... }` single-key maps are Result literals.
 *   - `call()?` needs the callee to be Result-typed and the enclosing
 *     function to return Result (or be untyped), and never at top level.
 *   - int -> float widens implicitly; nothing else does.
 *   - `if`/`while`/`and`/`or`/`not` require bool (strict, no JS truthiness).
 *
 * It stops at the first error (errbuf holds a readable message, like the
 * lexer/parser do).
 */

/* ===================== type representation ===================== */

static Type ANY_TYPE = {.kind = TY_ANY};

static Type *any_type(void) { return &ANY_TYPE; }

Type *type_prim(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    return t;
}

Type *type_list(Type *elem) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_LIST;
    t->elem = elem;
    return t;
}

Type *type_struct(const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = strdup(name);
    return t;
}

Type *type_anon_struct(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = NULL;
    return t;
}

Type *type_func(int arity, Type **params, Type *ret) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_FUNC;
    t->types = params;
    t->count = arity;
    t->ret = ret;
    return t;
}

Type *type_result(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_RESULT;
    return t;
}

void type_add_member(Type *t, const char *name, Type *ty) {
    if (!t || t->kind != TY_STRUCT) return;
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4;
        t->names = realloc(t->names, sizeof(char *) * (size_t)t->cap);
        t->types = realloc(t->types, sizeof(Type *) * (size_t)t->cap);
    }
    t->names[t->count] = strdup(name);
    t->types[t->count] = ty;
    t->count++;
}

static void tp_inner(Type *t) {
    switch (t->kind) {
        case TY_NULL:   printf("null"); break;
        case TY_INT:    printf("int"); break;
        case TY_FLOAT:  printf("float"); break;
        case TY_STRING: printf("string"); break;
        case TY_BOOL:   printf("bool"); break;
        case TY_ANY:    printf("any"); break;
        case TY_RESULT: printf("Result"); break;
        case TY_LIST:
            tp_inner(t->elem);
            printf("[]");
            break;
        case TY_FUNC:
            printf("func(");
            for (int i = 0; i < t->count; i++) {
                if (i) printf(", ");
                tp_inner(t->types[i]);
            }
            printf(")");
            if (t->ret) { printf(": "); tp_inner(t->ret); }
            break;
        case TY_STRUCT:
            if (t->name) {
                printf("%s", t->name);
            } else {
                printf("{ ");
                for (int i = 0; i < t->count; i++) {
                    if (i) printf(", ");
                    printf("%s: ", t->names[i]);
                    tp_inner(t->types[i]);
                }
                printf(" }");
            }
            break;
    }
}

void type_print(Type *t) {
    if (!t) { printf("?"); return; }
    tp_inner(t);
}

/* Format a type into a static buffer (for error messages). */
static const char *ty_str(Type *t) {
    static char buf[128];
    buf[0] = '\0';
    if (!t || t->kind == TY_ANY) return "any";
    if (t->kind == TY_NULL) return "null";
    if (t->kind == TY_INT) return "int";
    if (t->kind == TY_FLOAT) return "float";
    if (t->kind == TY_STRING) return "string";
    if (t->kind == TY_BOOL) return "bool";
    if (t->kind == TY_RESULT) return "Result";
    snprintf(buf, sizeof(buf), "<%s>", t->name ? t->name : "?");
    return buf;
}

/* ===================== checker state ===================== */

typedef struct CSym {
    char *name;
    Type *type;          /* can be NULL while a struct def is being filled */
    struct CSym *next;
} CSym;

typedef struct CScope {
    struct CScope *parent;
    CSym *syms;
} CScope;

typedef struct StructDef {
    char *name;
    Type *type;
    struct StructDef *next;
} StructDef;

typedef struct {
    CScope *scope;
    StructDef *structs;
    Type *cur_ret;       /* enclosing function return type; NULL outside func */
    bool in_func;        /* inside a function body */
    Type *param_hint;    /* one-shot: force a literal's first unannotated param */
    char *errbuf;
    size_t errbuf_size;
    bool failed;
} Checker;

static void ck_fail(Checker *c, size_t line, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = true;
    if (!c->errbuf || !c->errbuf_size) return;
    snprintf(c->errbuf, c->errbuf_size, "line %zu: ", line);
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(c->errbuf);
    vsnprintf(c->errbuf + used, c->errbuf_size - used, fmt, ap);
    va_end(ap);
}

static CScope *scope_new(CScope *parent) {
    CScope *s = calloc(1, sizeof(CScope));
    s->parent = parent;
    return s;
}

static void scope_put(CScope *s, const char *name, Type *t) {
    for (CSym *it = s->syms; it; it = it->next) {
        if (strcmp(it->name, name) == 0) { it->type = t; return; }
    }
    CSym *sym = calloc(1, sizeof(CSym));
    sym->name = strdup(name);
    sym->type = t;
    sym->next = s->syms;
    s->syms = sym;
}

static Type *scope_get(CScope *s, const char *name) {
    for (CScope *sc = s; sc; sc = sc->parent) {
        for (CSym *it = sc->syms; it; it = it->next)
            if (strcmp(it->name, name) == 0) return it->type;
    }
    return NULL;
}

static StructDef *find_struct(Checker *c, const char *name) {
    for (StructDef *d = c->structs; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

static void add_struct(Checker *c, const char *name, Type *t) {
    StructDef *d = calloc(1, sizeof(StructDef));
    d->name = strdup(name);
    d->type = t;
    d->next = c->structs;
    c->structs = d;
}

/* Resolve a named struct reference to its definition (or TY_ANY). */
static Type *resolve(Checker *c, Type *t, size_t line) {
    if (!t) return any_type();
    if (t->kind == TY_STRUCT && t->name) {
        StructDef *d = find_struct(c, t->name);
        if (!d) {
            ck_fail(c, line, "unknown type '%s'", t->name);
            return any_type();
        }
        return d->type;
    }
    return t;
}

/* Is a value of type `src` assignable to a slot of type `dst`? */
static bool type_compat(Checker *c, Type *src, Type *dst, size_t line) {
    if (!src || src->kind == TY_ANY) return true;
    if (!dst || dst->kind == TY_ANY) return true;
    if (src->kind == TY_NULL) return true;          /* all types nullable */
    if (src->kind == TY_INT && dst->kind == TY_FLOAT) return true; /* widen */
    if (src->kind != dst->kind) return false;

    switch (src->kind) {
        case TY_LIST:
            return type_compat(c, src->elem, dst->elem, line);
        case TY_STRUCT: {
            if (src->name && dst->name)
                return strcmp(src->name, dst->name) == 0;
            /* need field shape comparison */
            if (src->count != dst->count) return false;
            for (int i = 0; i < src->count; i++) {
                int j = -1;
                for (int k = 0; k < dst->count; k++)
                    if (strcmp(src->names[i], dst->names[k]) == 0) { j = k; break; }
                if (j < 0 || !type_compat(c, src->types[i], dst->types[j], line))
                    return false;
            }
            return true;
        }
        case TY_FUNC: {
            if (src->count != dst->count) return false;
            for (int i = 0; i < src->count; i++)
                if (!type_compat(c, src->types[i], dst->types[i], line))
                    return false;
            return type_compat(c, src->ret, dst->ret, line);
        }
        default:
            return true; /* equal primitive kinds */
    }
}

/* Does an expression evaluate in a boolean context? */
static bool is_bool_ok(Checker *c, Type *t, size_t line) {
    if (!t || t->kind == TY_ANY) return true;
    if (t->kind == TY_BOOL) return true;
    ck_fail(c, line, "expected bool, got %s", ty_str(t));
    return false;
}

/* Wire `expected` into the returned type: a value of type `actual` must be
 * assignable to an `expected` slot. Same rule as N_LET/N_ASSIGN/N_RETURN,
 * so call args, list elements and struct fields get static guarantees too
 * instead of a generic runtime error. ANY on either side stays loose. */
static void expect_compat(Checker *c, Type *actual, Type *expected,
                          size_t line, const char *what) {
    if (c->failed) return;
    if (!expected || expected->kind == TY_ANY) return;
    if (!actual || actual->kind == TY_ANY) return;
    if (!type_compat(c, actual, expected, line))
        ck_fail(c, line,
                "%s: cannot use a value of type %s where %s is expected",
                what, ty_str(actual), ty_str(expected));
}

/* Static statement in a type; combines ARITH with the widening rule so
 * `1 + 2` is int but `1 + 2.5` is float. */
static Type *arith_result(Type *a, Type *b) {
    if (!a || a->kind == TY_ANY || !b || b->kind == TY_ANY) return any_type();
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) {
        Type *f = type_prim(TY_FLOAT);
        return f;
    }
    Type *i = type_prim(TY_INT);
    return i;
}

/* ===================== expression checking ===================== */

static void ck_stmt(Checker *c, Node *n);
static void ck_fn(Checker *c, char **names, Type *ft, Node *body);

static Type *ck_expr(Checker *c, Node *n, Type *expected);

static Type *ck_list(Checker *c, Node *n, Type *expected) {
    if (n->as.list.count == 0) {
        /* `[]` — type comes from the expected annotation, else any[] */
        if (expected && expected->kind == TY_LIST) return expected;
        return type_list(any_type());
    }
    /* feed the first element the expected element type, then unify others */
    Type *elem = expected && expected->kind == TY_LIST ? expected->elem : NULL;
    Type *t0 = ck_expr(c, n->as.list.items[0], elem);
    expect_compat(c, t0, elem, n->as.list.items[0]->line, "list element");
    for (int i = 1; i < n->as.list.count; i++) {
        Type *ti = ck_expr(c, n->as.list.items[i], elem);
        expect_compat(c, ti, elem, n->as.list.items[i]->line, "list element");
        if (!type_compat(c, ti, t0, n->as.list.items[i]->line) &&
            !type_compat(c, t0, ti, n->as.list.items[i]->line)) {
            ck_fail(c, n->line, "list literal has incompatible element types");
            return type_list(any_type());
        }
    }
    if (elem) return expected;   /* annotated list: honor the expected type */
    return type_list(t0);
}

static Type *ck_expr(Checker *c, Node *n, Type *expected) {
    if (c->failed) return any_type();
    if (!n) return any_type();

    switch (n->type) {
        case N_LITERAL:
            switch (n->as.lit.kind) {
                case LIT_NUM:
                    return n->as.lit.is_float ? type_prim(TY_FLOAT)
                                              : type_prim(TY_INT);
                case LIT_STR:  return type_prim(TY_STRING);
                case LIT_TRUE:
                case LIT_FALSE: return type_prim(TY_BOOL);
                case LIT_NULL:  return type_prim(TY_NULL);
            }
            return any_type();

        case N_VAR: {
            Type *t = scope_get(c->scope, n->as.var.name);
            if (!t) {
                ck_fail(c, n->line, "undefined variable '%s'", n->as.var.name);
                return any_type();
            }
            return t;
        }

        case N_MAP_LIT: {
            /* single-key { ok: .. } / { err: .. } literals are Results */
            if (n->as.map.count == 1 &&
                (strcmp(n->as.map.keys[0], "ok") == 0 ||
                 strcmp(n->as.map.keys[0], "err") == 0)) {
                ck_expr(c, n->as.map.vals[0], NULL);
                return type_result();
            }
            /* duplicate keys are almost certainly a bug */
            for (int i = 0; i < n->as.map.count; i++)
                for (int j = i + 1; j < n->as.map.count; j++)
                    if (strcmp(n->as.map.keys[i], n->as.map.keys[j]) == 0)
                        ck_fail(c, n->line, "duplicate key '%s' in map literal",
                                n->as.map.keys[i]);

            Type *sel = expected ? resolve(c, expected, n->line) : NULL;
            if (sel && sel->kind == TY_STRUCT && sel->name) {
                /* strict struct: every literal key must be a declared field */
                for (int i = 0; i < n->as.map.count; i++) {
                    int idx = -1;
                    for (int j = 0; j < sel->count; j++)
                        if (strcmp(n->as.map.keys[i], sel->names[j]) == 0) {
                            idx = j;
                            break;
                        }
                    if (idx < 0) {
                        ck_fail(c, n->line, "type %s has no field '%s'",
                                sel->name, n->as.map.keys[i]);
                        continue;
                    }
                    expect_compat(c,
                                  ck_expr(c, n->as.map.vals[i],
                                           sel->types[idx]),
                                  sel->types[idx], n->line, "struct field");
                }
                /* ...and every declared field must be present, otherwise the
                 * per-field runtime lookup would fail on the missing key. */
                for (int j = 0; j < sel->count; j++) {
                    bool present = false;
                    for (int i = 0; i < n->as.map.count; i++)
                        if (strcmp(n->as.map.keys[i], sel->names[j]) == 0) {
                            present = true;
                            break;
                        }
                    if (!present)
                        ck_fail(c, n->line, "type %s is missing field '%s'",
                                sel->name, sel->names[j]);
                }
                return sel;
            }

            /* anonymous struct literal */
            Type *at = type_anon_struct();
            for (int i = 0; i < n->as.map.count; i++) {
                Type *vt = ck_expr(c, n->as.map.vals[i], NULL);
                type_add_member(at, n->as.map.keys[i], vt);
            }
            return at;
        }

        case N_LIST_LIT:
            return ck_list(c, n, expected);

        case N_FUNC_LIT: {
            Type **params = calloc((size_t)n->as.funclit.arity + 1,
                                   sizeof(Type *));
            Type *hint = c->param_hint;
            c->param_hint = NULL; /* one-shot */
            for (int i = 0; i < n->as.funclit.arity; i++)
                params[i] = (hint && i == 0 && !n->as.funclit.param_types[i])
                                ? hint
                                : (n->as.funclit.param_types[i]
                                       ? resolve(c, n->as.funclit.param_types[i],
                                                 n->line)
                                       : any_type());
            Type *ret = n->as.funclit.ret ? resolve(c, n->as.funclit.ret,
                                                    n->line) : any_type();
            Type *ft = type_func(n->as.funclit.arity, params, ret);
            ck_fn(c, n->as.funclit.names, ft, n->as.funclit.body);
            return ft;
        }

        case N_MEMBER: {
            Type *ot = ck_expr(c, n->as.member.obj, NULL);
            ot = resolve(c, ot, n->line);
            if (ot->kind == TY_ANY) return any_type();
            if (ot->kind == TY_STRUCT) {
                for (int i = 0; i < ot->count; i++)
                    if (strcmp(n->as.member.name, ot->names[i]) == 0) {
                        /* Sticky type annotation: the interpreter returns the
                         * member's zero value on a missing key (tool arg). */
                        n->as.member.type = ot->types[i];
                        return ot->types[i];
                    }
                ck_fail(c, n->line, "type has no field '%s'",
                        n->as.member.name);
                return any_type();
            }
            if ((ot->kind == TY_STRING || ot->kind == TY_LIST) &&
                (strcmp(n->as.member.name, "len") == 0 ||
                 strcmp(n->as.member.name, "length") == 0))
                return type_prim(TY_INT);
            ck_fail(c, n->line, "cannot read field '%s' on this type",
                    n->as.member.name);
            return any_type();
        }

        case N_UNARY:
            if (n->as.unary.op == OP_NOT) {
                Type *ot = ck_expr(c, n->as.unary.operand, NULL);
                is_bool_ok(c, ot, n->line);
            } else { /* OP_NEG */
                Type *ot = ck_expr(c, n->as.unary.operand, NULL);
                if (ot && ot->kind != TY_ANY && ot->kind != TY_INT &&
                    ot->kind != TY_FLOAT)
                    ck_fail(c, n->line, "cannot negate this type");
                /* negation keeps the numeric type (-5 is int, -5.5 float) */
                if (ot && ot->kind == TY_FLOAT) return type_prim(TY_FLOAT);
                if (ot && ot->kind == TY_ANY) return any_type();
                return type_prim(TY_INT);
            }

        case N_BINARY: {
            Op op = n->as.binary.op;
            if (op == OP_AND || op == OP_OR) {
                Type *lt = ck_expr(c, n->as.binary.left, NULL);
                Type *rt = ck_expr(c, n->as.binary.right, NULL);
                is_bool_ok(c, lt, n->line);
                is_bool_ok(c, rt, n->line);
                return type_prim(TY_BOOL);
            }
            if (op == OP_EQ || op == OP_NE) {
                ck_expr(c, n->as.binary.left, NULL);
                ck_expr(c, n->as.binary.right, NULL);
                return type_prim(TY_BOOL);
            }
            if (op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE) {
                Type *lt = ck_expr(c, n->as.binary.left, NULL);
                Type *rt = ck_expr(c, n->as.binary.right, NULL);
                /* comparison is numeric or matching strings, never mixed */
                if (lt && rt && lt->kind != TY_ANY && rt->kind != TY_ANY) {
                    bool a_num = lt->kind == TY_INT || lt->kind == TY_FLOAT;
                    bool b_num = rt->kind == TY_INT || rt->kind == TY_FLOAT;
                    bool a_str = lt->kind == TY_STRING;
                    bool b_str = rt->kind == TY_STRING;
                    if (!((a_num && b_num) || (a_str && b_str)))
                        ck_fail(c, n->line,
                                "comparison needs numbers or strings");
                }
                return type_prim(TY_BOOL);
            }
            /* arithmetic */
            Type *lt = ck_expr(c, n->as.binary.left, NULL);
            Type *rt = ck_expr(c, n->as.binary.right, NULL);
            if (op == OP_ADD) {
                /* string + string concatenates */
                if (lt && rt &&
                    (lt->kind == TY_STRING || lt->kind == TY_ANY) &&
                    (rt->kind == TY_STRING || rt->kind == TY_ANY))
                    return lt->kind == TY_STRING ? type_prim(TY_STRING)
                                                 : any_type();
            }
            if (lt && rt && lt->kind != TY_ANY && rt->kind != TY_ANY &&
                !type_compat(c, lt, type_prim(TY_FLOAT), n->line))
                ck_fail(c, n->line, "arithmetic needs numbers");
            return arith_result(lt, rt);
        }

        case N_CALL: {
            Type *callee_t = ck_expr(c, n->as.call.callee, NULL);
            callee_t = resolve(c, callee_t, n->line);
            if (callee_t->kind == TY_ANY) {
                for (int i = 0; i < n->as.call.argc; i++)
                    ck_expr(c, n->as.call.args[i], NULL);
                return any_type();
            }
            if (callee_t->kind != TY_FUNC) {
                ck_fail(c, n->line, "calling a non-function value");
                for (int i = 0; i < n->as.call.argc; i++)
                    ck_expr(c, n->as.call.args[i], NULL);
                return any_type();
            }
            if (callee_t->count != n->as.call.argc) {
                ck_fail(c, n->line, "function expects %d arguments, got %d",
                        callee_t->count, n->as.call.argc);
                return any_type();
            }
            for (int i = 0; i < n->as.call.argc; i++)
                expect_compat(c, ck_expr(c, n->as.call.args[i],
                                         callee_t->types[i]),
                              callee_t->types[i], n->line, "argument");

            Type *ret = callee_t->ret ? callee_t->ret : any_type();
            if (n->as.call.propagate) {
                Type *src = callee_t->ret;
                if (!src || (src->kind != TY_RESULT && src->kind != TY_ANY))
                    ck_fail(c, n->line, "'?' used on a call that does not return Result");
                if (!c->in_func)
                    ck_fail(c, n->line, "'?' is only allowed inside a function");
                if (c->cur_ret && c->cur_ret->kind != TY_RESULT &&
                    c->cur_ret->kind != TY_ANY)
                    ck_fail(c, n->line,
                            "'?' needs the enclosing function to return Result");
                return any_type(); /* payload type is unknown without generics */
            }
            return ret;
        }

        default:
            return any_type(); /* non-expression encountered while atom-checking */
    }
}

/* ===================== statement checking ===================== */

static void ck_blk(Checker *c, Node *n) {
    if (n->type != N_BLOCK) { ck_stmt(c, n); return; }
    CScope *saved = c->scope;
    c->scope = scope_new(saved);
    for (int i = 0; i < n->as.block.count; i++) {
        ck_stmt(c, n->as.block.stmts[i]);
        if (c->failed) break;
    }
    c->scope = saved;
}

/* Check a function body with its params bound and its return type current. */
static void ck_fn(Checker *c, char **names, Type *ft, Node *body) {
    CScope *saved = c->scope;
    Type *ret_saved = c->cur_ret;
    bool in_saved = c->in_func;
    c->scope = scope_new(saved);
    for (int i = 0; i < ft->count; i++)
        scope_put(c->scope, names[i], ft->types[i]);
    c->cur_ret = ft->ret;
    c->in_func = true;
    ck_blk(c, body);
    c->in_func = in_saved;
    c->cur_ret = ret_saved;
    c->scope = saved;
}

/* Map a bare schema type word to a scalar Type; NULL for unknown words. */
static Type *scalar_from_word(const char *w, size_t len) {
    if (len == 3 && strncmp(w, "int", 3) == 0)    return type_prim(TY_INT);
    if (len == 5 && strncmp(w, "float", 5) == 0)  return type_prim(TY_FLOAT);
    if (len == 6 && strncmp(w, "string", 6) == 0) return type_prim(TY_STRING);
    if (len == 3 && strncmp(w, "str", 3) == 0)    return type_prim(TY_STRING);
    if (len == 4 && strncmp(w, "bool", 4) == 0)   return type_prim(TY_BOOL);
    return NULL;
}

/* A string literal's `text`/`len` include the surrounding quotes (the
 * interpreter unescapes at eval time); strip them to read a bare type word. */
static Type *scalar_from_strlit(Node *v) {
    const char *s = v->as.lit.text;
    size_t n = (size_t)v->as.lit.len;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    return scalar_from_word(s, n);
}

/* Derive the tool handler's `arg` struct type straight from the params
 * expression, honoring both `{ a: int }` and `{ a: "int" }`. Non-scalar or
 * non-literal entries degrade to `any` (no zero-default at runtime). */
static Type *tool_param_struct(Node *p) {
    Type *at = type_anon_struct();
    if (!p || p->type != N_MAP_LIT) return at;
    for (int i = 0; i < p->as.map.count; i++) {
        Type *mt = NULL;
        Node *v = p->as.map.vals[i];
        if (v->type == N_VAR)
            mt = scalar_from_word(v->as.var.name, strlen(v->as.var.name));
        else if (v->type == N_LITERAL && v->as.lit.kind == LIT_STR)
            mt = scalar_from_strlit(v);
        else if (v->type == N_MAP_LIT) {
            /* already-schema form: { a: { type: "int" } } */
            for (int j = 0; j < v->as.map.count; j++) {
                if (strcmp(v->as.map.keys[j], "type") != 0) continue;
                Node *tv = v->as.map.vals[j];
                if (tv->type == N_LITERAL && tv->as.lit.kind == LIT_STR)
                    mt = scalar_from_strlit(tv);
                else if (tv->type == N_VAR)
                    mt = scalar_from_word(tv->as.var.name, strlen(tv->as.var.name));
                break;
            }
        }
        type_add_member(at, p->as.map.keys[i], mt ? mt : any_type());
    }
    return at;
}

static void ck_stmt(Checker *c, Node *n) {
    if (c->failed || !n) return;
    switch (n->type) {
        case N_BLOCK:
            ck_blk(c, n);
            return;
        case N_LET: {
            Type *annot = n->as.let.annot ? resolve(c, n->as.let.annot, n->line)
                                          : NULL;
            Type *it = ck_expr(c, n->as.let.init, annot);
            if (annot && !type_compat(c, it, annot, n->line)) {
                ck_fail(c, n->line, "'%s' is not assignable to the declared type '%s' of '%s'",
                        ty_str(it), ty_str(annot), n->as.let.name);
            }
            scope_put(c->scope, n->as.let.name,
                      annot ? annot : (it ? it : any_type()));
            return;
        }
        case N_IF: {
            Type *ct = ck_expr(c, n->as.ifs.cond, NULL);
            is_bool_ok(c, ct, n->line);
            ck_stmt(c, n->as.ifs.then);
            if (n->as.ifs.els) ck_stmt(c, n->as.ifs.els);
            return;
        }
        case N_WHILE: {
            Type *ct = ck_expr(c, n->as.whiles.cond, NULL);
            is_bool_ok(c, ct, n->line);
            ck_stmt(c, n->as.whiles.body);
            return;
        }
        case N_RETURN: {
            if (n->as.ret.expr) {
                Type *rt = ck_expr(c, n->as.ret.expr, c->cur_ret);
                if (c->cur_ret && !type_compat(c, rt, c->cur_ret, n->line))
                    ck_fail(c, n->line, "return value does not match the "
                            "function's return type");
            } /* bare `return` == null, nullable means that's always ok */
            return;
        }
        case N_EXPR_STMT:
            ck_expr(c, n->as.expr_stmt.expr, NULL);
            return;
        case N_FUNC_DECL: {
            Type *ft = scope_get(c->scope, n->as.func.name);
            if (ft && ft->kind == TY_FUNC)
                ck_fn(c, n->as.func.names, ft, n->as.func.body);
            return;
        }
        case N_TYPE_DECL: /* registered in pass A, filled in pass B */
            return;
        case N_SERVER:
            for (int i = 0; i < n->as.server.count; i++)
                ck_expr(c, n->as.server.assigns[i]->as.assign.value, NULL);
            return;
        case N_ROUTE:
            if (n->as.route.handler) ck_expr(c, n->as.route.handler, NULL);
            return;
        case N_TOOL:
            ck_expr(c, n->as.tool.params, NULL);
            if (n->as.tool.handler &&
                n->as.tool.handler->type == N_FUNC_LIT) {
                if (n->as.tool.handler->as.funclit.arity != 1) {
                    ck_fail(c, n->line,
                            "tool handler must take exactly one argument");
                    return;
                }
                /* the handler's `arg` is typed by the tool's own param
                 * schema, so `arg.a` is checkable and defaults on missing */
                c->param_hint = tool_param_struct(n->as.tool.params);
            }
            ck_expr(c, n->as.tool.handler, NULL);
            return;
        case N_VERBS:
            ck_expr(c, n->as.verbs.methods, NULL);
            scope_put(c->scope, n->as.verbs.name, any_type());
            return;
        case N_ASSIGN: {
            Type *vt = scope_get(c->scope, n->as.assign.name);
            if (!vt) {
                ck_fail(c, n->line, "assignment to undefined variable '%s'",
                        n->as.assign.name);
                return;
            }
            Type *it = ck_expr(c, n->as.assign.value, vt);
            if (!type_compat(c, it, vt, n->line))
                ck_fail(c, n->line, "assignment type mismatch for '%s'",
                        n->as.assign.name);
            return;
        }
        case N_ASSIGN_MEMBER: {
            Type *ot = ck_expr(c, n->as.assign_mem.obj, NULL);
            ot = resolve(c, ot, n->line);
            if (ot->kind == TY_ANY) { ck_expr(c, n->as.assign_mem.value, NULL); return; }
            if (ot->kind != TY_STRUCT) {
                ck_fail(c, n->line, "cannot assign a member on this type");
                return;
            }
            int idx = -1;
            for (int i = 0; i < ot->count; i++)
                if (strcmp(n->as.assign_mem.name, ot->names[i]) == 0) { idx = i; break; }
            if (idx < 0) {
                ck_fail(c, n->line, "type has no field '%s'",
                        n->as.assign_mem.name);
                return;
            }
            expect_compat(c, ck_expr(c, n->as.assign_mem.value, ot->types[idx]),
                          ot->types[idx], n->line, "member assignment");
            return;
        }
        default:
            ck_expr(c, n, NULL);
            return;
    }
}

/* ===================== program entry ===================== */

bool type_check_program(Node *prog, char *errbuf, size_t errbuf_size) {
    if (!prog || prog->type != N_PROGRAM) return true;

    Checker c;
    memset(&c, 0, sizeof(c));
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    c.errbuf = errbuf;
    c.errbuf_size = errbuf_size;
    c.scope = scope_new(NULL);

    /* builtins are loose */
    {
        static const char *BUILTINS[] = {
            "run", "print", "str", "int", "len", "keys", "get",
            "json", "stringify", "now", "el", "render", "html",
            "float", "bool", "string", "type", "Result", /* type words usable as idents */
            "write", "read", /* built-in verb groups (see seed_verb_groups) */
            "env", "files", "read_file", "write_file", "mkdir", "strftime", "put",
            "tools", "skills", "mcps",
            "discovery_endpoints", "catalog", /* discovery builtins */
        };
        for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++)
            scope_put(c.scope, BUILTINS[i], any_type());
    }

    /* Pass A: register every struct name before resolving any body, so
     * forward references between structs work. The def Type keeps the name:
     * named-struct strict checking keys off `TY_STRUCT.name`. */
    for (int i = 0; i < prog->as.program.count; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type == N_TYPE_DECL) {
            if (find_struct(&c, s->as.type_decl.name)) {
                ck_fail(&c, s->line, "type '%s' already defined",
                        s->as.type_decl.name);
                break;
            }
            Type *def = type_anon_struct();
            def->name = strdup(s->as.type_decl.name);
            add_struct(&c, s->as.type_decl.name, def);
        }
    }

    /* Pass B: fill struct fields. */
    if (!c.failed) {
        for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
            Node *s = prog->as.program.stmts[i];
            if (s->type != N_TYPE_DECL) continue;
            Type *def = find_struct(&c, s->as.type_decl.name)->type;
            for (int j = 0; j < s->as.type_decl.count; j++)
                type_add_member(def, s->as.type_decl.field_names[j],
                                resolve(&c, s->as.type_decl.field_types[j],
                                        s->line));
        }
    }

    /* Pass C: register function signatures (two passes so mutual recursion
     * between functions resolves). */
    for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type != N_FUNC_DECL) continue;
        Type **params = calloc((size_t)s->as.func.arity + 1, sizeof(Type *));
        for (int j = 0; j < s->as.func.arity; j++)
            params[j] = s->as.func.param_types[j]
                            ? resolve(&c, s->as.func.param_types[j], s->line)
                            : any_type();
        Type *ret = s->as.func.ret ? resolve(&c, s->as.func.ret, s->line)
                                   : any_type();
        scope_put(c.scope, s->as.func.name, type_func(s->as.func.arity,
                                                      params, ret));
    }

    /* Pass D: walk everything (function bodies included). */
    for (int i = 0; i < prog->as.program.count && !c.failed; i++)
        ck_stmt(&c, prog->as.program.stmts[i]);

    return !c.failed;
}