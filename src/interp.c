#include "lume.h"
#include "minijson.h"
#include "tools.h"
#include "skills.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

/* Tree-walk interpreter (the "Runtime"). Every expression evaluation leaves
 * exactly ONE value on the VM's value stack, so partial results are always
 * GC roots. User-function calls push a jmp_buf; `return` longjmps to it.
 * The bridge (bridge.c) reuses the same call machinery for HTTP routes and
 * agent tool callbacks. */

static void exec_statement(VM *vm, Node *n, Env *env);
static void exec_block_walk(VM *vm, Node *block, Env *env);

/* ---------- VM plumbing ---------- */

void vm_set_error(VM *vm, const char *fmt, ...) {
    if (vm->error) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm->error = true;
}

void vm_push(VM *vm, Value v) {
    if (vm->stack_count >= AL_STACK_MAX) {
        vm_set_error(vm, "value stack overflow (max %d)", AL_STACK_MAX);
        return;
    }
    vm->stack[vm->stack_count++] = v;
}

Value vm_pop(VM *vm) {
    if (vm->stack_count <= 0) return val_null(); /* defensive underflow */
    return vm->stack[--vm->stack_count];
}

Value vm_peek(VM *vm, int depth) {
    if (vm->stack_count - 1 - depth < 0) return val_null();
    return vm->stack[vm->stack_count - 1 - depth];
}

/* ---------- string literal unescaping ---------- */

static char *unescape_literal(const char *s, int n, int *out_len) {
    /* s spans the raw lexeme INCLUDING the surrounding quotes. */
    char *buf = malloc((size_t)n + 1);
    int w = 0;
    for (int i = 1; i < n - 1; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n - 1) {
            i++;
            switch (s[i]) {
                case 'n': buf[w++] = '\n'; break;
                case 't': buf[w++] = '\t'; break;
                case 'r': buf[w++] = '\r'; break;
                case '0': buf[w++] = '\0'; break;
                case '\\': buf[w++] = '\\'; break;
                case '"': buf[w++] = '"'; break;
                default: buf[w++] = s[i]; break;
            }
        } else {
            buf[w++] = c;
        }
    }
    buf[w] = '\0';
    *out_len = w;
    return buf;
}

/* ---------- native builtins ---------- */

static bool arg_string(VM *vm, Value v, const char **out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) {
        vm_set_error(vm, "expected string argument");
        return false;
    }
    *out = obj_string(AS_OBJ(v));
    return true;
}

static void native_print(VM *vm, int argc, Value *args, Value *out) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        if (IS_OBJ(args[i]) && AS_OBJ(args[i])->type == OBJ_STRING) {
            fwrite(obj_string(AS_OBJ(args[i])), 1, obj_string_len(AS_OBJ(args[i])), stdout);
        } else if (IS_NUM(args[i])) {
            double d = AS_NUM(args[i]);
            if (d == (long long)d)
                printf("%lld", (long long)d);
            else
                printf("%g", d);
        } else if (IS_BOOL(args[i]) || IS_NULL(args[i])) {
            printf(IS_BOOL(args[i]) ? (AS_BOOL(args[i]) ? "true" : "false") : "null");
        } else {
            sbuf b = {0};
            json_append_value(vm, &b, args[i]);
            printf("%s", b.p ? b.p : "");
        }
    }
    printf("\n");
    fflush(stdout);
    *out = val_null();
}

static void str_of_value(VM *vm, Value v, Value *out) {
    sbuf b = {0};
    if (IS_NUM(v)) {
        double d = AS_NUM(v);
        char buf[64];
        if (d == (long long)d)
            snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            snprintf(buf, sizeof(buf), "%g", d);
        sb_str(&b, buf);
    } else if (IS_BOOL(v)) {
        sb_str(&b, AS_BOOL(v) ? "true" : "false");
    } else if (IS_NULL(v)) {
        sb_str(&b, "null");
    } else {
        json_append_value(vm, &b, v);
    }
    *out = make_string(vm, b.p ? b.p : "", b.len);
}

/* When args = (map, string-key [, default]), resolve the field (default when
   missing) into *v and return 1; otherwise return 0. Lets casts double as
   safe accessors: int(m, "a") == int(get(m, "a")). */
static int value_from_map(VM *vm, int argc, Value *args, Value *v) {
    if (argc < 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP)
        return 0;
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING)
        return 0;
    int found = 0;
    Value g = map_get(vm, AS_OBJ(args[0]), obj_string(AS_OBJ(args[1])), &found);
    *v = found ? g : (argc >= 3 ? args[2] : val_null());
    return 1;
}

static void native_str(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "str() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) { *out = v; return; }
        str_of_value(vm, v, out);
        return;
    }
    if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = args[0];
        return;
    }
    str_of_value(vm, args[0], out);
}

static void native_int(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "int() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_NUM(v))
            *out = val_int((long long)AS_NUM(v));
        else if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
            *out = val_int(atoll(obj_string(AS_OBJ(v))));
        else
            *out = val_num(0);
        return;
    }
    if (IS_NUM(args[0])) {
        *out = val_int((long long)AS_NUM(args[0]));
    } else if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = val_int(atoll(obj_string(AS_OBJ(args[0]))));
    } else {
        *out = val_num(0);
    }
}

static void native_float(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "float() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_NUM(v)) *out = val_num(AS_NUM(v));
        else if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
            *out = val_num(atof(obj_string(AS_OBJ(v))));
        else
            *out = val_num(0);
        return;
    }
    if (IS_NUM(args[0])) {
        *out = val_num(AS_NUM(args[0]));
    } else if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = val_num(atof(obj_string(AS_OBJ(args[0]))));
    } else {
        *out = val_num(0);
    }
}

static void native_bool(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "bool() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v))
        *out = val_bool(value_truthy(v));
    else
        *out = val_bool(value_truthy(args[0]));
}

static void native_len(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "len() needs an argument"); return; }
    if (IS_OBJ(args[0])) {
        Obj *o = AS_OBJ(args[0]);
        if (o->type == OBJ_STRING) { *out = val_int((long long)o->as.str.len); return; }
        if (o->type == OBJ_LIST)   { *out = val_int(o->as.list.count); return; }
        if (o->type == OBJ_MAP)    { *out = val_int(o->as.map.count); return; }
    }
    *out = val_num(0);
}

static void native_keys(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "keys() expects a map");
        return;
    }
    Obj *m = AS_OBJ(args[0]);
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root during construction */
    for (int i = 0; i < m->as.map.count; i++) {
        Value s = make_string_cstr(vm, m->as.map.keys[i]);
        vm_push(vm, s);
        if (list->as.list.count == list->as.list.cap) {
            list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
            list->as.list.items = realloc(list->as.list.items,
                                          sizeof(Value) * (size_t)list->as.list.cap);
        }
        list->as.list.items[list->as.list.count++] = s;
        vm_pop(vm);
    }
    vm_pop(vm);
    *out = val_obj((Obj *)list);
}

static void native_get(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !IS_OBJ(args[0])) {
        vm_set_error(vm, "get() expects a map or list and a key");
        return;
    }
    Obj *o = AS_OBJ(args[0]);
    if (o->type == OBJ_MAP) {
        const char *key = NULL;
        if (!arg_string(vm, args[1], &key)) return;
        int found = 0;
        Value v = map_get(vm, o, key, &found);
        *out = found ? v : (argc >= 3 ? args[2] : val_null());
    } else if (o->type == OBJ_LIST) {
        if (!IS_NUM(args[1])) {
            vm_set_error(vm, "list index must be a number");
            return;
        }
        int i = (int)AS_NUM(args[1]);
        *out = (i >= 0 && i < o->as.list.count) ? o->as.list.items[i]
                                                : (argc >= 3 ? args[2] : val_null());
    } else {
        vm_set_error(vm, "get() expects a map or list and a key");
    }
}

