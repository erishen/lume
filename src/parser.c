#include <stdlib.h>
#include "lume.h"

/* Recursive-descent parser with a layered arithmetic grammar
 * (assignment -> or -> and -> equality -> comparison -> term -> factor ->
 * unary -> postfix -> primary). Produces a malloc'd AST that lives for the
 * process lifetime (parsed once before agenthttpd_run forks workers). */

typedef struct {
    Token *toks;
    int count;
    int pos;
    char errbuf[256];
} Parser;

static void perror_at(Parser *p, size_t line, const char *fmt, ...) {
    if (p->errbuf[0]) return;
    va_list ap;
    va_start(ap, fmt);
    snprintf(p->errbuf, sizeof(p->errbuf), "line %zu: ", line);
    size_t used = strlen(p->errbuf);
    vsnprintf(p->errbuf + used, sizeof(p->errbuf) - used, fmt, ap);
    va_end(ap);
}

static Node *nalloc(NodeType type, size_t line) {
    Node *n = calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

static Token peek(const Parser *p)        { return p->toks[p->pos]; }
static Token peek2(const Parser *p)       { return p->toks[p->pos + 1]; }
static size_t previous_line(const Parser *p) { return p->toks[p->pos - 1].line; }
static bool at_end(const Parser *p)       { return peek(p).type == TOK_EOF; }
static bool check(const Parser *p, TokenType t) { return peek(p).type == t; }

static bool advance(Parser *p) {
    if (p->pos < p->count) p->pos++;
    return peek(p).type != TOK_EOF;
}

static bool match(Parser *p, TokenType t) {
    if (check(p, t)) {
        advance(p);
        return true;
    }
    return false;
}

static bool expect(Parser *p, TokenType t) {
    if (check(p, t)) {
        advance(p);
        return true;
    }
    perror_at(p, peek(p).line, "expected %s, got %s",
              token_type_name(t), token_type_name(peek(p).type));
    return false;
}

static char *ident_name(Parser *p, Token t) {
    (void)p;
    char *s = malloc((size_t)t.length + 1);
    memcpy(s, t.start, (size_t)t.length);
    s[t.length] = '\0';
    return s;
}

/* `type`/`int`/... are keywords in type position but stay usable as map keys
 * and member field names (e.g. a route response's `type` field). */
/* `get`/`post`/... are route-shorthand keywords at statement level, but stay
 * usable as identifiers/keys elsewhere (same rule as `type`/`int`) so the
 * `get(m, key)` builtin and `{ get: 1 }` maps keep working. */
static bool is_method_keyword(TokenType t) {
    return t == TOK_GET || t == TOK_HEAD || t == TOK_POST || t == TOK_PUT ||
           t == TOK_PATCH || t == TOK_DELETE || t == TOK_OPTIONS;
}

static const char *method_keyword_name(TokenType t) {
    switch (t) {
        case TOK_GET:     return "GET";
        case TOK_HEAD:    return "HEAD";
        case TOK_POST:    return "POST";
        case TOK_PUT:     return "PUT";
        case TOK_PATCH:   return "PATCH";
        case TOK_DELETE:  return "DELETE";
        case TOK_OPTIONS: return "OPTIONS";
        default:          return "";
    }
}

static bool is_field_token(TokenType t) {
    return t == TOK_IDENT || t == TOK_TYPE || t == TOK_INT || t == TOK_FLOAT ||
           t == TOK_KW_STRING || t == TOK_BOOL || t == TOK_RESULT ||
           is_method_keyword(t);
}

static Node *parse_expression(Parser *p);
static Node *parse_statement(Parser *p);

/* ---------- types ---------- */

/* type := ('int' | 'float' | 'string' | 'bool' | 'Result' | IDENT) '[' ']'* */
static Type *parse_type(Parser *p) {
    Token t = peek(p);
    Type *ty = NULL;
    switch (t.type) {
        case TOK_INT:    advance(p); ty = type_prim(TY_INT); break;
        case TOK_FLOAT:  advance(p); ty = type_prim(TY_FLOAT); break;
        case TOK_KW_STRING: advance(p); ty = type_prim(TY_STRING); break;
        case TOK_BOOL:   advance(p); ty = type_prim(TY_BOOL); break;
        case TOK_RESULT: advance(p); ty = type_result(); break;
        case TOK_IDENT:  advance(p); ty = type_struct(ident_name(p, t)); break;
        default:
            perror_at(p, t.line, "expected a type, got %s",
                      token_type_name(t.type));
            return NULL;
    }
    while (match(p, TOK_LBRACKET)) {
        if (!expect(p, TOK_RBRACKET)) return NULL;
        ty = type_list(ty);
    }
    return ty;
}

/* ---------- blocks & statements ---------- */

static Node *parse_block(Parser *p) {
    size_t line = peek(p).line;
    if (!expect(p, TOK_LBRACE)) return NULL;
    Node *n = nalloc(N_BLOCK, line);
    n->as.block.stmts = NULL;
    n->as.block.count = 0;
    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        Node *s = parse_statement(p);
        if (!s) return NULL;
        n->as.block.stmts = realloc(n->as.block.stmts,
                                    sizeof(Node *) * ((size_t)n->as.block.count + 1));
        n->as.block.stmts[n->as.block.count++] = s;
    }
    if (!expect(p, TOK_RBRACE)) return NULL;
    return n;
}

