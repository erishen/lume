#include "lume.h"
#include "iquest.h"
#include "minijson.h"
#include "llm.h"
#include <stdlib.h> /* getenv(LUME_BIND) */

/* bridge.c — the language's runtime translated into libagenthttpd.a calls.
 *
 *   server { ... }           -> agenthttpd_config fields
 *   route "M", "p", fn       -> agenthttpd_route (C shim executes the DSL fn)
 *   tool "n", "d", {...}, fn -> agenthttpd_tool (shim runs fn on tool calls)
 *   run();                   -> agenthttpd_run() (blocks the process)
 *
 * Requests, results and tool arguments cross the C <-> DSL boundary as
 * JSON-ish maps. The shims run on the master's fast loop (GET/HEAD) or on a
 * prefork worker (everything else); each worker inherits its own VM copy at
 * fork time, so one VM instance per process is exactly the concurrency model
 * the embed library assumes. */

static VM *g_vm = NULL; /* single instance per process (single-instance lib) */

/* Reset per-request interpreter state: an error is sticky (stops the whole
 * program) during top-level setup, but a shim handles many requests in one
 * worker and must recover between them. */
static void vm_after_request(VM *vm) {
    vm->error = false;
    vm->error_msg[0] = '\0';
    vm->stack_count = 0;
    /* leave call_result alone — it is re-initialized per function call */
    vm->call_result = val_null();
}

/* ---------- request <-> DSL map ---------- */

static Value request_to_value(VM *vm, const HttpRequest *req, const char *label) {
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)m)); /* root while filling */

    if (label) map_set(vm, m, "label", make_string_cstr(vm, label));
    map_set(vm, m, "method", make_string_cstr(vm, req->method));
    map_set(vm, m, "path", make_string_cstr(vm, req->path));
    map_set(vm, m, "remote_addr",
            req->remote_addr[0] ? make_string_cstr(vm, req->remote_addr)
                                : val_null());
    map_set(vm, m, "host", req->host[0] ? make_string_cstr(vm, req->host)
                                        : val_null());
    map_set(vm, m, "content_type",
            req->content_type[0] ? make_string_cstr(vm, req->content_type)
                                 : val_null());
    map_set(vm, m, "content_length", val_int(req->content_length));
    map_set(vm, m, "user_agent",
            req->user_agent[0] ? make_string_cstr(vm, req->user_agent)
                               : val_null());
    map_set(vm, m, "body",
            req->body ? make_string(vm, req->body, strlen(req->body)) : val_null());

    return val_obj((Obj *)m); /* still rooted on the stack */
}

static bool map_str(VM *vm, Obj *m, const char *key, const char **out,
                    const char **defval) {
    int found = 0;
    Value v = map_get(vm, m, key, &found);
    if (!found || IS_NULL(v)) { *out = defval ? *defval : NULL; return true; }
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) {
        vm_set_error(vm, "route result field '%s' must be a string", key);
        return false;
    }
    *out = obj_string(AS_OBJ(v));
    return true;
}

static bool map_int(VM *vm, Obj *m, const char *key, int *out, int defval) {
    int found = 0;
    Value v = map_get(vm, m, key, &found);
    if (!found || IS_NULL(v)) { *out = defval; return true; }
    if (!IS_NUM(v)) {
        vm_set_error(vm, "route result field '%s' must be a number", key);
        return false;
    }
    *out = (int)AS_NUM(v);
    return true;
}