/* Default handler for handler-less routes (`write "/items";`): acknowledge the
 * request as { action: <label>, method: <method>, got: <body> }. `action` is
 * null for a route whose group carries no label. */
static void native_default_route(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "default route handler expects a request map");
        return;
    }
    Obj *req = AS_OBJ(args[0]);
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m)); /* root while filling */
    int found = 0;
    map_set(vm, m, "action", map_get(vm, req, "label", &found));
    map_set(vm, m, "method", map_get(vm, req, "method", &found));
    map_set(vm, m, "got", map_get(vm, req, "body", &found));
    *out = vm_pop(vm);
}

static void native_json(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "json() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    char err[256] = {0};
    json_parse(vm, s, err, sizeof(err));
    if (vm->error) {
        if (err[0]) vm_set_error(vm, "%s", err);
        *out = val_null();
        return;
    }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

static void native_stringify(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "stringify() needs a value"); return; }
    sbuf b = {0};
    json_append_value(vm, &b, args[0]);
    *out = make_string(vm, b.p ? b.p : "", b.len);
}

static void native_now(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    *out = val_int((long long)time(NULL));
}

/* Append v to a list, rooting it while the backing array may grow. */
static void list_push(VM *vm, Obj *list, Value v) {
    vm_push(vm, v);
    if (list->as.list.count == list->as.list.cap) {
        int nc = list->as.list.cap ? list->as.list.cap * 2 : 8;
        Value *ni = realloc(list->as.list.items, sizeof(Value) * (size_t)nc);
        if (ni) { list->as.list.items = ni; list->as.list.cap = nc; }
    }
    list->as.list.items[list->as.list.count++] = v;
    vm_pop(vm);
}

static int str_entry_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int skill_entry_cmp(const void *a, const void *b) {
    const SkillInfo *x = *(const SkillInfo *const *)a;
    const SkillInfo *y = *(const SkillInfo *const *)b;
    return strcmp(x->name, y->name);
}

static void native_env(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "env() needs a variable name"); return; }
    const char *k = NULL;
    if (!arg_string(vm, args[0], &k)) return;
    const char *v = getenv(k);
    *out = v ? make_string_cstr(vm, v) : val_null();
}

/* Sorted directory listing; directories carry a trailing "/". Missing or
 * unreadable dirs yield an empty list (a discovery page should degrade). */
static void native_files(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "files() needs a directory path"); return; }
    const char *dir = NULL;
    if (!arg_string(vm, args[0], &dir)) return;
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    DIR *d = opendir(dir);
    if (d) {
        const char *names[1024];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) && n < 1024) {
            if (e->d_name[0] == '.') continue;
            names[n++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(names, n, sizeof names[0], str_entry_cmp);
        char full[4096];
        for (int i = 0; i < n; i++) {
            struct stat st;
            int is_dir = 0;
            if (snprintf(full, sizeof full, "%s/%s", dir, names[i]) <
                (int)sizeof full) {
                is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (is_dir) {
                size_t l = strlen(names[i]);
                char *with_slash = malloc(l + 2);
                if (with_slash) {
                    memcpy(with_slash, names[i], l);
                    with_slash[l] = '/';
                    with_slash[l + 1] = '\0';
                    list_push(vm, list, make_string_cstr(vm, with_slash));
                    free(with_slash);
                }
            } else {
                list_push(vm, list, make_string_cstr(vm, names[i]));
            }
            free((void *)names[i]);
        }
    }
    *out = vm_pop(vm);
}

/* Whole file contents as a string, or null when missing/unreadable.
 * 16 MiB cap: this exists for catalog/skill inspection, not memory dumps. */
static void native_read_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "read_file() needs a path"); return; }
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    FILE *f = fopen(p, "rb");
    if (!f) { *out = val_null(); return; }
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "read_file() too large: %s", p);
            *out = val_null();
            return;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) {
        free(b.p);
        *out = val_null();
        return;
    }
    *out = make_string(vm, b.p, b.len);
    free(b.p);
}

/* Whole file write from a string (binary-safe); the settings page persists
 * the PSE approval switch into frameworks/autogen-pse/.env through this.
 * Returns true/false. Mirror of native_read_file, capped at 16 MiB.
 * NOTE: string payload lives inline after the Obj (obj_string()), NOT in
 * as.str.data — that pointer field is never set and was a "works until the
 * fopen actually succeeds" latent bug. */
static void native_write_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "write_file() needs a path and content"); return; }
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING) {
        vm_set_error(vm, "write_file() content must be a string");
        return;
    }
    const char *data = obj_string(AS_OBJ(args[1]));
    size_t len = obj_string_len(AS_OBJ(args[1]));
    if (len > (16u << 20)) {
        vm_set_error(vm, "write_file() too large: %s", p);
        *out = val_bool(false);
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) { *out = val_bool(false); return; }
    size_t wrote = data && len ? fwrite(data, 1, len, f) : 0;
    int ok = (fclose(f) == 0) && (wrote == len);
    *out = val_bool(ok);
}

/* Ensure a directory exists, creating it (with parents) when missing. Returns
 * true when the path exists as a directory afterwards. Lets product tools
 * lazily create their private data dirs (`.data/`, `.data/reports/`) instead
 * of hoping a build/deploy step pre-made them.
 * Mode 0700: these dirs hold session/report/portfolio data — 0755 would let
 * other local users read them (matches the 0600 session files). */
static void native_mkdir(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "mkdir() needs a directory path"); return; }
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    if (!p[0]) { *out = val_bool(false); return; }
    char tmp[4096];
    if (strlen(p) >= sizeof tmp) { *out = val_bool(false); return; }
    strcpy(tmp, p);
    for (char *c = tmp + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) { *out = val_bool(false); return; }
            *c = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) { *out = val_bool(false); return; }
    struct stat st;
    *out = val_bool(stat(tmp, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Localtime format of a unix timestamp, like strftime(3). The DSL has no date
 * type, so now() alone can't name or timestamp a report file. */
static void native_strftime(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) {
        vm_set_error(vm, "strftime() needs a format and a unix timestamp");
        return;
    }
    const char *fmt = NULL;
    if (!arg_string(vm, args[0], &fmt)) return;
    if (!IS_NUM(args[1])) {
        vm_set_error(vm, "strftime() timestamp must be a number");
        return;
    }
    time_t t = (time_t)AS_NUM(args[1]);
    struct tm tm;
    char buf[160];
    if (localtime_r(&t, &tm) && strftime(buf, sizeof buf, fmt, &tm) > 0)
        *out = make_string_cstr(vm, buf);
    else
        *out = make_string_cstr(vm, "");
}

/* Dynamic-key map write: put(map, key, value) -> the map. Maps hold object
 * references, so this is the one place the DSL can key by a runtime value
 * (a portfolio symbol) instead of a statically-known member name. */
static void native_put(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 3) { vm_set_error(vm, "put() needs a map, a key and a value"); return; }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "put() key must be a map");
        return;
    }
    const char *k = NULL;
    if (!arg_string(vm, args[1], &k)) return;
    map_set(vm, AS_OBJ(args[0]), k, args[2]); /* roots map + value internally */
    *out = args[0];
}