/* func ( a, b ) — params consumed, names + types + arity returned.
 * A param type (nullable Type*) is NULL when unannotated. */
static bool parse_params(Parser *p, char ***names_out, Type ***types_out,
                         int *arity_out) {
    char **names = NULL;
    Type **types = NULL;
    int arity = 0;
    if (!expect(p, TOK_LPAREN)) return false;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected parameter name", NULL);
                free(names);
                free(types);
                return false;
            }
            Token pt = peek(p);
            advance(p);
            names = realloc(names, sizeof(char *) * ((size_t)arity + 1));
            types = realloc(types, sizeof(Type *) * ((size_t)arity + 1));
            names[arity] = ident_name(p, pt);
            types[arity] = NULL;
            if (match(p, TOK_COLON)) {
                types[arity] = parse_type(p);
                if (!types[arity]) {
                    for (int i = 0; i < arity; i++) free(names[i]);
                    free(names);
                    free(types);
                    return false;
                }
            }
            arity++;
        } while (match(p, TOK_COMMA));
    }
    if (!expect(p, TOK_RPAREN)) { free(names); free(types); return false; }
    *names_out = names;
    *types_out = types;
    *arity_out = arity;
    return true;
}

/* optional ':' return-type annotation after params (). */
static Type *parse_optional_ret(Parser *p) {
    Type *ret = NULL;
    if (match(p, TOK_COLON)) {
        ret = parse_type(p);
        if (!ret) return NULL;
    }
    return ret;
}

static Node *parse_func_literal(Parser *p, size_t kw_line) {
    /* TOK_FUNC already consumed. */
    Node *n = nalloc(N_FUNC_LIT, kw_line);
    if (!parse_params(p, &n->as.funclit.names, &n->as.funclit.param_types,
                      &n->as.funclit.arity)) return NULL;
    n->as.funclit.ret = parse_optional_ret(p);
    if (peek(p).type == TOK_EOF && !n->as.funclit.ret) {
        perror_at(p, kw_line, "expected function body or return type", NULL);
        return NULL;
    }
    n->as.funclit.body = parse_block(p);
    if (!n->as.funclit.body) return NULL;
    return n;
}

/* `"path", handler;` — shared tail of the `route` statement and the
 * `get`/`post`/.../verbs-alias shorthand forms. The caller must have set
 * exactly one of n->as.route.method / n->as.route.alias. */
static Node *parse_route_tail(Parser *p, Node *n) {
    if (!check(p, TOK_STRING)) {
        perror_at(p, peek(p).line, "expected path string in route", NULL);
        return NULL;
    }
    Token path = peek(p);
    advance(p);
    n->as.route.path = malloc((size_t)(path.length - 2) + 1);
    memcpy(n->as.route.path, path.start + 1, (size_t)(path.length - 2));
    n->as.route.path[path.length - 2] = '\0';
    /* The handler is optional: `write "/items";` registers the routes with a
     * default response (action/method/body). */
    if (match(p, TOK_COMMA)) {
        n->as.route.handler = parse_expression(p);
        if (!n->as.route.handler) return NULL;
    }
    if (!expect(p, TOK_SEMI)) return NULL;
    return n;
}

