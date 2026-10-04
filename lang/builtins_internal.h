#ifndef LUME_BUILTINS_INTERNAL_H
#define LUME_BUILTINS_INTERNAL_H

#include "builtins.h"
#include "minijson.h"
#include "tools.h"
#include "skills.h"
#include "sqlite_tool.h"

/* 内建函数实现的跨文件共享声明(builtins.c / builtins_sql.c /
 * builtins_fs.c / builtins_catalog.c / builtins_hof.c)。
 * native_* 是各片的实现,b_* 包装(builtins.c)与片间 helper
 * (list_push / 排序比较器)都经此声明;仅片内自用的 static 不在此列。 */

/* ---- 基础 helper(builtins.c) ---- */
bool arg_string(VM *vm, Value v, const char **out);
void str_of_value(VM *vm, Value v, Value *out);
int value_from_map(VM *vm, int argc, Value *args, Value *v);

/* ---- 列表 / 排序 helper(builtins_fs.c) ---- */
void list_push(VM *vm, Obj *list, Value v);
int str_entry_cmp(const void *a, const void *b);
int skill_entry_cmp(const void *a, const void *b);

/* ---- SQL(builtins_sql.c) ---- */
void native_sql_query(VM *vm, int argc, Value *args, Value *out);
void native_sql_write(VM *vm, int argc, Value *args, Value *out);

/* ---- 文件 / 环境 / 锁 / 时间(builtins_fs.c) ---- */
void native_env(VM *vm, int argc, Value *args, Value *out);
void native_files(VM *vm, int argc, Value *args, Value *out);
void native_read_file(VM *vm, int argc, Value *args, Value *out);
void native_write_file(VM *vm, int argc, Value *args, Value *out);
void native_mkdir(VM *vm, int argc, Value *args, Value *out);
void native_lock_file(VM *vm, int argc, Value *args, Value *out);
void native_unlock_file(VM *vm, int argc, Value *args, Value *out);
void native_strftime(VM *vm, int argc, Value *args, Value *out);
void native_put(VM *vm, int argc, Value *args, Value *out);

/* ---- 工具 / 技能 / MCP / 目录(builtins_catalog.c) ---- */
void native_tools(VM *vm, int argc, Value *args, Value *out);
void native_skills(VM *vm, int argc, Value *args, Value *out);
void native_mcps(VM *vm, int argc, Value *args, Value *out);
void native_discovery_endpoints(VM *vm, int argc, Value *args, Value *out);
void native_catalog(VM *vm, int argc, Value *args, Value *out);

/* ---- 高阶集合函数(builtins_hof.c) ---- */
void native_range(VM *vm, int argc, Value *args, Value *out);
void native_map(VM *vm, int argc, Value *args, Value *out);
void native_filter(VM *vm, int argc, Value *args, Value *out);
void native_reduce(VM *vm, int argc, Value *args, Value *out);

#endif /* LUME_BUILTINS_INTERNAL_H */