/* Sorted list of every registered agent tool (local builtins, DSL `tool`
 * registrations, MCP <server>/<tool>, router proxies). */
static void native_tools(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    const char *names[TOOL_MAX];
    int n = 0;
    int full = tools_count();
    for (int i = 0; i < full && n < TOOL_MAX; i++) names[n++] = tools_get(i)->name;
    qsort(names, n, sizeof names[0], str_entry_cmp);
    for (int i = 0; i < n; i++) list_push(vm, list, make_string_cstr(vm, names[i]));
    *out = vm_pop(vm);
}

/* Sorted list of { name, desc } for every indexed skill. */
static void native_skills(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    const SkillInfo *items[256];
    int n = 0;
    int full = skills_count();
    for (int i = 0; i < full && n < 256; i++) items[n++] = skills_get(i);
    qsort(items, n, sizeof items[0], skill_entry_cmp);
    for (int i = 0; i < n; i++) {
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m)); /* root the map while filling */
        map_set(vm, m, "name", make_string_cstr(vm, items[i]->name));
        map_set(vm, m, "desc", make_string_cstr(vm, items[i]->desc));
        vm_pop(vm);                    /* now owned by the rooted list */
        list_push(vm, list, val_obj((Obj *)m));
    }
    *out = vm_pop(vm);
}

/* MCP entries must never be published over HTTP verbatim: `args` may carry
 * absolute host filesystem paths (machine/user layout) and, if a deployment
 * ever inlines a credential, the token too. Replace it with a fixed marker
 * and keep the safe fields (id/transport/command/source/approval). */
#define MCP_ARGS_REDACTED "<redacted>"

static void redact_mcp_args(VM *vm, Obj *m) {
    int found = 0;
    map_get(vm, m, "args", &found);
    if (found) map_set(vm, m, "args", make_string_cstr(vm, MCP_ARGS_REDACTED));
}

static void redact_mcp_list(VM *vm, Value v) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) return;
    Obj *list = AS_OBJ(v);
    for (int i = 0; i < list->as.list.count; i++) {
        Value item = list->as.list.items[i];
        if (!IS_OBJ(item) || AS_OBJ(item)->type != OBJ_MAP) continue;
        redact_mcp_args(vm, AS_OBJ(item));
    }
}

/* Read <cwd>/.data/mcp-servers-router.json and parse it as JSON.
 * Returns null when the file does not exist or is invalid (the router
 * catalog is optional — only present after a router sync ran). */
static void native_mcps(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    const char *path = ".data/mcp-servers-router.json";
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* No sync file -> "no MCP servers known", which is an empty list, not
         * null. Returning null here made /discovery publish "mcps": null and
         * the hub pages crashed on data.mcps.length; a missing sync is a
         * normal state (first boot, router down), not a read error. */
        Obj *empty = AS_OBJ(make_list(vm));
        vm_push(vm, val_obj((Obj *)empty));
        *out = vm_pop(vm);
        return;
    }
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "mcps() too large");
            *out = val_null();
            return;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) { free(b.p); *out = val_null(); return; }
    char err[256] = {0};
    json_parse(vm, b.p, err, sizeof(err));
    free(b.p);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm);
    redact_mcp_list(vm, *out);
}

/* discovery_endpoints(): { llm, router, model } safe for publication.
 * The raw URLs are deliberately omitted — they expose the internal service
 * topology (host.docker.internal, private ports, /v1 paths) and can carry
 * credentials in the query string. The discovery UI only needs to know
 * whether an upstream is wired and which model runs. */
static void native_discovery_endpoints(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    const char *llm = getenv("LLM_API_URL");
    const char *router = getenv("ROUTER_API_URL");
    const char *model = getenv("LLM_MODEL");
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)m)); /* root while filling */
    map_set(vm, m, "llm", (llm && llm[0]) ? make_string_cstr(vm, "configured")
                                           : val_null());
    map_set(vm, m, "router", (router && router[0]) ? make_string_cstr(vm, "configured")
                                                    : val_null());
    map_set(vm, m, "model", (model && model[0]) ? make_string_cstr(vm, model)
                                                 : val_null());
    vm_pop(vm);
    *out = val_obj((Obj *)m);
}

/* ---------- catalog(): rich registry snapshot -----------------------
 * /discovery stays a light debug view (tools as name strings, skills'
 * desc capped at SKILL_DESC_MAX); catalog() is the full 台账:
 *   skills: name + FULL description (re-read from SKILL.md) + path
 *   tools:  name + desc + params schema (OpenAI-style JSON)
 *   mcps:   gateway (.data/mcp-servers-router.json) + local
 *           (.data/mcp-servers.json) merged, each tagged with "source".
 * Backed by the same registries /discovery reads, so a profile allow-list
 * is reflected here too. */

/* Read a whole file into an sbuf (16 MiB cap); 1 ok, 0 otherwise. */
static int file_read_sbuf(const char *path, sbuf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b->len + got > (16u << 20)) {
            fclose(f);
            free(b->p);
            return 0;
        }
        sb_mem(b, buf, got);
    }
    fclose(f);
    return b->p != NULL && !b->oom;
}

/* The in-memory SkillInfo.desc is capped at SKILL_DESC_MAX; re-read the
 * SKILL.md frontmatter to hand out the full one (caller frees). */
static char *skill_full_desc(const char *path) {
    sbuf b = {0};
    if (!path || !file_read_sbuf(path, &b)) return NULL;
    char *p = b.p, *end = b.p + b.len;
    int in_fm = 0;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        while (s < p + linelen && (*s == ' ' || *s == '\t')) s++;
        if (s >= p + linelen) { p += linelen + 1; continue; }
        if (strncmp(s, "---", 3) == 0) {
            if (!in_fm) { in_fm = 1; }
            p += linelen + 1;
            continue;
        }
        if (in_fm && strncmp(s, "description:", 12) == 0) {
            s += 12;
            while (s < p + linelen && (*s == ' ' || *s == '\t')) s++;
            size_t n = (size_t)((p + linelen) - s);
            char *desc = malloc(n + 1);
            if (desc) { memcpy(desc, s, n); desc[n] = '\0'; }
            free(b.p);
            return desc;
        }
        p += linelen + 1;
    }
    free(b.p);
    return NULL;
}

/* Parse <path> as JSON, loading the result onto the VM stack (rooted).
 * Returns 1 when a value was pushed; 0 (nothing pushed) when the file is
 * missing. A parse failure surfaces through vm->error like any other. */
static int json_file_push(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "catalog(): file too large");
            return 0;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) { free(b.p); return 0; }
    char err[256] = {0};
    json_parse(vm, b.p, err, sizeof(err));
    free(b.p);
    /* success leaves the parsed value on the VM stack */
    return !vm->error;
}

static int tool_def_cmp(const void *a, const void *b) {
    const ToolDef *x = *(const ToolDef *const *)a;
    const ToolDef *y = *(const ToolDef *const *)b;
    return strcmp(x->name, y->name);
}

/* Append every map in a parsed JSON array to `list`, tagging source. */
static void mcps_merge(VM *vm, Obj *list, Value v, const char *source) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) return;
    Obj *src = AS_OBJ(v);
    for (int i = 0; i < src->as.list.count; i++) {
        Value item = src->as.list.items[i];
        if (!IS_OBJ(item) || AS_OBJ(item)->type != OBJ_MAP) continue;
        map_set(vm, AS_OBJ(item), "source", make_string_cstr(vm, source));
        redact_mcp_args(vm, AS_OBJ(item));
        list_push(vm, list, item);
    }
}