static Node *parse_statement(Parser *p) {
    Token t = peek(p);

    if (t.type == TOK_LBRACE) {
        return parse_block(p);
    }
    if (t.type == TOK_IF) {
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *cond = parse_expression(p);
        if (!cond) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        Node *then = parse_statement(p);
        if (!then) return NULL;
        Node *els = NULL;
        if (match(p, TOK_ELSE)) {
            els = parse_statement(p);
            if (!els) return NULL;
        }
        Node *n = nalloc(N_IF, t.line);
        n->as.ifs.cond = cond;
        n->as.ifs.then = then;
        n->as.ifs.els = els;
        return n;
    }
    if (t.type == TOK_WHILE) {
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *cond = parse_expression(p);
        if (!cond) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        Node *body = parse_statement(p);
        if (!body) return NULL;
        Node *n = nalloc(N_WHILE, t.line);
        n->as.whiles.cond = cond;
        n->as.whiles.body = body;
        return n;
    }
    if (t.type == TOK_FOR) {
        /* for (init; cond; incr) body  — C-style; init may be `let` or an
         * expression, cond/incr optional (`for (;;)`) — and the two for-in
         * forms: `for (x in xs)` / `for (let x in xs)`. */
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *n = nalloc(N_FOR, t.line);
        if (check(p, TOK_LET)) {
            /* `let` head: either `let x in xs` (for-in) or `let i = e` (C) */
            advance(p);
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected variable name after 'let'", NULL);
                return NULL;
            }
            Token vn = peek(p);
            advance(p);
            if (match(p, TOK_IN)) {
                n->as.fors.is_in = true;
                n->as.fors.var = ident_name(p, vn);
                n->as.fors.iterable = parse_expression(p);
                if (!n->as.fors.iterable) return NULL;
                if (!expect(p, TOK_RPAREN)) return NULL;
                n->as.fors.body = parse_statement(p);
                if (!n->as.fors.body) return NULL;
                return n;
            }
            Node *init = nalloc(N_LET, vn.line);
            init->as.let.name = ident_name(p, vn);
            if (match(p, TOK_COLON)) {
                init->as.let.annot = parse_type(p);
                if (!init->as.let.annot) return NULL;
            }
            if (!expect(p, TOK_EQ)) return NULL;
            init->as.let.init = parse_expression(p);
            if (!init->as.let.init) return NULL;
            n->as.fors.init = init;
            if (!expect(p, TOK_SEMI)) return NULL;
            if (!check(p, TOK_SEMI)) {
                n->as.fors.cond = parse_expression(p);
                if (!n->as.fors.cond) return NULL;
            }
            if (!expect(p, TOK_SEMI)) return NULL;
            if (!check(p, TOK_RPAREN)) {
                Node *incr = nalloc(N_EXPR_STMT, peek(p).line);
                incr->as.expr_stmt.expr = parse_expression(p);
                if (!incr->as.expr_stmt.expr) return NULL;
                n->as.fors.incr = incr;
            }
            if (!expect(p, TOK_RPAREN)) return NULL;
            n->as.fors.body = parse_statement(p);
            if (!n->as.fors.body) return NULL;
            return n;
        }
        if (check(p, TOK_IDENT) && peek2(p).type == TOK_IN) {
            Token vn = peek(p);
            advance(p);
            advance(p); /* in */
            n->as.fors.is_in = true;
            n->as.fors.var = ident_name(p, vn);
            n->as.fors.iterable = parse_expression(p);
            if (!n->as.fors.iterable) return NULL;
            if (!expect(p, TOK_RPAREN)) return NULL;
            n->as.fors.body = parse_statement(p);
            if (!n->as.fors.body) return NULL;
            return n;
        }
        /* C-style with an expression init (assignment / call / ...) */
        if (!check(p, TOK_SEMI)) {
            Node *init = nalloc(N_EXPR_STMT, peek(p).line);
            init->as.expr_stmt.expr = parse_expression(p);
            if (!init->as.expr_stmt.expr) return NULL;
            n->as.fors.init = init;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        if (!check(p, TOK_SEMI)) {
            n->as.fors.cond = parse_expression(p);
            if (!n->as.fors.cond) return NULL;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        if (!check(p, TOK_RPAREN)) {
            Node *incr = nalloc(N_EXPR_STMT, peek(p).line);
            incr->as.expr_stmt.expr = parse_expression(p);
            if (!incr->as.expr_stmt.expr) return NULL;
            n->as.fors.incr = incr;
        }
        if (!expect(p, TOK_RPAREN)) return NULL;
        n->as.fors.body = parse_statement(p);
        if (!n->as.fors.body) return NULL;
        return n;
    }
    if (t.type == TOK_BREAK || t.type == TOK_CONTINUE) {
        TokenType kt = t.type;
        advance(p);
        if (!expect(p, TOK_SEMI)) return NULL;
        return nalloc(kt == TOK_BREAK ? N_BREAK : N_CONTINUE, t.line);
    }
    if (t.type == TOK_LET) {
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected variable name after 'let'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        Type *annot = NULL;
        if (match(p, TOK_COLON)) {
            annot = parse_type(p);
            if (!annot) return NULL;
        }
        if (!expect(p, TOK_EQ)) return NULL;
        Node *init = parse_expression(p);
        if (!init) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_LET, t.line);
        n->as.let.name = ident_name(p, name);
        n->as.let.annot = annot;
        n->as.let.init = init;
        return n;
    }
    if (t.type == TOK_VERBS) {
        /* verbs name = ["POST", "PUT", ...];  — a reusable method group that
         * `name "path", handler;` expands to (one route per method). */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected verb-group name after 'verbs'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        if (!expect(p, TOK_EQ)) return NULL;
        Node *methods = parse_expression(p);
        if (!methods) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_VERBS, t.line);
        n->as.verbs.name = ident_name(p, name);
        n->as.verbs.methods = methods;
        return n;
    }
    if (t.type == TOK_RETURN) {
        advance(p);
        Node *expr = NULL;
        if (!check(p, TOK_SEMI)) {
            expr = parse_expression(p);
            if (!expr) return NULL;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_RETURN, t.line);
        n->as.ret.expr = expr;
        return n;
    }
    if (t.type == TOK_SERVER) {
        /* server { name = expr; ... } */
        advance(p);
        Node *n = nalloc(N_SERVER, t.line);
        n->as.server.assigns = NULL;
        n->as.server.count = 0;
        if (!expect(p, TOK_LBRACE)) return NULL;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected config field name", NULL);
                return NULL;
            }
            Token key = peek(p);
            advance(p);
            if (!expect(p, TOK_EQ)) return NULL;
            Node *value = parse_expression(p);
            if (!value) return NULL;
            if (!expect(p, TOK_SEMI)) return NULL;
            Node *a = nalloc(N_ASSIGN, key.line);
            a->as.assign.name = ident_name(p, key);
            a->as.assign.value = value;
            n->as.server.assigns = realloc(n->as.server.assigns,
                                           sizeof(Node *) * ((size_t)n->as.server.count + 1));
            n->as.server.assigns[n->as.server.count++] = a;
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_ROUTE) {
        /* route "METHOD", "path|path*", handler; */
        advance(p);
        Node *n = nalloc(N_ROUTE, t.line);
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected method string in route", NULL);
            return NULL;
        }
        Token m = peek(p);
        advance(p);
        n->as.route.method = malloc((size_t)(m.length - 2) + 1);
        memcpy(n->as.route.method, m.start + 1, (size_t)(m.length - 2));
        n->as.route.method[m.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        return parse_route_tail(p, n);
    }
    if (is_method_keyword(t.type)) {
        /* get "path", handler;  ≡  route "GET", "path", handler;
         * Only when the *next* token is a string is this the route form;
         * otherwise it stays a plain expression statement (`get(m, key);`). */
        if (p->pos + 1 < p->count && p->toks[p->pos + 1].type == TOK_STRING) {
            advance(p); /* consume the method keyword */
            Node *n = nalloc(N_ROUTE, t.line);
            n->as.route.method = strdup(method_keyword_name(t.type));
            return parse_route_tail(p, n);
        }
    }
    if (t.type == TOK_IDENT &&
        p->pos + 1 < p->count && p->toks[p->pos + 1].type == TOK_STRING) {
        /* name "path", handler;  — `name` must be a `verbs` group declared
         * earlier; the bridge resolves it to one route per method. A bare
         * identifier followed by a string is not a valid expression, so this
         * form is unambiguous. */
        advance(p); /* consume the group name */
        Node *n = nalloc(N_ROUTE, t.line);
        n->as.route.alias = ident_name(p, t);
        return parse_route_tail(p, n);
    }
    if (t.type == TOK_TOOL) {
        /* tool "name", "desc", {params}, handler; */
        advance(p);
        Node *n = nalloc(N_TOOL, t.line);
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected name string in tool", NULL);
            return NULL;
        }
        Token nm = peek(p);
        advance(p);
        n->as.tool.name = malloc((size_t)(nm.length - 2) + 1);
        memcpy(n->as.tool.name, nm.start + 1, (size_t)(nm.length - 2));
        n->as.tool.name[nm.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected description string in tool", NULL);
            return NULL;
        }
        Token ds = peek(p);
        advance(p);
        n->as.tool.desc = malloc((size_t)(ds.length - 2) + 1);
        memcpy(n->as.tool.desc, ds.start + 1, (size_t)(ds.length - 2));
        n->as.tool.desc[ds.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        n->as.tool.params = parse_expression(p);
        if (!n->as.tool.params) return NULL;
        if (!expect(p, TOK_COMMA)) return NULL;
        n->as.tool.handler = parse_expression(p);
        if (!n->as.tool.handler) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        return n;
    }
    if (t.type == TOK_TYPE) {
        /* type Name = { field: type, ... }; */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected type name after 'type'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        if (!expect(p, TOK_EQ)) return NULL;
        if (!expect(p, TOK_LBRACE)) return NULL;
        Node *n = nalloc(N_TYPE_DECL, t.line);
        n->as.type_decl.name = ident_name(p, name);
        n->as.type_decl.field_names = NULL;
        n->as.type_decl.field_types = NULL;
        n->as.type_decl.count = 0;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected field name in type", NULL);
                return NULL;
            }
            Token f = peek(p);
            advance(p);
            if (!expect(p, TOK_COLON)) return NULL;
            Type *ft = parse_type(p);
            if (!ft) return NULL;
            n->as.type_decl.field_names =
                realloc(n->as.type_decl.field_names,
                        sizeof(char *) * ((size_t)n->as.type_decl.count + 1));
            n->as.type_decl.field_types =
                realloc(n->as.type_decl.field_types,
                        sizeof(Type *) * ((size_t)n->as.type_decl.count + 1));
            n->as.type_decl.field_names[n->as.type_decl.count] = ident_name(p, f);
            n->as.type_decl.field_types[n->as.type_decl.count] = ft;
            n->as.type_decl.count++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_FUNC) {
        /* top-level named function declaration */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected function name after 'func'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        Node *n = nalloc(N_FUNC_DECL, t.line);
        n->as.func.name = ident_name(p, name);
        if (!parse_params(p, &n->as.func.names, &n->as.func.param_types,
                          &n->as.func.arity)) return NULL;
        n->as.func.ret = parse_optional_ret(p);
        n->as.func.body = parse_block(p);
        if (!n->as.func.body) return NULL;
        return n;
    }

    /* expression statement (run(); a = b; f(); ...) */
    Node *expr = parse_expression(p);
    if (!expr) return NULL;
    if (!expect(p, TOK_SEMI)) return NULL;
    Node *n = nalloc(N_EXPR_STMT, t.line);
    n->as.expr_stmt.expr = expr;
    return n;
}