/* Reason phrase for the status line (common codes; default OK). */
static const char *status_text_for(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

/* JSON-encode a value into a malloc'd NUL-terminated string (caller frees). */
static char *value_to_json(VM *vm, Value v, size_t *len) {
    sbuf b = {0};
    json_append_value(vm, &b, v);
    *len = b.len;
    char *out = malloc(b.len + 1);
    if (out) {
        memcpy(out, b.p ? b.p : "", b.len);
        out[b.len] = '\0';
    }
    free(b.p);
    return out;
}

/* Translate a handler return value into an HttpResponse. Ergonomics:
 *   - a bare string            -> 200 text/html
 *   - { status?, type?, body } -> envelope; body auto-JSON if it isn't a string
 *   - any other map            -> 200 application/json, map is the payload
 *   - list/number/bool/null    -> 200 application/json, JSON-encoded payload
 * So `return { message: "hi" };` and `return { status: 201, body: {...} };`
 * need no explicit stringify()/type. */
static void result_to_response(VM *vm, Value result, HttpResponse *res) {
    if (vm->error) {
        set_error_response(res, 500, "Lume handler error");
        return;
    }
    int status = 200;
    const char *mime = NULL;
    char *owned_body = NULL;
    const char *body = NULL;
    size_t body_len = 0;

    if (IS_OBJ(result) && AS_OBJ(result)->type == OBJ_STRING) {
        body = obj_string(AS_OBJ(result));
        body_len = obj_string_len(AS_OBJ(result));
        mime = "text/html";
    } else if (IS_OBJ(result) && AS_OBJ(result)->type == OBJ_MAP) {
        Obj *m = AS_OBJ(result);
        int has_body = 0;
        (void)map_get(vm, m, "body", &has_body);
        if (has_body) {
            int found = 0;
            Value bv = map_get(vm, m, "body", &found);
            const char *explicit_type = NULL;
            if (!map_int(vm, m, "status", &status, 200)) goto fail;
            if (!map_str(vm, m, "type", &explicit_type, NULL)) goto fail;
            if (IS_OBJ(bv) && AS_OBJ(bv)->type == OBJ_STRING) {
                body = obj_string(AS_OBJ(bv));
                body_len = obj_string_len(AS_OBJ(bv));
                mime = explicit_type ? explicit_type : "text/html";
            } else {
                owned_body = value_to_json(vm, bv, &body_len);
                body = owned_body;
                mime = explicit_type ? explicit_type : "application/json";
            }
        } else {
            owned_body = value_to_json(vm, result, &body_len);
            body = owned_body;
            mime = "application/json";
        }
    } else {
        owned_body = value_to_json(vm, result, &body_len);
        body = owned_body;
        mime = "application/json";
    }

    if (body) {
        res->body = malloc(body_len + 1);
        memcpy(res->body, body, body_len);
        res->body[body_len] = '\0';
        res->body_length = (int)body_len;
    }
    res->status_code = status;
    snprintf(res->status_text, sizeof(res->status_text), "%s",
             status_text_for(status));
    if (mime) snprintf(res->content_type, sizeof(res->content_type), "%s", mime);
    free(owned_body);
    return;

fail:
    free(owned_body);
    set_error_response(res, 500, "Lume handler error");
    return;
}

/* ---------- route shim ---------- */

static int route_pattern_match(const char *pattern, const char *path) {
    size_t n = strlen(pattern);
    if (n && pattern[n - 1] == '*')
        return strncmp(pattern, path, n - 1) == 0;
    return strcmp(pattern, path) == 0;
}

/* C callback registered via agenthttpd_route. A single shim serves every DSL
 * route; it finds its handler by matching (method, path) against the table. */
static int route_shim(HttpRequest *req, HttpResponse *res) {
    VM *vm = g_vm;
    for (int i = 0; i < vm->route_count; i++) {
        RouteRec *r = &vm->routes[i];
        if (strcmp(r->method, req->method) != 0) continue;
        if (!route_pattern_match(r->path, req->path)) continue;

        request_to_value(vm, req, r->label);   /* request map pushed */
        Value argval = vm_pop(vm);
        vm_push(vm, r->handler);     /* callee first, then args */
        vm_push(vm, argval);         /* [callee, arg] */
        call_function(vm, r->handler, 1);
        /* call_function replaced [callee,arg] with the single result */
        Value result = vm_peek(vm, 0);

        if (!IS_OBJ(result) ||
            (AS_OBJ(result)->type != OBJ_MAP && AS_OBJ(result)->type != OBJ_STRING)) {
            if (!vm->error)
                vm_set_error(vm, "route handler for %s %s returned a bad value",
                             req->method, req->path);
        }
        result_to_response(vm, result, res);
        vm_pop(vm);
        vm_after_request(vm);
        /* Return 0 = handled; the framework serializes the filled response.
         * Do NOT set res->handled: that flag signals a self-streaming handler
         * and makes http.c skip response serialization entirely. */
        return 0;
    }
    return -1; /* fall through to the default dispatch */
}

int bridge_define_route(VM *vm, const char *method, const char *path, Value handler,
                        const char *label) {
    if (vm->route_count >= MAX_AL_ROUTES) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    RouteRec *r = &vm->routes[vm->route_count++];
    r->method = strdup(method);
    r->path = strdup(path);
    r->label = label ? strdup(label) : NULL;
    r->handler = handler;

    if (agenthttpd_route(method, path, route_shim) != 0) {
        vm->route_count--; /* registration refused; roll back */
        return -1;
    }
    return 0;
}

/* ---------- tool shim ---------- */

typedef struct {
    VM *vm;
    int index;
} ToolCtx;

/* The DSL registers tool params as { name: "string", ... } — property names
 * mapped straight to a bare scalar type word. The OpenAI tool schema that
 * agent-httpd relays upstream requires each property to be a *schema object*
 * ({"type":"string"}); strict gateways (e.g. the agnes provider behind the
 * demo llm-router) reject the bare form with HTTP 400, while lenient ones
 * (sensenova) silently accept it — hence the intermittent upstream 400.
 * Upgrade at registration time so the emitted schema is valid everywhere:
 *   {"name":"string","age":"int"}   -> {"name":{"type":"string"},...}
 *   {"name":{"type":"string"}}      -> unchanged (already structured)
 * Malformed input passes through untouched so registration still works. */
static void upgrade_tool_params(const char *in, sbuf *out) {
    if (!in || !*in) in = "{}";
    sbuf tmp = {0};
    const char *p = jws(in);
    if (*p != '{') { sb_str(out, in); return; }
    p++;
    sb_chr(&tmp, '{');
    int first = 1;
    for (;;) {
        p = jws(p);
        if (*p == ',') p = jws(p + 1); /* advance past a pair separator */
        if (*p == '}' || *p == '\0') { sb_chr(&tmp, '}'); break; }
        if (*p != '"') goto malformed;
        const char *key_start = p;
        do { p++; } while (*p && *p != '"');
        if (!*p) goto malformed;
        p++; /* past the closing quote */
        const char *key_end = p;
        p = jws(p);
        if (*p != ':') goto malformed;
        p = jws(p + 1);

        const char *val_start = p;
        size_t val_len;
        if (*p == '{' || *p == '[') {
            /* structured (possibly already a schema object): copy verbatim */
            int depth = 0;
            do {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') depth--;
                p++;
            } while (depth > 0 && *p);
            val_len = (size_t)(p - val_start);
        } else if (*p == '"') {
            /* bare scalar type word: wrap into {"type":<word>} */
            do { p++; } while (*p && *p != '"');
            p++;
            val_len = (size_t)(p - val_start);
            if (!first) sb_chr(&tmp, ',');
            first = 0;
            sb_mem(&tmp, key_start, (size_t)(key_end - key_start));
            sb_chr(&tmp, ':');
            sb_str(&tmp, "{\"type\":");
            sb_mem(&tmp, val_start, val_len);
            sb_chr(&tmp, '}');
            continue;
        } else if (!*p || *p == '}') {
            goto malformed;
        } else {
            /* number / bool / raw token: copy untouched */
            do { p++; } while (*p && *p != ',' && *p != '}');
            val_len = (size_t)(p - val_start);
        }
        if (!first) sb_chr(&tmp, ',');
        first = 0;
        sb_mem(&tmp, key_start, (size_t)(key_end - key_start));
        sb_chr(&tmp, ':');
        sb_mem(&tmp, val_start, val_len);
    }
    sb_str(out, tmp.p ? tmp.p : "{}");
    free(tmp.p);
    return;
malformed:
    free(tmp.p);
    sb_str(out, in);
}

/* C callback registered via agenthttpd_tool. Args arrive as a raw JSON
 * object; the DSL handler receives it as a map and its return value is JSON-
 * encoded into the tool result buffer. */
static void tool_shim(void *data, const char *args_json,
                      const char *session_id, sbuf *out) {
    (void)session_id;
    ToolCtx *ctx = data;
    VM *vm = ctx->vm;
    if (ctx->index < 0 || ctx->index >= vm->tool_count) return;

    Value handler = vm->tool_records[ctx->index].handler;

    Value args = val_null();
    if (args_json && args_json[0] && strcmp(args_json, "{}") != 0) {
        char err[256] = {0};
        json_parse(vm, args_json, err, sizeof(err));
        if (vm->error) {
            sb_str(out, "tool arguments failed to parse");
            vm_after_request(vm);
            return;
        }
        args = vm_pop(vm); /* json result, now a plain local */
    } else {
        vm_push(vm, make_map(vm)); /* empty args: root it immediately */
        args = vm_pop(vm);
    }

    vm_push(vm, handler);  /* callee first, then the single arg */
    vm_push(vm, args);     /* args is now rooted: allocations below are safe */
    if (session_id && session_id[0]) {
        /* conversation id for the agent loop -> DSL side, as `session` */
        map_set(vm, AS_OBJ(args), "session", make_string_cstr(vm, session_id));
    }
    call_function(vm, handler, 1);
    Value result = vm_peek(vm, 0);

    if (vm->error) {
        sb_str(out, vm->error_msg);
    } else {
        json_append_value(vm, out, result);
    }
    vm_pop(vm);
    vm_after_request(vm);
}

int bridge_define_tool(VM *vm, const char *name, const char *desc,
                       const char *params_json, Value handler) {
    if (vm->tool_count >= MAX_AL_TOOLS) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    ToolRec *rec = &vm->tool_records[vm->tool_count];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    snprintf(rec->desc, sizeof(rec->desc), "%s", desc);
    sbuf schema = {0};
    upgrade_tool_params(params_json, &schema);
    const char *final_params =
        schema.p ? schema.p : (params_json ? params_json : "{}");
    snprintf(rec->params, sizeof(rec->params), "%s", final_params);
    rec->handler = handler;

    ToolCtx *ctx = malloc(sizeof(ToolCtx));
    ctx->vm = vm;
    ctx->index = vm->tool_count;

    /* Profile allow-list (HARNESS_TOOLS_ALLOW): excluding a demo tool is a
     * trim, not a load error — the rest of the example keeps running. */
    if (!agenthttpd_tool_allowed(name)) {
        fprintf(stderr, "[tools] DSL skip (profile) %s\n", name);
        free(ctx);
        free(schema.p);
        return 0;
    }

    int rc = agenthttpd_tool(name, desc, rec->params, tool_shim, ctx);
    free(schema.p);
    if (rc != 0) {
        free(ctx);
        return -1;
    }
    vm->tool_count++;
    return 0;
}

/* ---------- run(): config -> agenthttpd_run ---------- */

/* Environment fallbacks: a .lume `server { ... }` literal always wins; when a
 * field is omitted we consult the process env (which llm_env_init() fills from
 * a CWD .env); finally the framework default applies. */
static int env_int_or(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return dflt; /* not numeric */
    return (int)n;
}

static const char *env_str_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

void bridge_run(VM *vm) {
    llm_env_init(); /* load CWD .env before reading PORT/WORKERS/DOCROOT/LLM_* */

    agenthttpd_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* env fallbacks; overridden by server{...} below, 0/NULL = framework default */
    cfg.port = env_int_or("PORT", 0);
    cfg.workers = env_int_or("WORKERS", 0);
    cfg.docroot = env_str_or("DOCROOT", NULL);

    if (vm->server_config) {
        Obj *m = vm->server_config;
        /* numbered fields */
        int found = 0;
        Value v = map_get(vm, m, "port", &found);
        if (found && IS_NUM(v)) cfg.port = (int)AS_NUM(v);
        v = map_get(vm, m, "workers", &found);
        if (found && IS_NUM(v)) cfg.workers = (int)AS_NUM(v);
        v = map_get(vm, m, "rate_limit_rps", &found);
        if (found && IS_NUM(v)) cfg.rate_limit_rps = (int)AS_NUM(v);
        v = map_get(vm, m, "vite_upstream_port", &found);
        if (found && IS_NUM(v)) cfg.vite_upstream_port = (int)AS_NUM(v);
        v = map_get(vm, m, "no_directory_listing", &found);
        if (found && value_truthy(v)) cfg.no_directory_listing = 1;

        /* string fields are strdup'd: agenthttpd_run blocks for the process
         * lifetime, so these intentionally leak. */
        struct { const char *key; const char **dst; } strs[] = {
            {"bind", &cfg.bind_host},
            {"docroot", &cfg.docroot},
            {"views", &cfg.views},
            {"cgi_bin", &cfg.cgi_bin},
            {"access_log", &cfg.access_log},
            {"htpasswd", &cfg.htpasswd},
            {"auth_realm", &cfg.auth_realm},
            {"fcgi_socket", &cfg.fcgi_socket},
            {"react_socket", &cfg.react_socket},
        };
        for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
            v = map_get(vm, m, strs[i].key, &found);
            if (found && IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
                const char *s = obj_string(AS_OBJ(v));
                char *copy = strdup(s);
                if (copy) *strs[i].dst = copy;
            }
        }

        /* LUME_BIND 环境变量覆盖脚本里的 bind: 本地默认 127.0.0.1 是安全
         * 基线(防止裸跑 invest.lume 时对网卡全接口暴露), 容器编排场景
         * (k8s NodePort 从 pod 网络栈访问 pod IP) 需要监听 0.0.0.0,
         * 暴露面由编排层(Namespace/NodePort 绑定)控制。 */
        const char *env_bind = getenv("LUME_BIND");
        if (env_bind && env_bind[0]) {
            char *copy = strdup(env_bind);
            if (copy) cfg.bind_host = copy;
        }
    }

    vm->run_called = true;
    iquest_register(); /* 产品 API 路由(POST /api/settings 等),fork 前注册 */
    printf("[lume] run(): port=%d workers=%d docroot=%s\n",
           cfg.port > 0 ? cfg.port : DEFAULT_PORT,
           cfg.workers, /* 0 = fork-per-connection (framework default) */
           cfg.docroot ? cfg.docroot : "./www");
    fflush(stdout);

    agenthttpd_run(&cfg);
}

void bridge_init(VM *vm) { g_vm = vm; }