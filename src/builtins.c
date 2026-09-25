/* Native builtin functions for Lume — split out of interp.c
 * (2026-09-25 refactor; behavior unchanged). The b_* wrappers are the
 * function pointers bridge_seed_builtins() registers; native_* are the
 * static implementations. VM plumbing lives in interp.c. */

#include "builtins.h"
#include "minijson.h"
#include "tools.h"
#include "skills.h"
#include "sqlite_tool.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/file.h>   /* flock */
#include <sys/stat.h>
#include <sys/time.h>   /* gettimeofday */
#include <time.h>
#include <unistd.h>     /* getpid / usleep / close */

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

/* sql_query(sql) / sql_query(path, sql) - read-only SELECT against SQLite,
 * returning a list of row maps (same guardrails as the chat sql_query tool:
 * single statement, SELECT only; the db is opened physically read-only). */
static void native_sql_query(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL;
    if (argc == 1) {
        if (!arg_string(vm, args[0], &sql)) return;
    } else if (argc >= 2) {
        if (!arg_string(vm, args[0], &db)) return;
        if (!arg_string(vm, args[1], &sql)) return;
    } else {
        vm_set_error(vm, "sql_query() needs sql, or (path, sql)");
        return;
    }
    sbuf b = {0};
    char err[512] = {0};
    if (sqlite_query_json(db, sql, &b, err, sizeof err) != 0) {
        vm_set_error(vm, "sql_query: %s", err[0] ? err : "failed");
        free(b.p);
        *out = val_null();
        return;
    }
    char jerr[256] = {0};
    json_parse(vm, b.p ? b.p : "[]", jerr, sizeof jerr);
    free(b.p);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

/* sql_write(sql) / sql_write(path, sql) - guarded write: INSERT / UPDATE /
 * DELETE (UPDATE/DELETE must carry WHERE) or CREATE TABLE for a new table;
 * DROP/ALTER/TRUNCATE/VACUUM/ATTACH/PRAGMA and portfolio-mirror writes are
 * rejected. Returns the number of rows affected (0 for DDL). */
static void native_sql_write(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL;
    if (argc == 1) {
        if (!arg_string(vm, args[0], &sql)) return;
    } else if (argc >= 2) {
        if (!arg_string(vm, args[0], &db)) return;
        if (!arg_string(vm, args[1], &sql)) return;
    } else {
        vm_set_error(vm, "sql_write() needs sql, or (path, sql)");
        return;
    }
    int affected = 0;
    char err[512] = {0};
    if (sqlite_write_exec(db, sql, &affected, err, sizeof err) != 0) {
        vm_set_error(vm, "sql_write: %s", err[0] ? err : "failed");
        *out = val_null();
        return;
    }
    *out = val_int(affected);
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
 * fopen actually succeeds" latent bug.
 * Atomic: the payload goes to a sibling .tmp.<pid> file which is then
 * rename()d over the target. A crash mid-write can never leave a truncated
 * file behind — product tools overwrite portfolio.json / .env through this
 * and a torn write would otherwise destroy the only copy of the data. */
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
    if (strlen(p) + 32 >= 4096) { *out = val_bool(false); return; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", p, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { *out = val_bool(false); return; }
    /* 数据文件默认 0600(账本/周报/设置 .env 都经此写;即使用户把
     * IQUEST_REPORTS_DIR 指到 .data 之外,报告也不会随 umask 落成 0644) */
    fchmod(fileno(f), 0600);
    size_t wrote = data && len ? fwrite(data, 1, len, f) : 0;
    int ok = (fclose(f) == 0) && (wrote == len);
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) remove(tmp);
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

/* ---- advisory file lock (single lock per VM process) ---- */

/* flock(2)-based mutual exclusion for product data files: invest.lume wraps
 * its read-modify-write of .data/portfolio.json in lock_file/unlock_file so
 * two workers cannot lose an update to each other. The lock is held on the
 * open fd; it is released automatically when the process dies (no stale lock
 * files to clean up). One lock per VM process: acquiring again replaces the
 * previous lock, which is enough for the single-ledger pattern. */
static int g_lock_fd = -1;

static void native_lock_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "lock_file() needs a path"); return; }
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    long wait_ms = 2000;
    if (argc >= 2 && IS_NUM(args[1])) {
        wait_ms = (long)AS_NUM(args[1]);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 30000) wait_ms = 30000;
    }
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
    int fd = open(p, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { *out = val_bool(false); return; }
    struct timeval t0;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) break;
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd);
            *out = val_bool(false);
            return;
        }
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000 +
                          (now.tv_usec - t0.tv_usec) / 1000;
        if (elapsed_ms >= wait_ms) {
            close(fd);
            *out = val_bool(false);
            return;
        }
        usleep(25000); /* 25 ms backoff */
    }
    g_lock_fd = fd;
    *out = val_bool(true);
}