/* ---------- expressions ---------- */

static Node *parse_primary(Parser *p) {
    Token t = peek(p);

    if (t.type == TOK_NUMBER) {
        advance(p);
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = LIT_NUM;
        n->as.lit.num = t.num;
        bool is_float = false;
        for (int i = 0; i < t.length && !is_float; i++)
            if (t.start[i] == '.' || t.start[i] == 'e' || t.start[i] == 'E')
                is_float = true;
        n->as.lit.is_float = is_float;
        return n;
    }
    if (t.type == TOK_STRING) {
        advance(p);
        /* JS-style adjacent string literals: "a" "b" is "ab". Merge the
         * quote-inclusive spans here; the combined literal unescapes at
         * eval time like any other, so \" and \\n inside either part keep
         * their meaning across the join. */
        size_t mlen = (size_t)t.length;
        char *merged = malloc(mlen + 1);
        memcpy(merged, t.start, mlen);
        merged[mlen] = '\0';
        while (peek(p).type == TOK_STRING) {
            Token u = peek(p);
            advance(p);
            size_t inner = (size_t)u.length - 2; /* drop u's quotes */
            merged = realloc(merged, mlen - 1 + inner + 1 + 1);
            memcpy(merged + mlen - 1, u.start + 1, inner); /* overwrite our closing quote */
            mlen = mlen - 1 + inner + 1;
            merged[mlen - 1] = '"';
            merged[mlen] = '\0';
        }
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = LIT_STR;
        n->as.lit.text = merged; /* includes quotes; unescaped at eval */
        n->as.lit.len = mlen;
        return n;
    }
    if (t.type == TOK_TRUE || t.type == TOK_FALSE || t.type == TOK_NULL) {
        advance(p);
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = (t.type == TOK_TRUE) ? LIT_TRUE
                       : (t.type == TOK_FALSE) ? LIT_FALSE : LIT_NULL;
        return n;
    }
    if (t.type == TOK_LPAREN) {
        /* `(a, b) => { ... }`: arrow-function params. Probe parse_params and
         * roll back to a plain grouped expression when the next token isn't
         * `=>` (e.g. `(a) * 3` or `if ((a))`). */
        size_t save_pos = p->pos;
        bool err_was_empty = !p->errbuf[0];
        char **arrow_names = NULL;
        Type **arrow_types = NULL;
        int arrow_arity = 0;
        if (parse_params(p, &arrow_names, &arrow_types, &arrow_arity) &&
            check(p, TOK_ARROW)) {
            advance(p); /* consume `=>` */
            Node *n = nalloc(N_FUNC_LIT, t.line);
            n->as.funclit.names = arrow_names;
            n->as.funclit.param_types = arrow_types;
            n->as.funclit.arity = arrow_arity;
            n->as.funclit.ret = NULL;    /* arrows carry no return annotation */
            n->as.funclit.body = parse_block(p);
            if (!n->as.funclit.body) return NULL;
            return n;
        }
        /* Not an arrow function: free the probe and re-parse as grouping. */
        for (int i = 0; i < arrow_arity; i++) free(arrow_names[i]);
        free(arrow_names);
        free(arrow_types);
        p->pos = save_pos;
        if (err_was_empty) p->errbuf[0] = '\0';
        advance(p);
        Node *e = parse_expression(p);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        return e;
    }
    if (t.type == TOK_LBRACE) {
        /* map literal */
        advance(p);
        Node *n = nalloc(N_MAP_LIT, t.line);
        n->as.map.keys = NULL;
        n->as.map.vals = NULL;
        n->as.map.count = 0;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected map key (name or string)", NULL);
                return NULL;
            }
            Token k = peek(p);
            advance(p);
            if (!expect(p, TOK_COLON)) return NULL;
            Node *v = parse_expression(p);
            if (!v) return NULL;
            n->as.map.keys = realloc(n->as.map.keys,
                                     sizeof(char *) * ((size_t)n->as.map.count + 1));
            n->as.map.vals = realloc(n->as.map.vals,
                                     sizeof(Node *) * ((size_t)n->as.map.count + 1));
            n->as.map.keys[n->as.map.count] = ident_name(p, k);
            n->as.map.vals[n->as.map.count] = v;
            n->as.map.count++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_LBRACKET) {
        advance(p);
        Node *n = nalloc(N_LIST_LIT, t.line);
        n->as.list.items = NULL;
        n->as.list.count = 0;
        while (!check(p, TOK_RBRACKET) && !at_end(p)) {
            Node *e = parse_expression(p);
            if (!e) return NULL;
            n->as.list.items = realloc(n->as.list.items,
                                       sizeof(Node *) * ((size_t)n->as.list.count + 1));
            n->as.list.items[n->as.list.count++] = e;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACKET)) break;
        }
        if (!expect(p, TOK_RBRACKET)) return NULL;
        return n;
    }
    if (t.type == TOK_FUNC) {
        size_t line = t.line;
        advance(p);
        return parse_func_literal(p, line);
    }
    if (t.type == TOK_IDENT || t.type == TOK_TYPE || t.type == TOK_INT ||
        t.type == TOK_FLOAT || t.type == TOK_KW_STRING || t.type == TOK_BOOL ||
        t.type == TOK_RESULT || is_method_keyword(t.type)) {
        advance(p);
        Node *n = nalloc(N_VAR, t.line);
        n->as.var.name = ident_name(p, t);
        return n;
    }

    perror_at(p, t.line, "unexpected token '%s' in expression",
              token_type_name(t.type));
    return NULL;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    if (!n) return NULL;
    while (true) {
        if (match(p, TOK_LPAREN)) {
            Node *args = nalloc(N_LIST_LIT, previous_line(p));
            args->as.list.items = NULL;
            args->as.list.count = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    Node *e = parse_expression(p);
                    if (!e) return NULL;
                    args->as.list.items = realloc(args->as.list.items,
                                                  sizeof(Node *) * ((size_t)args->as.list.count + 1));
                    args->as.list.items[args->as.list.count++] = e;
                } while (match(p, TOK_COMMA));
            }
            if (!expect(p, TOK_RPAREN)) return NULL;
            Node *call = nalloc(N_CALL, previous_line(p));
            call->as.call.callee = n;
            call->as.call.args = args->as.list.items;
            call->as.call.argc = args->as.list.count;
            call->as.call.propagate = false;
            free(args);
            if (match(p, TOK_QUESTION))
                call->as.call.propagate = true;
            n = call;
        } else if (match(p, TOK_DOT)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected field name after '.'", NULL);
                return NULL;
            }
            Token f = peek(p);
            advance(p);
            Node *member = nalloc(N_MEMBER, f.line);
            member->as.member.obj = n;
            member->as.member.name = ident_name(p, f);
            n = member;
        } else {
            break;
        }
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    Token t = peek(p);
    if (t.type == TOK_NOT || t.type == TOK_MINUS) {
        advance(p);
        Node *operand = parse_unary(p);
        if (!operand) return NULL;
        Node *n = nalloc(N_UNARY, t.line);
        n->as.unary.op = (t.type == TOK_NOT) ? OP_NOT : OP_NEG;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_postfix(p);
}