/* Build the rich catalog: { skills:[{name,desc,path}], tools:[{name,desc,
 * schema}], mcps:[{id,source,...}] }. The result map is left rooted ON the
 * VM stack (caller pops it after use) — returned via *out. */
static void build_catalog(VM *vm, Value *out) {
    /* skills: full desc + path, sorted by name */
    Obj *skills = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)skills)); /* root while filling */
    const SkillInfo *sitems[256];
    int sfull = skills_count();
    int sn = 0;
    for (int i = 0; i < sfull && sn < 256; i++) sitems[sn++] = skills_get(i);
    qsort(sitems, sn, sizeof sitems[0], skill_entry_cmp);
    for (int i = 0; i < sn; i++) {
        char *full = skill_full_desc(sitems[i]->path);
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m));
        map_set(vm, m, "name", make_string_cstr(vm, sitems[i]->name));
        map_set(vm, m, "desc",
                make_string_cstr(vm, full ? full : sitems[i]->desc));
        map_set(vm, m, "path", make_string_cstr(vm, sitems[i]->path));
        free(full);
        vm_pop(vm);
        list_push(vm, skills, val_obj((Obj *)m));
    }

    /* tools: name/desc/schema, sorted by name */
    Obj *tools = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)tools));
    const ToolDef *titems[TOOL_MAX];
    int tfull = tools_count();
    int tn = 0;
    for (int i = 0; i < tfull && tn < TOOL_MAX; i++) titems[tn++] = tools_get(i);
    qsort(titems, tn, sizeof titems[0], tool_def_cmp);
    for (int i = 0; i < tn; i++) {
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m));
        map_set(vm, m, "name", make_string_cstr(vm, titems[i]->name));
        map_set(vm, m, "desc", make_string_cstr(vm, titems[i]->desc));
        map_set(vm, m, "schema",
                make_string_cstr(vm, titems[i]->params[0] ? titems[i]->params
                                                          : "{}"));
        vm_pop(vm);
        list_push(vm, tools, val_obj((Obj *)m));
    }

    /* mcps: gateway + local, merged with a source tag */
    Obj *mcps = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)mcps));
    int gw = json_file_push(vm, ".data/mcp-servers-router.json");
    if (gw) {
        mcps_merge(vm, mcps, vm->stack[vm->stack_count - 1], "gateway");
        vm_pop(vm);
    }
    int loc = json_file_push(vm, ".data/mcp-servers.json");
    if (loc) {
        mcps_merge(vm, mcps, vm->stack[vm->stack_count - 1], "local");
        vm_pop(vm);
    }

    Obj *r = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)r));
    map_set(vm, r, "skills", val_obj((Obj *)skills));
    map_set(vm, r, "tools", val_obj((Obj *)tools));
    map_set(vm, r, "mcps", val_obj((Obj *)mcps));
    *out = val_obj((Obj *)r); /* left on the stack; caller pops */
}

static void native_catalog(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    build_catalog(vm, out);
    vm_pop(vm); /* r */
    vm_pop(vm); /* mcps */
    vm_pop(vm); /* tools */
    vm_pop(vm); /* skills */
}

/* ---------- Virtual DOM: el(tag, props, ...children) / render(vnode)
 * (reconstructed) ----------
 * SSR story: Lume builds a vnode tree with el() and render() serializes it
 * to HTML once (html() is the template-string shell that mixes markup and
 * slots, deferring to render logic for vnode/list slots). Text and attribute
 * values are escaped; `on*` props are dropped (event handlers run
 * client-side); `data_page` is emitted as `data-page` so it stays a valid
 * Lume identifier. */

static void el_grow(Obj *list) {
    if (list->as.list.count == list->as.list.cap) {
        list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
        list->as.list.items = realloc(list->as.list.items,
                                      sizeof(Value) * (size_t)list->as.list.cap);
    }
}

static void html_escape(sbuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&': sb_str(b, "&amp;"); break;
            case '<': sb_str(b, "&lt;"); break;
            case '>': sb_str(b, "&gt;"); break;
            case '"': sb_str(b, "&quot;"); break;
            case '\'': sb_str(b, "&#39;"); break;
            default:  sb_chr(b, s[i]);
        }
    }
}

/* HTML void elements: render as <tag> with no closing tag. */
static bool is_void_tag(const char *tag) {
    static const char *const void_tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; void_tags[i]; i++)
        if (strcmp(tag, void_tags[i]) == 0) return true;
    return false;
}

static void scalar_into(sbuf *b, Value v) {
    if (IS_NULL(v)) return;
    if (IS_BOOL(v)) { sb_str(b, AS_BOOL(v) ? "true" : "false"); return; }
    if (IS_NUM(v)) {
        char buf[64];
        double d = AS_NUM(v);
        if (d == (long long)d)
            snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            snprintf(buf, sizeof(buf), "%g", d);
        sb_str(b, buf);
        return;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        html_escape(b, obj_string(AS_OBJ(v)), obj_string_len(AS_OBJ(v)));
}

static void render_node(VM *vm, sbuf *b, Value v);

static void element_to_sb(VM *vm, sbuf *b, Obj *o) {
    int found = 0;
    Value tagv = map_get(vm, o, "type", &found);
    const char *tag = (found && IS_OBJ(tagv) && AS_OBJ(tagv)->type == OBJ_STRING)
                          ? obj_string(AS_OBJ(tagv))
                          : "div";
    sb_chr(b, '<');
    sb_str(b, tag);
    int pfound = 0;
    Value pv = map_get(vm, o, "props", &pfound);
    if (pfound && IS_OBJ(pv) && AS_OBJ(pv)->type == OBJ_MAP) {
        Obj *props = AS_OBJ(pv);
        for (int i = 0; i < props->as.map.count; i++) {
            const char *key = props->as.map.keys[i];
            Value vv = props->as.map.vals[i];
            /* client-side handlers, null props and `false` values are
             * dropped server-side (`hidden: true` still renders) */
            if (strncmp(key, "on", 2) == 0 || IS_NULL(vv) ||
                (IS_BOOL(vv) && !AS_BOOL(vv)))
                continue;
            const char *attr = strcmp(key, "data_page") == 0 ? "data-page" : key;
            sb_str(b, " ");
            sb_str(b, attr);
            sb_str(b, "=\"");
            scalar_into(b, vv);
            sb_chr(b, '"');
        }
    }
    sb_str(b, ">");
    int cfound = 0;
    Value cv = map_get(vm, o, "children", &cfound);
    if (cfound && IS_OBJ(cv) && AS_OBJ(cv)->type == OBJ_LIST) {
        Obj *cs = AS_OBJ(cv);
        for (int i = 0; i < cs->as.list.count; i++)
            render_node(vm, b, cs->as.list.items[i]);
    }
    if (!is_void_tag(tag)) {
        sb_str(b, "</");
        sb_str(b, tag);
        sb_chr(b, '>');
    }
}

static void render_node(VM *vm, sbuf *b, Value v) {
    if (!IS_OBJ(v)) { scalar_into(b, v); return; }
    Obj *o = AS_OBJ(v);
    if (o->type == OBJ_STRING) { html_escape(b, obj_string(o), obj_string_len(o)); return; }
    if (o->type == OBJ_LIST) {
        for (int i = 0; i < o->as.list.count; i++)
            render_node(vm, b, o->as.list.items[i]);
        return;
    }
    if (o->type == OBJ_MAP) { element_to_sb(vm, b, o); return; }
    scalar_into(b, v);
}

/* html() slot injection: vnodes render structurally, a list flattens with
 * the same rule, and strings are escaped to HTML just like el() text
 * children — EXCEPT strings produced by html() itself (trusted_html), which
 * are already-rendered markup and compose raw into the outer shell. */
static void slot_into(VM *vm, sbuf *b, Value v) {
    if (IS_OBJ(v)) {
        Obj *o = AS_OBJ(v);
        if (o->type == OBJ_STRING) {
            if (o->as.str.trusted_html)
                sb_mem(b, obj_string(o), obj_string_len(o));
            else
                html_escape(b, obj_string(o), obj_string_len(o));
            return;
        }
        if (o->type == OBJ_LIST) {
            for (int i = 0; i < o->as.list.count; i++)
                slot_into(vm, b, o->as.list.items[i]);
            return;
        }
        if (o->type == OBJ_MAP) { element_to_sb(vm, b, o); return; }
    }
    scalar_into(b, v);
}

static void native_el(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "el() needs (tag, props, ...children)");
        return;
    }
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_MAP)
        args[1] = make_map(vm);
    vm_push(vm, make_map(vm));
    Obj *m = AS_OBJ(vm->stack[vm->stack_count - 1]);
    map_set(vm, m, "type", args[0]);
    map_set(vm, m, "props", args[1]);
    vm_push(vm, make_list(vm));
    Obj *kids = AS_OBJ(vm->stack[vm->stack_count - 1]);
    for (int i = 2; i < argc; i++) {
        el_grow(kids);
        kids->as.list.items[kids->as.list.count++] = args[i];
    }
    map_set(vm, m, "children", vm_pop(vm));
    vm_pop(vm);
    *out = val_obj(m);
}

