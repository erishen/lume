/* Local functions (SPEC 4.14): the hoisting rewrite, host-side copy.
 *
 * Upstream (lume-core) keeps lume_resolve_localfns() and lume_collect_funcs()
 * in codegen_sig.c, a file that belongs to the native backends this host
 * does not link. The checker's registration pass calls both, so the host
 * carries this verbatim copy of the rewrite region. It is pure AST work -
 * no IR, no types table, no codegen state - so the copy is exact: same
 * function bodies, same diagnostics, same idempotence. The only local
 * additions are the two OOM-exiting allocators (lf_realloc / lf_strdup),
 * mirroring codegen.c's xrealloc / xstrdup contract (exit 70).
 *
 * SYNC RULE: when a lang sync brings in a lume-core that touched the
 * localfns region of src/codegen_sig.c (functions nv_push, fn_collect_walk,
 * lume_collect_funcs, lf_rename_calls, lf_scan_locals, lf_fail,
 * lf_walk_fn_body, lume_resolve_localfns), re-diff that region against this
 * file and port the changes. check-sync cannot see this file - it is
 * host-only by design - so the obligation lives in this comment.
 */
#include "lume.h"
#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *lf_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("lume(lang): out of memory\n", stderr);
        exit(70);
    }
    return q;
}

static char *lf_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)lf_realloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

/* Grow-on-push node list. Pointers only - nothing here owns a Node. */

typedef struct NodeVec { Node **v; int n, cap; } NodeVec;

typedef struct LfCtx {
    int    counter;                 /* synthetic-name source */
    bool   failed;
    char  *err;
    size_t err_size;
} LfCtx;

/* forward: same tu, defined below this point. */
static void lf_walk_fn_body(LfCtx *c, Node *body);

/* forward: same tu, defined below this point. */
static void lf_scan_locals(Node *n, NodeVec *out);

/* forward: same tu, defined below this point. */
static void lf_rename_calls(Node *n, const char *from, const char *to);

/* forward: same tu, defined below this point. */
static void fn_collect_walk(Node *n, NodeVec *out);

static void nv_push(NodeVec *s, Node *n)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (Node **)lf_realloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = n;
}

/* Every function declaration in the tree, pre-order (a parent always sits
 * before the locals nested inside it). Shared by the checker's registration
 * pass and both native backends' signature/emit collection, so all four see
 * exactly the same function set. */

static void fn_collect_walk(Node *n, NodeVec *out)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        nv_push(out, n);
        fn_collect_walk(n->as.func.body, out);
        return;
    case N_FUNC_LIT:
        fn_collect_walk(n->as.funclit.body, out);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            fn_collect_walk(n->as.block.stmts[i], out);
        return;
    case N_LET:        fn_collect_walk(n->as.let.init, out); return;
    case N_LET_PAT:    fn_collect_walk(n->as.letpat.init, out); return;
    case N_LET_EXPR:
        fn_collect_walk(n->as.letx.init, out);
        fn_collect_walk(n->as.letx.body, out);
        return;
    case N_IF:
        fn_collect_walk(n->as.ifs.cond, out);
        fn_collect_walk(n->as.ifs.then, out);
        fn_collect_walk(n->as.ifs.els,  out);
        return;
    case N_WHILE:
        fn_collect_walk(n->as.whiles.cond, out);
        fn_collect_walk(n->as.whiles.body, out);
        return;
    case N_FOR:
        fn_collect_walk(n->as.fors.init,     out);
        fn_collect_walk(n->as.fors.cond,     out);
        fn_collect_walk(n->as.fors.incr,     out);
        fn_collect_walk(n->as.fors.iterable, out);
        fn_collect_walk(n->as.fors.body,     out);
        return;
    case N_RETURN:     fn_collect_walk(n->as.ret.expr, out); return;
    case N_EXPR_STMT:  fn_collect_walk(n->as.expr_stmt.expr, out); return;
    case N_ASSIGN:     fn_collect_walk(n->as.assign.value, out); return;
    case N_ASSIGN_MEMBER:
        fn_collect_walk(n->as.assign_mem.obj,   out);
        fn_collect_walk(n->as.assign_mem.value, out);
        return;
    case N_CALL:
        fn_collect_walk(n->as.call.callee, out);
        for (int i = 0; i < n->as.call.argc; i++)
            fn_collect_walk(n->as.call.args[i], out);
        return;
    case N_MEMBER:     fn_collect_walk(n->as.member.obj, out); return;
    case N_INDEX:
        fn_collect_walk(n->as.index.obj,   out);
        fn_collect_walk(n->as.index.index, out);
        return;
    case N_UNARY:      fn_collect_walk(n->as.unary.operand, out); return;
    case N_BINARY:
        fn_collect_walk(n->as.binary.left,  out);
        fn_collect_walk(n->as.binary.right, out);
        return;
    case N_TERNARY:
        fn_collect_walk(n->as.ternary.cond, out);
        fn_collect_walk(n->as.ternary.then, out);
        fn_collect_walk(n->as.ternary.els,  out);
        return;
    case N_MATCH:
        fn_collect_walk(n->as.matchn.scrutinee, out);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            fn_collect_walk(n->as.matchn.arms[i].guard, out);
            fn_collect_walk(n->as.matchn.arms[i].body,  out);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            fn_collect_walk(n->as.ctor.args[i], out);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            fn_collect_walk(n->as.list.items[i], out);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            fn_collect_walk(n->as.map.vals[i], out);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            fn_collect_walk(n->as.server.assigns[i]->as.assign.value, out);
        return;
    case N_ROUTE:      fn_collect_walk(n->as.route.handler, out); return;
    case N_TOOL:
        fn_collect_walk(n->as.tool.params,  out);
        fn_collect_walk(n->as.tool.handler, out);
        return;
    case N_VERBS:      fn_collect_walk(n->as.verbs.methods, out); return;
    default: return;   /* literals, imports, type/enum decls, break/continue */
    }
}

