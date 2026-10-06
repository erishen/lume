/* Windows stand-ins for the builtins that live in slices this build excludes.
 *
 * The Makefile's $(IS_WINDOWS) branch drops three slices because their
 * implementations need agent-httpd (which has no Windows support) or raw
 * BSD sockets:
 *
 *   lang/builtins_sql.c   - db_query_json / db_write_exec live in
 *                           libagenthttpd.a, so the whole slice needs the library.
 *   lang/builtins_http.c  - bare BSD sockets + optional libssl (no Winsock port).
 *   lang/iquest.c         - registers product API routes via agenthttpd_route and
 *                           parses them back with minijson's reader half.
 *
 * But lang/builtins.c still *registers* sql_query/sql_write/http_get/http_post/
 * http_put/http_patch/http_delete unconditionally, so the symbols have to
 * exist. Deleting the registrations would fork the language surface between
 * platforms — a script that type-checks on Windows would then fail on macOS
 * for a reason that has nothing to do with the script.
 *
 * So the names stay, and calling one is an explicit runtime error naming the
 * reason. Error wording follows builtins_http.c's existing style ("%s(): ...").
 */

#include "builtins_internal.h"

/* sql_query / sql_write ride on agent-httpd's SQLite layer. */
static void sql_unavailable(VM *vm, const char *name) {
    vm_set_error(vm, "%s(): needs the agent-httpd host build, which is POSIX-only "
                     "(no SQLite layer in the Windows build)", name);
}

void native_sql_query(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    sql_unavailable(vm, "sql_query");
    *out = val_null();
}

void native_sql_write(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    sql_unavailable(vm, "sql_write");
    *out = val_null();
}

/* http_get / http_post / http_put / http_patch / http_delete need sockets.
 * LUME_HAS_HTTP is forced to 0 on Windows, so builtins_http.c is not compiled
 * and there is no transport at all here — not even a cleartext one. */
static void http_unavailable(VM *vm, const char *name) {
    vm_set_error(vm, "%s(): outbound HTTP is not available in the Windows build "
                     "(no Winsock transport; LUME_HAS_HTTP=0)", name);
}

#define HTTP_STUB(fn, label)                                  \
    void fn(VM *vm, int argc, Value *args, Value *out) {     \
        (void)argc; (void)args;                               \
        http_unavailable(vm, label);                          \
        *out = val_null();                                    \
    }

HTTP_STUB(native_http_get,    "http_get")
HTTP_STUB(native_http_post,   "http_post")
HTTP_STUB(native_http_put,    "http_put")
HTTP_STUB(native_http_patch,  "http_patch")
HTTP_STUB(native_http_delete, "http_delete")