static void native_render(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "render() needs a vnode"); return; }
    sbuf b = {0};
    render_node(vm, &b, args[0]);
    *out = make_string(vm, b.p ? b.p : "", b.len);
    free(b.p);
}

static void native_html(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "html() needs (template, ...slots)");
        return;
    }
    size_t n = obj_string_len(AS_OBJ(args[0]));
    const char *t = obj_string(AS_OBJ(args[0]));
    sbuf b = {0};
    const char *p = t, *end = t + n;
    while (p < end) {
        /* `{{` and `}}` are literal braces (mustache escape) */
        if (*p == '{' && p + 1 < end && p[1] == '{') { sb_chr(&b, '{'); p += 2; continue; }
        if (*p == '}' && p + 1 < end && p[1] == '}') { sb_chr(&b, '}'); p += 2; continue; }
        if (*p == '{' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            long idx = 0;
            while (q < end && *q >= '0' && *q <= '9') { idx = idx * 10 + (*q - '0'); q++; }
            if (q < end && *q == '}' && idx < argc - 1) {
                slot_into(vm, &b, args[idx + 1]);
                p = q + 1;
                continue;
            }
        }
        sb_chr(&b, *p);
        p++;
    }
    *out = make_string(vm, b.p ? b.p : "", b.len);
    free(b.p);
    /* html() returns already-rendered markup: mark it so a slot inside an
     * outer html() shell injects it raw instead of escaping it again. */
    if (IS_OBJ(*out) && AS_OBJ(*out)->type == OBJ_STRING)
        AS_OBJ(*out)->as.str.trusted_html = true;
}

typedef void (*Native2)(VM *, int, Value *, Value *);

static Value vm_native(VM *vm, int argc, Value *args, Native2 impl) {
    Value out = val_null();
    impl(vm, argc, args, &out);
    return out;
}

static Value b_run(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    bridge_run(vm);
    return val_null();
}
static Value b_print(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_print); }
static Value b_str(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_str); }
static Value b_int(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_int); }
static Value b_float(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_float); }
static Value b_bool(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bool); }
static Value b_string(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_str); }
static Value b_len(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_len); }
static Value b_keys(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_keys); }
static Value b_get(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_get); }
static Value b_json(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_json); }
static Value b_stringify(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_stringify); }
static Value b_now(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_now); }
static Value b_env(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_env); }
static Value b_files(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_files); }
static Value b_read_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_read_file); }
static Value b_write_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_write_file); }
static Value b_mkdir(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mkdir); }
static Value b_strftime(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_strftime); }
static Value b_put(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_put); }
static Value b_tools(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_tools); }
static Value b_skills(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_skills); }
static Value b_mcps(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mcps); }
static Value b_discovery_endpoints(VM *vm, int argc, Value *args)
{ return vm_native(vm, argc, args, native_discovery_endpoints); }
static Value b_catalog(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_catalog); }
static Value b_default_route(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_default_route); }
static Value b_el(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_el); }
static Value b_render(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_render); }
static Value b_html(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_html); }

/* ---------- function call engine ---------- */

/* Contract: the caller has already pushed the callee and its args onto the
 * value stack (callee first, then the args for a total of argc+1 slots).
 * call_function replaces that region with a single result value. */
void call_function(VM *vm, Value callee, int argc) {
    if (vm->error) return;

    int slot = vm->stack_count - argc - 1; /* callee slot */

    Obj *f = IS_OBJ(callee) ? AS_OBJ(callee) : NULL;
    if (!f || (f->type != OBJ_NATIVE && f->type != OBJ_FUNC)) {
        vm_set_error(vm, "calling a non-function value");
        vm->stack_count = slot;
        vm_push(vm, val_null());
        return;
    }

    if (f->type == OBJ_NATIVE) {
        Value result = f->as.native.fn(vm, argc, &vm->stack[slot + 1]);
        vm->stack_count = slot;
        vm_push(vm, vm->error ? val_null() : result);
        return;
    }

    /* user function */
    if (f->as.fn.arity != argc) {
        vm_set_error(vm, "function '%s' expects %d arguments, got %d",
                     f->as.fn.name, f->as.fn.arity, argc);
        vm->stack_count = slot;
        vm_push(vm, val_null());
        return;
    }

    Value envv = make_env(vm, f->as.fn.closure);
    vm_push(vm, envv); /* root the fresh frame until activated */
    Env *frame = (Env *)AS_OBJ(envv);

    for (int i = 0; i < argc; i++)
        env_set(vm, frame, f->as.fn.params[i], vm->stack[slot + 1 + i]);
    if (vm->error) goto user_done;

    frame->next_active = vm->active_envs;
    vm->active_envs = frame;
    vm->call_result = val_null();

    int index = vm->jump_depth;
    if (index >= AL_FRAME_MAX) {
        vm_set_error(vm, "call depth exceeded (%d)", AL_FRAME_MAX);
        goto user_done;
    }
    if (setjmp(vm->jump_bufs[index]) == 0) {
        vm->jump_depth++;
        exec_block_walk(vm, (Node *)f->as.fn.body, frame);
    }
    /* both the normal and the longjmp paths pop the frame */
    vm->jump_depth--;

user_done:
    /* deactivate the frame in both normal and jump paths */
    vm->active_envs = frame->next_active;
    vm->stack_count = slot;
    vm_pop(vm); /* drop the env root */
    vm->stack_count = slot;
    vm_push(vm, vm->error ? val_null() : vm->call_result);
}

static void exec_block_walk(VM *vm, Node *block, Env *env) {
    if (!block) return;
    if (block->type == N_BLOCK) {
        for (int i = 0; i < block->as.block.count && !vm->error; i++)
            exec_statement(vm, block->as.block.stmts[i], env);
        return;
    }
    exec_statement(vm, block, env);
}

/* ---------- expressions ---------- */

