/* Windows stand-in for the host tree's lang/bridge.c.
 *
 * bridge.c is the host's agent-httpd embedding layer: it translates the DSL's
 * `route {}` / `tool {}` / `server {}` constructs into agenthttpd_route() /
 * agenthttpd_tool() / agenthttpd_run() and wires HttpRequest into DSL values.
 * agent-httpd has no Windows support (its 30 .c files are POSIX socket code
 * with zero _WIN32 guards), and lume's bin/lume links libagenthttpd.a
 * directly, so on Windows this file replaces bridge.c and the static library
 * is left out of the link entirely. See the Makefile's $(IS_WINDOWS) branch.
 *
 * What still works, and is deliberately not faked:
 *
 *   - bridge_define_route() / bridge_define_tool() record into the VM's own
 *     tables (RouteRec routes[] / ToolRec tool_records[] already exist and
 *     hold the handler as a GC root). `route {}` / `tool {}` keep meaning
 *     "register" — they just no longer mean "reachable from an HTTP request",
 *     because this build has no server to serve them.
 *   - bridge_run() refuses loudly. The `run` builtin calls it; a binary that
 *     silently "succeeds" at starting a server it does not have is worse than
 *     one that says no, so this exits non-zero.
 *
 * Signatures must match lang/lume.h's bridge_* declarations exactly.
 */

#include "lume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bridge_init(VM *vm)
{
    /* The host bound a file-static VM pointer here because agenthttpd's C
     * callbacks had no per-call VM. Everything below takes vm explicitly and
     * there is no callback table to keep in sync, so there is nothing to do. */
    (void)vm;
}

int bridge_define_route(VM *vm, const char *method, const char *path,
                        Value handler, const char *label)
{
    if (vm->route_count >= MAX_AL_ROUTES) return -1;
    if (!method || !path) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    RouteRec *r = &vm->routes[vm->route_count++];
    r->method = strdup(method);
    r->path = strdup(path);
    r->label = label ? strdup(label) : NULL;
    r->handler = handler;
    return 0;
}

int bridge_define_tool(VM *vm, const char *name, const char *desc,
                       const char *params_json, Value handler)
{
    if (vm->tool_count >= MAX_AL_TOOLS) return -1;
    if (!name) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    ToolRec *rec = &vm->tool_records[vm->tool_count];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    snprintf(rec->desc, sizeof(rec->desc), "%s", desc ? desc : "");
    snprintf(rec->params, sizeof(rec->params), "%s",
             params_json ? params_json : "{}");
    rec->handler = handler;
    vm->tool_count++;
    return 0;
}

void bridge_run(VM *vm)
{
    (void)vm;
    fprintf(stderr,
            "lume: run() needs the agent-httpd host build, which is POSIX-only.\n"
            "      This Windows build registers routes and tools but serves\n"
            "      nothing. Use --check / --dump, or run the script directly.\n");
    exit(2);
}