int lume_collect_funcs(Node *prog, Node **out, int cap)
{
    if (!prog || prog->type != N_PROGRAM) return 0;
    NodeVec v = {0};
    fn_collect_walk(prog, &v);
    int n = v.n;
    if (out)
        for (int i = 0; i < n && i < cap; i++) out[i] = v.v[i];
    free(v.v);
    return n;
}

/* Rename every call that spells `from` into `to`. The walk is total - it
 * descends into lambda bodies and nested function bodies - because a local
 * is visible throughout its enclosing function once hoisted, and two locals
 * in one enclosing function cannot share a name (diagnosed below), so no
 * inner declaration can shadow the name mid-walk. */

static void lf_rename_calls(Node *n, const char *from, const char *to)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        lf_rename_calls(n->as.func.body, from, to);
        return;
    case N_FUNC_LIT:
        lf_rename_calls(n->as.funclit.body, from, to);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            lf_rename_calls(n->as.block.stmts[i], from, to);
        return;
    case N_LET:        lf_rename_calls(n->as.let.init, from, to); return;
    case N_LET_PAT:    lf_rename_calls(n->as.letpat.init, from, to); return;
    case N_LET_EXPR:
        lf_rename_calls(n->as.letx.init, from, to);
        lf_rename_calls(n->as.letx.body, from, to);
        return;
    case N_IF:
        lf_rename_calls(n->as.ifs.cond, from, to);
        lf_rename_calls(n->as.ifs.then, from, to);
        lf_rename_calls(n->as.ifs.els,  from, to);
        return;
    case N_WHILE:
        lf_rename_calls(n->as.whiles.cond, from, to);
        lf_rename_calls(n->as.whiles.body, from, to);
        return;
    case N_FOR:
        lf_rename_calls(n->as.fors.init,     from, to);
        lf_rename_calls(n->as.fors.cond,     from, to);
        lf_rename_calls(n->as.fors.incr,     from, to);
        lf_rename_calls(n->as.fors.iterable, from, to);
        lf_rename_calls(n->as.fors.body,     from, to);
        return;
    case N_RETURN:     lf_rename_calls(n->as.ret.expr, from, to); return;
    case N_EXPR_STMT:  lf_rename_calls(n->as.expr_stmt.expr, from, to); return;
    case N_ASSIGN:     lf_rename_calls(n->as.assign.value, from, to); return;
    case N_ASSIGN_MEMBER:
        lf_rename_calls(n->as.assign_mem.obj,   from, to);
        lf_rename_calls(n->as.assign_mem.value, from, to);
        return;
    case N_CALL: {
        Node *ce = n->as.call.callee;
        if (ce && ce->type == N_VAR && strcmp(ce->as.var.name, from) == 0) {
            /* The old name is arena memory (TODO.md P2) — dropping the free
             * here mirrors codegen_sig.c: stamp the renamed callee straight
             * over it, nothing to reclaim mid-walk. */
            ce->as.var.name = arena_strdup(to);
        }
        lf_rename_calls(ce, from, to);
        for (int i = 0; i < n->as.call.argc; i++)
            lf_rename_calls(n->as.call.args[i], from, to);
        return;
    }
    case N_MEMBER:     lf_rename_calls(n->as.member.obj, from, to); return;
    case N_INDEX:
        lf_rename_calls(n->as.index.obj,   from, to);
        lf_rename_calls(n->as.index.index, from, to);
        return;
    case N_UNARY:      lf_rename_calls(n->as.unary.operand, from, to); return;
    case N_BINARY:
        lf_rename_calls(n->as.binary.left,  from, to);
        lf_rename_calls(n->as.binary.right, from, to);
        return;
    case N_TERNARY:
        lf_rename_calls(n->as.ternary.cond, from, to);
        lf_rename_calls(n->as.ternary.then, from, to);
        lf_rename_calls(n->as.ternary.els,  from, to);
        return;
    case N_MATCH:
        lf_rename_calls(n->as.matchn.scrutinee, from, to);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            lf_rename_calls(n->as.matchn.arms[i].guard, from, to);
            lf_rename_calls(n->as.matchn.arms[i].body,  from, to);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            lf_rename_calls(n->as.ctor.args[i], from, to);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            lf_rename_calls(n->as.list.items[i], from, to);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            lf_rename_calls(n->as.map.vals[i], from, to);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            lf_rename_calls(n->as.server.assigns[i]->as.assign.value, from, to);
        return;
    case N_ROUTE:      lf_rename_calls(n->as.route.handler, from, to); return;
    case N_TOOL:
        lf_rename_calls(n->as.tool.params,  from, to);
        lf_rename_calls(n->as.tool.handler, from, to);
        return;
    case N_VERBS:      lf_rename_calls(n->as.verbs.methods, from, to); return;
    default: return;
    }
}

