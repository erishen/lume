/* SQLite 内建:sql_query / sql_write——从 builtins.c 拆出(2026-09-27)。
 * 参数绑定与 ? 占位符只读 SELECT / 受限写执行的 guardrails 见函数注释。 */

#include "builtins_internal.h"
#include "db_layer.h"

/* sql_query(sql) / sql_query(path, sql) - read-only SELECT against SQLite,
 * returning a list of row maps (same guardrails as the chat sql_query tool:
 * single statement, SELECT only; the db is opened physically read-only). */
/* Resolve sql_query / sql_write args: (sql[, params]) or (path, sql[,
 * params]). `params` is a list of scalars bound to ? placeholders (null →
 * SQL NULL); values are converted to text, so they never enter the SQL
 * string and cannot be parsed as SQL. Returns 0 and fills the out-params
 * on success (caller frees *params with free_sql_params). */
int sql_arg_bind(VM *vm, int argc, Value *args, const char **db,
                        const char **sql, const char ***params, int *nparams) {
    *db = NULL; *sql = NULL; *params = NULL; *nparams = 0;
    if (argc < 1 || argc > 3) {
        vm_set_error(vm, "sql needs (sql[, params]) or (path, sql[, params])");
        return 1;
    }
    const char *s0 = NULL, *s1 = NULL;
    if (!arg_string(vm, args[0], &s0)) return 1;
    Value pv = val_null();
    if (argc == 1) {
        *sql = s0;
    } else if (IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_STRING) {
        if (!arg_string(vm, args[1], &s1)) return 1;
        *db = s0; *sql = s1;
        if (argc == 3) pv = args[2];
    } else {
        *sql = s0;
        pv = args[1];
        if (argc == 3) {
            vm_set_error(vm, "sql has too many arguments");
            return 1;
        }
    }
    if (IS_NULL(pv)) return 0; /* no params */
    if (!IS_OBJ(pv) || AS_OBJ(pv)->type != OBJ_LIST) {
        vm_set_error(vm, "params must be a list");
        return 1;
    }
    Obj *o = AS_OBJ(pv);
    int cnt = o->as.list.count;
    const char **arr = calloc((size_t)cnt + 1, sizeof(char *));
    if (!arr) { vm_set_error(vm, "out of memory"); return 1; }
    for (int i = 0; i < cnt; i++) {
        Value e = o->as.list.items[i];
        if (IS_NULL(e)) { arr[i] = NULL; continue; }
        const char *txt = NULL;
        if (IS_OBJ(e) && AS_OBJ(e)->type == OBJ_STRING) {
            txt = obj_string(AS_OBJ(e));
        } else {
            Value sv = val_null();
            str_of_value(vm, e, &sv);
            if (vm->error || !IS_OBJ(sv) || AS_OBJ(sv)->type != OBJ_STRING) {
                vm_set_error(vm, "param %d is not a scalar", i + 1);
                for (int j = 0; j < i; j++) if (arr[j]) free((void *)arr[j]);
                free(arr);
                return 1;
            }
            txt = obj_string(AS_OBJ(sv));
        }
        arr[i] = strdup(txt);
        if (!arr[i]) {
            vm_set_error(vm, "out of memory");
            for (int j = 0; j < i; j++) if (arr[j]) free((void *)arr[j]);
            free(arr);
            return 1;
        }
    }
    *params = arr;
    *nparams = cnt;
    return 0;
}

void free_sql_params(const char **params, int n) {
    if (!params) return;
    for (int i = 0; i < n; i++) if (params[i]) free((void *)params[i]);
    free((void *)params);
}

void native_sql_query(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc, args, &db, &sql, &params, &nparams)) return;
    sbuf b = {0};
    char err[512] = {0};
    if (db_query_json(db, sql, params, nparams, &b, err, sizeof err) != 0) {
        vm_set_error(vm, "sql_query: %s", err[0] ? err : "failed");
        free(b.p);
        free_sql_params(params, nparams);
        *out = val_null();
        return;
    }
    char jerr[256] = {0};
    json_parse(vm, b.p ? b.p : "[]", jerr, sizeof jerr);
    free(b.p);
    free_sql_params(params, nparams);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

/* sql_write(sql) / sql_write(path, sql) - guarded write: INSERT / UPDATE /
 * DELETE (UPDATE/DELETE must carry WHERE) or CREATE TABLE for a new table;
 * DROP/ALTER/TRUNCATE/VACUUM/ATTACH/PRAGMA and portfolio-mirror writes are
 * rejected. Returns the number of rows affected (0 for DDL). */
void native_sql_write(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc, args, &db, &sql, &params, &nparams)) return;
    int affected = 0;
    char err[512] = {0};
    if (db_write_exec(db, sql, params, nparams, &affected, err, sizeof err) != 0) {
        vm_set_error(vm, "sql_write: %s", err[0] ? err : "failed");
        free_sql_params(params, nparams);
        *out = val_null();
        return;
    }
    free_sql_params(params, nparams);
    *out = val_int(affected);
}