/* Zero value of a statically-known member type. Used when a typed map member
 * (a tool handler's `arg.<field>`) is absent, so callers get a usable default
 * rather than a runtime error. `any`/aggregate types fall back to null. */
static Value vm_zero_value(VM *vm, Type *t) {
    switch (t->kind) {
        case TY_INT:
        case TY_FLOAT:
            return val_num(0.0);
        case TY_STRING:
            return make_string_cstr(vm, "");
        case TY_BOOL:
            return val_bool(false);
        default:
            return val_null();
    }
}

static void eval_expr(VM *vm, Node *n, Env *env) {
    if (vm->error) return;
    switch (n->type) {
        case N_LITERAL: {
            switch (n->as.lit.kind) {
                case LIT_NUM:  vm_push(vm, val_num(n->as.lit.num)); return;
                case LIT_TRUE: vm_push(vm, val_bool(true)); return;
                case LIT_FALSE: vm_push(vm, val_bool(false)); return;
                case LIT_NULL: vm_push(vm, val_null()); return;
                case LIT_STR: {
                    int len = 0;
                    char *txt = unescape_literal(n->as.lit.text, n->as.lit.len, &len);
                    vm_push(vm, make_string(vm, txt, (size_t)len));
                    free(txt);
                    return;
                }
            }
            return;
        }
        case N_VAR: {
            int found = 0;
            Value v = env_get(env, n->as.var.name, &found);
            if (!found) {
                vm_set_error(vm, "line %zu: undefined variable '%s'", n->line, n->as.var.name);
                return;
            }
            vm_push(vm, v);
            return;
        }
        case N_ASSIGN: {
            eval_expr(vm, n->as.assign.value, env);
            if (vm->error) return;
            Value v = vm_peek(vm, 0); /* keep rooted on the stack */
            env_set(vm, env, n->as.assign.name, v);
            return; /* result: assigned value, already on stack */
        }
        case N_ASSIGN_MEMBER: {
            eval_expr(vm, n->as.assign_mem.obj, env);
            if (vm->error) return;
            eval_expr(vm, n->as.assign_mem.value, env);
            if (vm->error) return;
            Value val = vm_peek(vm, 0);
            Value objv = vm_peek(vm, 1);
            if (!IS_OBJ(objv) || AS_OBJ(objv)->type != OBJ_MAP) {
                vm_set_error(vm, "line %zu: cannot assign member on non-map", n->line);
                return;
            }
            map_set(vm, AS_OBJ(objv), n->as.assign_mem.name, val);
            vm_pop(vm); /* val */
            vm_pop(vm); /* obj */
            vm_push(vm, val);
            return;
        }
        case N_MAP_LIT: {
            Obj *m = AS_OBJ(make_map(vm));
            vm_push(vm, val_obj(m)); /* root during construction */
            for (int i = 0; i < n->as.map.count; i++) {
                eval_expr(vm, n->as.map.vals[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, m, n->as.map.keys[i], v);
            }
            return; /* map stays on stack */
        }
        case N_LIST_LIT: {
            Obj *list = AS_OBJ(make_list(vm));
            vm_push(vm, val_obj(list)); /* root during construction */
            for (int i = 0; i < n->as.list.count; i++) {
                eval_expr(vm, n->as.list.items[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                vm_push(vm, v); /* root while the array grows */
                if (list->as.list.count == list->as.list.cap) {
                    list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
                    list->as.list.items = realloc(list->as.list.items,
                                                  sizeof(Value) * (size_t)list->as.list.cap);
                }
                list->as.list.items[list->as.list.count++] = v;
                vm_pop(vm);
            }
            return;
        }
        case N_FUNC_LIT: {
            Value f = make_func(vm, "(anonymous)", n->as.funclit.names,
                                n->as.funclit.arity, n->as.funclit.body, env);
            vm_push(vm, f);
            return;
        }
        case N_MEMBER: {
            eval_expr(vm, n->as.member.obj, env);
            if (vm->error) return;
            Value objv = vm_peek(vm, 0);
            if (IS_OBJ(objv)) {
                Obj *o = AS_OBJ(objv);
                if (o->type == OBJ_MAP) {
                    int found = 0;
                    Value v = map_get(vm, o, n->as.member.name, &found);
                    if (!found) {
                        /* Member type is known statically (tool handler args
                         * typed by the schema): a missing key reads as the
                         * type's zero value instead of failing. */
                        if (n->as.member.type) {
                            Value z = vm_zero_value(vm, n->as.member.type);
                            vm_pop(vm);
                            vm_push(vm, z);
                            return;
                        }
                        vm_set_error(vm, "line %zu: map has no field '%s'", n->line, n->as.member.name);
                        return;
                    }
                    vm_pop(vm);
                    vm_push(vm, v);
                    return;
                }
                if (o->type == OBJ_STRING &&
                    (strcmp(n->as.member.name, "len") == 0 ||
                     strcmp(n->as.member.name, "length") == 0)) {
                    vm_pop(vm);
                    vm_push(vm, val_int((long long)o->as.str.len));
                    return;
                }
                if (o->type == OBJ_LIST &&
                    (strcmp(n->as.member.name, "len") == 0 ||
                     strcmp(n->as.member.name, "length") == 0)) {
                    vm_pop(vm);
                    vm_push(vm, val_int(o->as.list.count));
                    return;
                }
            }
            vm_set_error(vm, "line %zu: cannot read field '%s' on this value",
                         n->line, n->as.member.name);
            return;
        }
        case N_UNARY: {
            eval_expr(vm, n->as.unary.operand, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            if (n->as.unary.op == OP_NOT) {
                vm_push(vm, val_bool(!value_truthy(v)));
            } else { /* OP_NEG */
                if (!IS_NUM(v)) { vm_set_error(vm, "cannot negate a non-number"); return; }
                vm_push(vm, val_num(-AS_NUM(v)));
            }
            return;
        }
        case N_BINARY: {
            Op op = n->as.binary.op;
            if (op == OP_AND || op == OP_OR) {
                /* short-circuit: never evaluate the right operand unless the
                 * left decides the result (b != 0 and a / b > 1 must not
                 * divide by zero when b == 0) */
                eval_expr(vm, n->as.binary.left, env);
                if (vm->error) return;
                bool lt = value_truthy(vm_pop(vm));
                if ((op == OP_AND && !lt) || (op == OP_OR && lt)) {
                    vm_push(vm, val_bool(lt));
                    return;
                }
                eval_expr(vm, n->as.binary.right, env);
                if (vm->error) return;
                bool rt = value_truthy(vm_pop(vm));
                vm_push(vm, val_bool(op == OP_AND ? (lt && rt) : (lt || rt)));
                return;
            }
            eval_expr(vm, n->as.binary.left, env);
            if (vm->error) return;
            eval_expr(vm, n->as.binary.right, env);
            if (vm->error) return;
            Value r = vm_pop(vm);
            Value l = vm_pop(vm);

            if (n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE) {
                bool eq = values_equal(l, r);
                vm_push(vm, val_bool(n->as.binary.op == OP_EQ ? eq : !eq));
                return;
            }

            /* string concatenation with + */
            if (n->as.binary.op == OP_ADD &&
                IS_OBJ(l) && AS_OBJ(l)->type == OBJ_STRING &&
                IS_OBJ(r) && AS_OBJ(r)->type == OBJ_STRING) {
                Obj *a = AS_OBJ(l), *b = AS_OBJ(r);
                size_t total = a->as.str.len + b->as.str.len;
                char *buf = malloc(total + 1);
                if (total) {
                    memcpy(buf, obj_string(a), a->as.str.len);
                    memcpy(buf + a->as.str.len, obj_string(b), b->as.str.len);
                }
                buf[total] = '\0';
                Value s = make_string(vm, buf, total);
                free(buf);
                vm_push(vm, s);
                return;
            }

            if (!IS_NUM(l) || !IS_NUM(r)) {
                vm_set_error(vm, "line %zu: operator needs numbers", n->line);
                return;
            }
            double a = AS_NUM(l), b = AS_NUM(r);
            switch (n->as.binary.op) {
                case OP_ADD: vm_push(vm, val_num(a + b)); return;
                case OP_SUB: vm_push(vm, val_num(a - b)); return;
                case OP_MUL: vm_push(vm, val_num(a * b)); return;
                case OP_DIV:
                    if (b == 0) { vm_set_error(vm, "division by zero"); return; }
                    vm_push(vm, val_num(a / b));
                    return;
                case OP_MOD:
                    if (b == 0) { vm_set_error(vm, "modulo by zero"); return; }
                    vm_push(vm, val_num(fmod(a, b)));
                    return;
                case OP_LT: vm_push(vm, val_bool(a < b)); return;
                case OP_LE: vm_push(vm, val_bool(a <= b)); return;
                case OP_GT: vm_push(vm, val_bool(a > b)); return;
                case OP_GE: vm_push(vm, val_bool(a >= b)); return;
                default: vm_push(vm, val_null()); return;
            }
        }
        case N_CALL: {
            eval_expr(vm, n->as.call.callee, env);
            if (vm->error) return;
            for (int i = 0; i < n->as.call.argc; i++) {
                eval_expr(vm, n->as.call.args[i], env);
                if (vm->error) return;
            }
            /* stack now: [callee, a0..an-1] */
            Value callee = vm_peek(vm, n->as.call.argc);
            if (!IS_OBJ(callee) ||
                (AS_OBJ(callee)->type != OBJ_FUNC && AS_OBJ(callee)->type != OBJ_NATIVE)) {
                vm_set_error(vm, "line %zu: trying to call a non-function", n->line);
                return;
            }
            call_function(vm, callee, n->as.call.argc);
            if (vm->error) return;
            if (n->as.call.propagate) {
                /* `?` — unwrap a Result: `{ err: e }` propagates up like a
                 * return; `{ ok: v }` becomes the expression's value. */
                Value r = vm_peek(vm, 0);
                if (IS_OBJ(r) && AS_OBJ(r)->type == OBJ_MAP) {
                    Obj *res = AS_OBJ(r);
                    int found = 0;
                    map_get(vm, res, "err", &found);
                    if (found) {
                        if (vm->jump_depth <= 0) {
                            vm_set_error(vm,
                                "line %zu: '?' with an error outside a function",
                                n->line);
                            return;
                        }
                        vm->call_result = r;
                        longjmp(vm->jump_bufs[vm->jump_depth - 1], 1);
                    }
                    Value o = map_get(vm, res, "ok", &found);
                    if (found) {
                        vm_pop(vm);
                        vm_push(vm, o);
                    }
                    /* neither key: not a Result — no-op (loose callee) */
                }
            }
            return;
        }
        default:
            vm_set_error(vm, "line %zu: internal error (expr node %d)", n->line, (int)n->type);
            return;
    }
}

/* ---------- statements ---------- */

static void exec_statement(VM *vm, Node *n, Env *env) {
    if (vm->error) return;
    switch (n->type) {
        case N_BLOCK:
            for (int i = 0; i < n->as.block.count && !vm->error; i++)
                exec_statement(vm, n->as.block.stmts[i], env);
            return;
        case N_LET: {
            eval_expr(vm, n->as.let.init, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            env_set(vm, env, n->as.let.name, v);
            return;
        }
        case N_IF: {
            eval_expr(vm, n->as.ifs.cond, env);
            if (vm->error) return;
            if (value_truthy(vm_pop(vm))) exec_statement(vm, n->as.ifs.then, env);
            else if (n->as.ifs.els) exec_statement(vm, n->as.ifs.els, env);
            return;
        }
        case N_WHILE: {
            while (!vm->error) {
                eval_expr(vm, n->as.whiles.cond, env);
                if (vm->error) return;
                if (!value_truthy(vm_pop(vm))) return;
                exec_statement(vm, n->as.whiles.body, env);
            }
            return;
        }
        case N_EXPR_STMT:
            eval_expr(vm, n->as.expr_stmt.expr, env);
            if (!vm->error) vm_pop(vm);
            return;
        case N_RETURN: {
            if (n->as.ret.expr) {
                eval_expr(vm, n->as.ret.expr, env);
                if (vm->error) return;
                vm->call_result = vm_pop(vm);
            } else {
                vm->call_result = val_null();
            }
            if (vm->jump_depth <= 0) {
                vm_set_error(vm, "line %zu: return outside a function", n->line);
                return;
            }
            longjmp(vm->jump_bufs[vm->jump_depth - 1], 1);
            break; /* unreachable */
        }
        case N_FUNC_DECL: {
            Value f = make_func(vm, n->as.func.name, n->as.func.names,
                                n->as.func.arity, n->as.func.body, env);
            env_set(vm, env, n->as.func.name, f);
            return;
        }
        case N_TYPE_DECL:
            /* type declarations are checked statically, then vanish */
            return;
        case N_SERVER: {
            if (!vm->server_config)
                vm->server_config = AS_OBJ(make_map(vm));
            Obj *cfg = vm->server_config;
            for (int i = 0; i < n->as.server.count; i++) {
                Node *a = n->as.server.assigns[i];
                eval_expr(vm, a->as.assign.value, env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, cfg, a->as.assign.name, v);
            }
            return;
        }
        case N_ROUTE: {
            Value handler;
            if (n->as.route.handler) {
                eval_expr(vm, n->as.route.handler, env);
                if (vm->error) return;
                handler = vm_pop(vm);
            } else {
                handler = vm->default_handler; /* handler-less route */
            }
            if (n->as.route.alias) {
                /* `group "path", handler;` -> one route per method in the
                 * group, all sharing the same handler. A list group registers
                 * bare methods; a map group (method -> label) also attaches a
                 * label that the shim exposes as req.label. */
                int found = 0;
                Value group = env_get(env, n->as.route.alias, &found);
                if (!found || !IS_OBJ(group) ||
                    (AS_OBJ(group)->type != OBJ_LIST &&
                     AS_OBJ(group)->type != OBJ_MAP)) {
                    vm_set_error(vm, "line %zu: unknown verb group '%s' "
                                 "(declare it with `verbs %s = [...];` first)",
                                 n->line, n->as.route.alias, n->as.route.alias);
                    return;
                }
                Obj *g = AS_OBJ(group);
                if (g->type == OBJ_LIST) {
                    for (int i = 0; i < g->as.list.count; i++) {
                        Value mv = g->as.list.items[i];
                        if (!IS_OBJ(mv) || AS_OBJ(mv)->type != OBJ_STRING) {
                            vm_set_error(vm, "line %zu: verb group '%s' must contain "
                                         "only method strings",
                                         n->line, n->as.route.alias);
                            return;
                        }
                        const char *m = obj_string(AS_OBJ(mv));
                        if (bridge_define_route(vm, m, n->as.route.path, handler,
                                                NULL) != 0) {
                            vm_set_error(vm, "failed to register route %s %s", m,
                                         n->as.route.path);
                            return;
                        }
                    }
                } else {
                    for (int i = 0; i < g->as.map.count; i++) {
                        const char *m = g->as.map.keys[i];
                        Value lv = g->as.map.vals[i];
                        const char *label = NULL;
                        if (IS_OBJ(lv) && AS_OBJ(lv)->type == OBJ_STRING) {
                            label = obj_string(AS_OBJ(lv));
                        } else if (!IS_NULL(lv)) {
                            vm_set_error(vm, "line %zu: verb group '%s' label for "
                                         "%s must be a string",
                                         n->line, n->as.route.alias, m);
                            return;
                        }
                        if (bridge_define_route(vm, m, n->as.route.path, handler,
                                                label) != 0) {
                            vm_set_error(vm, "failed to register route %s %s", m,
                                         n->as.route.path);
                            return;
                        }
                    }
                }
                return;
            }
            if (bridge_define_route(vm, n->as.route.method, n->as.route.path, handler,
                                    NULL) != 0)
                vm_set_error(vm, "failed to register route %s %s",
                             n->as.route.method, n->as.route.path);
            return;
        }
        case N_VERBS: {
            eval_expr(vm, n->as.verbs.methods, env);
            if (vm->error) return;
            Value methods = vm_pop(vm);
            if (!IS_OBJ(methods) ||
                (AS_OBJ(methods)->type != OBJ_LIST &&
                 AS_OBJ(methods)->type != OBJ_MAP)) {
                vm_set_error(vm, "line %zu: `verbs %s` needs a list of methods or "
                             "a map of method -> label", n->line, n->as.verbs.name);
                return;
            }
            Obj *g = AS_OBJ(methods);
            if (g->type == OBJ_LIST) {
                for (int i = 0; i < g->as.list.count; i++) {
                    Value mv = g->as.list.items[i];
                    if (!IS_OBJ(mv) || AS_OBJ(mv)->type != OBJ_STRING) {
                        vm_set_error(vm, "line %zu: `verbs %s` items must be strings",
                                     n->line, n->as.verbs.name);
                        return;
                    }
                }
            } else {
                for (int i = 0; i < g->as.map.count; i++) {
                    Value lv = g->as.map.vals[i];
                    if (!IS_NULL(lv) &&
                        !(IS_OBJ(lv) && AS_OBJ(lv)->type == OBJ_STRING)) {
                        vm_set_error(vm, "line %zu: `verbs %s` label for %s must be "
                                     "a string", n->line, n->as.verbs.name,
                                     g->as.map.keys[i]);
                        return;
                    }
                }
            }
            env_set(vm, env, n->as.verbs.name, methods);
            return;
        }
        case N_TOOL: {
            eval_expr(vm, n->as.tool.params, env);
            if (vm->error) return;
            Value params = vm_pop(vm);
            sbuf b = {0};
            if (IS_OBJ(params) && AS_OBJ(params)->type == OBJ_MAP) {
                /* { a: int } — a bare type keyword evaluates to the seeded
                 * native/function; rewrite such values to their name string so
                 * the schema serializes as { a: "int" }. */
                vm_push(vm, params);
                Obj *m = AS_OBJ(params);
                for (int i = 0; i < m->as.map.count; i++) {
                    Value v = m->as.map.vals[i];
                    if (IS_OBJ(v) && (AS_OBJ(v)->type == OBJ_NATIVE ||
                                      AS_OBJ(v)->type == OBJ_FUNC)) {
                        const char *nm = (AS_OBJ(v)->type == OBJ_NATIVE)
                                             ? AS_OBJ(v)->as.native.name
                                             : AS_OBJ(v)->as.fn.name;
                        m->as.map.vals[i] = make_string_cstr(vm, nm ? nm : "");
                    }
                }
                json_append_value(vm, &b, params);
                vm_pop(vm);
            } else {
                json_append_value(vm, &b, params);
            }
            eval_expr(vm, n->as.tool.handler, env);
            if (vm->error) return;
            Value handler = vm_pop(vm);
            if (bridge_define_tool(vm, n->as.tool.name, n->as.tool.desc,
                                   b.p ? b.p : "", handler) != 0)
                vm_set_error(vm, "failed to register tool '%s'", n->as.tool.name);
            return;
        }
        default:
            vm_set_error(vm, "line %zu: internal error (stmt node %d)", n->line, (int)n->type);
            return;
    }
}

/* ---------- VM setup & top-level execution ---------- */

/* Built-in verb groups, so `write "/items", h;` needs no declaration.
 * `write` maps the write methods to conventional labels (surfaced as
 * `req.label`); `read` is a plain GET/HEAD method list. A user statement
 * `verbs write = ...;` / `verbs read = ...;` simply shadows either one. */
static void seed_verb_groups(VM *vm) {
    static const char *const w_methods[] = {"POST", "PUT", "PATCH", "DELETE"};
    static const char *const w_labels[]  = {"created", "replaced",
                                            "patched", "deleted"};
    static const char *const r_methods[] = {"GET", "HEAD"};

    Obj *w = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(w)); /* root while filling */
    for (int i = 0; i < 4; i++)
        map_set(vm, w, w_methods[i], make_string_cstr(vm, w_labels[i]));
    env_set(vm, vm->globals, "write", vm_peek(vm, 0));
    vm_pop(vm);

    Obj *r = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(r)); /* root while filling */
    for (int i = 0; i < 2; i++) {
        Value s = make_string_cstr(vm, r_methods[i]);
        if (r->as.list.count == r->as.list.cap) {
            r->as.list.cap = r->as.list.cap ? r->as.list.cap * 2 : 8;
            r->as.list.items = realloc(r->as.list.items,
                                       sizeof(Value) * (size_t)r->as.list.cap);
        }
        r->as.list.items[r->as.list.count++] = s;
    }
    env_set(vm, vm->globals, "read", vm_peek(vm, 0));
    vm_pop(vm);
}

void bridge_seed_builtins(VM *vm) {
    struct { const char *name; NativeFn fn; } built[] = {
        {"run", b_run},
        {"print", b_print},
        {"str", b_str},
        {"int", b_int},
        {"float", b_float},
        {"bool", b_bool},
        {"string", b_string},
        {"len", b_len},
        {"keys", b_keys},
        {"get", b_get},
        {"json", b_json},
        {"stringify", b_stringify},
        {"now", b_now},
        {"env", b_env},
        {"files", b_files},
        {"read_file", b_read_file},
        {"write_file", b_write_file},
        {"mkdir", b_mkdir},
        {"strftime", b_strftime},
        {"put", b_put},
        {"tools", b_tools},
        {"skills", b_skills},
        {"mcps", b_mcps},
        {"discovery_endpoints", b_discovery_endpoints},
        {"catalog", b_catalog},
        {"el", b_el},
        {"render", b_render},
        {"html", b_html},
    };
    for (size_t i = 0; i < sizeof(built) / sizeof(built[0]); i++)
        env_set(vm, vm->globals, built[i].name,
                make_native(vm, built[i].name, built[i].fn));
    seed_verb_groups(vm);
    vm->default_handler = make_native(vm, "__default_route", b_default_route);
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->gc_threshold = 2 * 1024 * 1024;
    vm->globals = (Env *)AS_OBJ(make_env(vm, NULL));
    bridge_seed_builtins(vm);
}

void exec_program(VM *vm, Node *prog) {
    if (!prog || prog->type != N_PROGRAM) {
        vm_set_error(vm, "no program to run");
        return;
    }
    for (int i = 0; i < prog->as.program.count && !vm->error; i++)
        exec_statement(vm, prog->as.program.stmts[i], vm->globals);
}