static Node *parse_factor(Parser *p) {
    Node *left = parse_unary(p);
    if (!left) return NULL;
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_unary(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_STAR) ? OP_MUL
                        : (t.type == TOK_SLASH) ? OP_DIV : OP_MOD;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_term(Parser *p) {
    Node *left = parse_factor(p);
    if (!left) return NULL;
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_factor(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_PLUS) ? OP_ADD : OP_SUB;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_comparison(Parser *p) {
    Node *left = parse_term(p);
    if (!left) return NULL;
    while (check(p, TOK_LT) || check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_term(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_LT) ? OP_LT
                        : (t.type == TOK_LE) ? OP_LE
                        : (t.type == TOK_GT) ? OP_GT : OP_GE;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_equality(Parser *p) {
    Node *left = parse_comparison(p);
    if (!left) return NULL;
    while (check(p, TOK_EQEQ) || check(p, TOK_NEQ)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_comparison(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_EQEQ) ? OP_EQ : OP_NE;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_equality(p);
    if (!left) return NULL;
    while (check(p, TOK_AND)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_equality(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = OP_AND;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_and(p);
    if (!left) return NULL;
    while (check(p, TOK_OR)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_and(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = OP_OR;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_expression(Parser *p) {
    Node *expr = parse_or(p);
    if (!expr) return NULL;
    /* assignment: lhs must be a variable or a member access. */
    if (match(p, TOK_EQ)) {
        Node *value = parse_expression(p);
        if (!value) return NULL;
        if (expr->type == N_VAR) {
            Node *n = nalloc(N_ASSIGN, previous_line(p));
            n->as.assign.name = expr->as.var.name;
            n->as.assign.value = value;
            free(expr);
            return n;
        }
        if (expr->type == N_MEMBER) {
            Node *n = nalloc(N_ASSIGN_MEMBER, previous_line(p));
            n->as.assign_mem.obj = expr->as.member.obj;
            n->as.assign_mem.name = expr->as.member.name;
            n->as.assign_mem.value = value;
            free(expr);
            return n;
        }
        perror_at(p, previous_line(p), "invalid assignment target", NULL);
        return NULL;
    }
    return expr;
}

/* ---------- program ---------- */

Node *parse_program(const char *source, char *errbuf, size_t errbuf_size) {
    char lexerr[256] = {0};
    int count = 0;
    Token *toks = al_lex(source, lexerr, sizeof(lexerr), &count);
    if (!toks) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "%s", lexerr);
        return NULL;
    }

    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.count = count;

    Node *prog = nalloc(N_PROGRAM, 1);
    prog->as.program.stmts = NULL;
    prog->as.program.count = 0;

    bool ok = true;
    while (!at_end(&p) && ok) {
        if (peek(&p).type == TOK_SEMI) {
            advance(&p); /* tolerate stray semicolons */
            continue;
        }
        Node *s = parse_statement(&p);
        if (!s) {
            ok = false;
            break;
        }
        prog->as.program.stmts = realloc(prog->as.program.stmts,
                                         sizeof(Node *) * ((size_t)prog->as.program.count + 1));
        prog->as.program.stmts[prog->as.program.count++] = s;
    }

    if (!ok) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "%s%s", p.errbuf,
                     p.errbuf[0] ? "" : "parse error");
        free(toks);
        return NULL;
    }
    free(toks);
    return prog;
}

/* ---------- debug dump ---------- */

static void indent_print(int n) { for (int i = 0; i < n; i++) printf("  "); }

static void dump_lit(Node *n, int depth) {
    switch (n->as.lit.kind) {
        case LIT_NUM: indent_print(depth); printf("num %g\n", n->as.lit.num); break;
        case LIT_STR:
            indent_print(depth);
            printf("str \"%.*s\"\n", n->as.lit.len, n->as.lit.text);
            break;
        case LIT_TRUE:  indent_print(depth); printf("true\n"); break;
        case LIT_FALSE: indent_print(depth); printf("false\n"); break;
        case LIT_NULL:  indent_print(depth); printf("null\n"); break;
    }
}

void node_print(Node *n, int depth) {
    if (!n) { indent_print(depth); printf("<null>\n"); return; }
    switch (n->type) {
        case N_PROGRAM:
            indent_print(depth); printf("program\n");
            for (int i = 0; i < n->as.program.count; i++)
                node_print(n->as.program.stmts[i], depth + 1);
            break;
        case N_BLOCK:
            indent_print(depth); printf("block\n");
            for (int i = 0; i < n->as.block.count; i++)
                node_print(n->as.block.stmts[i], depth + 1);
            break;
        case N_EXPR_STMT:
            indent_print(depth); printf("expr-stmt\n");
            node_print(n->as.expr_stmt.expr, depth + 1);
            break;
        case N_LITERAL:
            dump_lit(n, depth);
            break;
        case N_LET:
            indent_print(depth); printf("let %s\n", n->as.let.name);
            node_print(n->as.let.init, depth + 1);
            break;
        case N_ROUTE:
            indent_print(depth);
            if (n->as.route.alias)
                printf("route (verbs %s) %s\n", n->as.route.alias, n->as.route.path);
            else
                printf("route %s %s\n", n->as.route.method, n->as.route.path);
            if (n->as.route.handler)
                node_print(n->as.route.handler, depth + 1);
            else {
                indent_print(depth + 1);
                printf("(default handler)\n");
            }
            break;
        case N_VERBS:
            indent_print(depth); printf("verbs %s =\n", n->as.verbs.name);
            node_print(n->as.verbs.methods, depth + 1);
            break;
        case N_TYPE_DECL:
            indent_print(depth); printf("type %s\n", n->as.type_decl.name);
            for (int i = 0; i < n->as.type_decl.count; i++) {
                indent_print(depth + 1);
                printf("%s: ", n->as.type_decl.field_names[i]);
                type_print(n->as.type_decl.field_types[i]);
                printf("\n");
            }
            break;
        case N_FUNC_DECL:
            indent_print(depth);
            printf("func %s(", n->as.func.name);
            for (int i = 0; i < n->as.func.arity; i++) {
                if (i) printf(", ");
                printf("%s", n->as.func.names[i]);
                if (n->as.func.param_types[i]) {
                    printf(": ");
                    type_print(n->as.func.param_types[i]);
                }
            }
            printf(")");
            if (n->as.func.ret) { printf(": "); type_print(n->as.func.ret); }
            printf("\n");
            node_print(n->as.func.body, depth + 1);
            break;
        default:
            indent_print(depth); printf("node(type=%d)\n", (int)n->type);
            break;
    }
}