static void native_unlock_file(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
    *out = val_bool(true);
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

/* ---- collection tools: range / map / filter / reduce ----
 * map/filter/reduce take a DSL function (named func or a lambda) and drive
 * it through the shared call machinery (call_function), so results, GC
 * rooting and `return` unwinding behave exactly like a regular call. */

static bool fn_arg(Value v) {
    return IS_OBJ(v) && (AS_OBJ(v)->type == OBJ_FUNC || AS_OBJ(v)->type == OBJ_NATIVE);
}

/* Call a DSL function with `argc` args (array), returning its result and
 * leaving the VM stack balanced (call_function leaves an error-null on
 * failure; drop it). */
static Value call_dsl_fn(VM *vm, Value fn, Value *args, int argc) {
    vm_push(vm, fn);
    for (int i = 0; i < argc; i++) vm_push(vm, args[i]);
    call_function(vm, fn, argc);
    if (vm->error) return vm_pop(vm); /* the error-result null */
    return vm_pop(vm);
}

/* range(stop) / range(start, stop) / range(start, stop, step) -> list.
 * Whole numbers stay ints so `range(3)` prints [0, 1, 2], not [0.0, ...]. */
static void native_range(VM *vm, int argc, Value *args, Value *out) {
    double a = 0, b, step = 1;
    if (argc < 1) { vm_set_error(vm, "range() needs a stop or (start, stop)"); return; }
    if (argc == 1) {
        if (!IS_NUM(args[0])) { vm_set_error(vm, "range() argument must be a number"); return; }
        b = AS_NUM(args[0]);
    } else {
        if (!IS_NUM(args[0]) || !IS_NUM(args[1])) {
            vm_set_error(vm, "range() arguments must be numbers");
            return;
        }
        a = AS_NUM(args[0]); b = AS_NUM(args[1]);
    }
    if (argc >= 3) {
        if (!IS_NUM(args[2])) { vm_set_error(vm, "range() step must be a number"); return; }
        step = AS_NUM(args[2]);
        if (step == 0) { vm_set_error(vm, "range() step must not be zero"); return; }
    }
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    if (step > 0) {
        for (double v = a; v < b; v += step)
            list_push(vm, list, v == (long long)v ? val_int((long long)v)
                                                  : val_num(v));
    } else {
        for (double v = a; v > b; v += step)
            list_push(vm, list, v == (long long)v ? val_int((long long)v)
                                                  : val_num(v));
    }
    *out = vm_pop(vm);
}

/* map(fn, list) -> new list of fn(item) for each item. */
static void native_map(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "map() expects (fn, list)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Obj *dst = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)dst)); /* root while filling */
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value one = src->as.list.items[i];
        Value r = call_dsl_fn(vm, fn, &one, 1);
        if (vm->error) break;
        list_push(vm, dst, r);
    }
    *out = vm_pop(vm);
}

/* filter(fn, list) -> new list of items where fn(item) is truthy. */
static void native_filter(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "filter() expects (fn, list)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Obj *dst = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)dst)); /* root while filling */
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value one = src->as.list.items[i];
        Value keep = call_dsl_fn(vm, fn, &one, 1);
        if (vm->error) break;
        if (value_truthy(keep)) list_push(vm, dst, one);
    }
    *out = vm_pop(vm);
}

/* reduce(fn, list, init) -> fn(acc, item) folded left over the list. */
static void native_reduce(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 3 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "reduce() expects (fn, list, init)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Value acc = args[2];
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value pair[2] = { acc, src->as.list.items[i] };
        acc = call_dsl_fn(vm, fn, pair, 2);
    }
    *out = acc;
}

Value vm_native(VM *vm, int argc, Value *args, Native2 impl) {
    Value out = val_null();
    impl(vm, argc, args, &out);
    return out;
}

Value b_run(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    bridge_run(vm);
    return val_null();
}
Value b_print(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_print); }
Value b_str(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_str); }
Value b_int(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_int); }
Value b_float(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_float); }
Value b_bool(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bool); }
Value b_string(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_str); }
Value b_len(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_len); }
Value b_keys(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_keys); }
Value b_get(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_get); }
Value b_range(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_range); }
Value b_map(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_map); }
Value b_filter(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_filter); }
Value b_reduce(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_reduce); }
Value b_sql_query(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sql_query); }
Value b_sql_write(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sql_write); }
Value b_json(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_json); }
Value b_stringify(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_stringify); }
Value b_now(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_now); }
Value b_env(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_env); }
Value b_files(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_files); }
Value b_read_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_read_file); }
Value b_write_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_write_file); }
Value b_mkdir(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mkdir); }
Value b_lock_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_lock_file); }
Value b_unlock_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_unlock_file); }
Value b_strftime(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_strftime); }
Value b_put(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_put); }
Value b_tools(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_tools); }
Value b_skills(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_skills); }
Value b_mcps(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mcps); }
Value b_discovery_endpoints(VM *vm, int argc, Value *args)
{ return vm_native(vm, argc, args, native_discovery_endpoints); }
Value b_catalog(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_catalog); }
Value b_default_route(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_default_route); }