/* The local functions declared directly inside one body: statement level
 * plus any if/while/for/block below it. A nested function's own body is NOT
 * descended into here - its locals are a deeper scope, walked separately by
 * lf_walk_fn_body. Lambda bodies ARE descended into: a `func` inside a
 * lambda body belongs to the enclosing function's scope and the rename
 * reaches lambda bodies too, so the two walks agree. */

static void lf_scan_locals(Node *n, NodeVec *out)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        nv_push(out, n);            /* collected, not descended into */
        return;
    case N_FUNC_LIT:
        lf_scan_locals(n->as.funclit.body, out);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            lf_scan_locals(n->as.block.stmts[i], out);
        return;
    case N_LET:        lf_scan_locals(n->as.let.init, out); return;
    case N_LET_PAT:    lf_scan_locals(n->as.letpat.init, out); return;
    case N_LET_EXPR:
        lf_scan_locals(n->as.letx.init, out);
        lf_scan_locals(n->as.letx.body, out);
        return;
    case N_IF:
        lf_scan_locals(n->as.ifs.cond, out);
        lf_scan_locals(n->as.ifs.then, out);
        lf_scan_locals(n->as.ifs.els,  out);
        return;
    case N_WHILE:
        lf_scan_locals(n->as.whiles.cond, out);
        lf_scan_locals(n->as.whiles.body, out);
        return;
    case N_FOR:
        lf_scan_locals(n->as.fors.init,     out);
        lf_scan_locals(n->as.fors.cond,     out);
        lf_scan_locals(n->as.fors.incr,     out);
        lf_scan_locals(n->as.fors.iterable, out);
        lf_scan_locals(n->as.fors.body,     out);
        return;
    case N_RETURN:     lf_scan_locals(n->as.ret.expr, out); return;
    case N_EXPR_STMT:  lf_scan_locals(n->as.expr_stmt.expr, out); return;
    case N_ASSIGN:     lf_scan_locals(n->as.assign.value, out); return;
    case N_ASSIGN_MEMBER:
        lf_scan_locals(n->as.assign_mem.obj,   out);
        lf_scan_locals(n->as.assign_mem.value, out);
        return;
    case N_CALL:
        lf_scan_locals(n->as.call.callee, out);
        for (int i = 0; i < n->as.call.argc; i++)
            lf_scan_locals(n->as.call.args[i], out);
        return;
    case N_MEMBER:     lf_scan_locals(n->as.member.obj, out); return;
    case N_INDEX:
        lf_scan_locals(n->as.index.obj,   out);
        lf_scan_locals(n->as.index.index, out);
        return;
    case N_UNARY:      lf_scan_locals(n->as.unary.operand, out); return;
    case N_BINARY:
        lf_scan_locals(n->as.binary.left,  out);
        lf_scan_locals(n->as.binary.right, out);
        return;
    case N_TERNARY:
        lf_scan_locals(n->as.ternary.cond, out);
        lf_scan_locals(n->as.ternary.then, out);
        lf_scan_locals(n->as.ternary.els,  out);
        return;
    case N_MATCH:
        lf_scan_locals(n->as.matchn.scrutinee, out);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            lf_scan_locals(n->as.matchn.arms[i].guard, out);
            lf_scan_locals(n->as.matchn.arms[i].body,  out);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            lf_scan_locals(n->as.ctor.args[i], out);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            lf_scan_locals(n->as.list.items[i], out);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            lf_scan_locals(n->as.map.vals[i], out);
        return;
    default: return;
    }
}

