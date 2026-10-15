/* Windows stand-ins for the builtins that live in slices this build excludes.
 *
 * The Makefile's Windows branch drops three slices because their
 * implementations need agent-httpd (which has no Windows support) or raw
 * BSD sockets:
 *
 *   lang/builtins_sql.c   - db_query_json / db_write_exec live in
 *                           libagenthttpd.a, so the whole slice needs the library.
 *   lang/builtins_http.c  - bare BSD sockets + optional libssl (no Winsock port).
 *   lang/iquest.c         - registers product API routes via agenthttpd_route and
 *                           parses them back with minijson's reader half.
 *
 * lang/builtins.c still *registers* sql_query/sql_write unconditionally, so
 * those two symbols have to exist: deleting the registrations would fork the
 * language surface between platforms — a script that type-checks on Windows
 * would then fail on macOS for a reason that has nothing to do with the
 * script. The names stay, and calling one is an explicit runtime error
 * naming the reason.
 *
 * The five native_http_* names used to be stubbed here too, but upstream
 * builtins.c now defines its own unavailable fallbacks under
 * `#if !LUME_HAS_HTTP` (the Windows build forces LUME_HAS_HTTP=0), so this
 * file defining them again is a multiple-definition link error. That block
 * in builtins.c is the single source of the http stubs now; only the sql
 * stand-ins remain here because builtins.c has no sql fallback (the real
 * implementations live behind libagenthttpd, not behind LUME_HAS_HTTP).
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