static void lf_fail(LfCtx *c, Node *n, const char *fmt, ...)
{
    if (c->failed) return;
    c->failed = true;
    if (!c->err || !c->err_size) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(c->err, c->err_size, "line %zu: %s", n ? n->line : 0, msg);
}

/* Name, stamp and rewrite the locals of one body, then recurse into each
 * local's own body. Idempotent: a body whose locals all carry a cname is
 * skipped wholesale, so the checker's run and any later guard's run do no
 * double work. */

static void lf_walk_fn_body(LfCtx *c, Node *body)
{
    if (!body || c->failed) return;

    NodeVec v = {0};
    lf_scan_locals(body, &v);
    if (v.n == 0) { free(v.v); return; }

    bool all_done = true;
    for (int i = 0; i < v.n; i++)
        if (!v.v[i]->as.func.cname) { all_done = false; break; }
    if (all_done) { free(v.v); return; }

    /* Duplicate source names inside one enclosing function are rejected
     * before anything is stamped, so a failed program never leaves a
     * half-renamed tree behind. */
    for (int i = 0; i < v.n && !c->failed; i++)
        for (int j = i + 1; j < v.n && !c->failed; j++)
            if (!v.v[i]->as.func.cname && !v.v[j]->as.func.cname &&
                strcmp(v.v[i]->as.func.name, v.v[j]->as.func.name) == 0)
                lf_fail(c, v.v[j],
                        "local function '%s' is declared twice in the same "
                        "function; local function names must be unique "
                        "within their enclosing function",
                        v.v[j]->as.func.name);

    for (int i = 0; i < v.n && !c->failed; i++) {
        Node *L = v.v[i];
        if (L->as.func.cname) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "__lume_lf_%d", c->counter++);
        L->as.func.cname = lf_strdup(buf);
        lf_rename_calls(body, L->as.func.name, L->as.func.cname);
    }

    for (int i = 0; i < v.n && !c->failed; i++)
        lf_walk_fn_body(c, v.v[i]->as.func.body);

    free(v.v);
}

bool lume_resolve_localfns(Node *prog, char *errbuf, size_t errbuf_size)
{
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!prog || prog->type != N_PROGRAM) return true;

    LfCtx c = {0};
    c.err      = errbuf;
    c.err_size = errbuf_size;

    NodeVec fns = {0};
    fn_collect_walk(prog, &fns);
    for (int i = 0; i < fns.n && !c.failed; i++)
        lf_walk_fn_body(&c, fns.v[i]->as.func.body);
    free(fns.v);

    return !c.failed;
}
