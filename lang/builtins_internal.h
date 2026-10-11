#ifndef LUME_BUILTINS_INTERNAL_H
#define LUME_BUILTINS_INTERNAL_H

#include "builtins.h"
#ifdef _WIN32
/* Windows does not link libagenthttpd.a (agent-httpd's sources are POSIX
 * socket code with no Windows support), so the agent-httpd headers that ride
 * along with it cannot be used here. What this file's slices actually need
 * from that bundle is only the `sbuf` string buffer, and lang/sbuf.h is the
 * same struct with the same always-NUL-terminated + sticky-oom contract,
 * inlined — so it stands in for minijson.h.
 *
 * The reader half (jfind_value / jread_string) and the tool/skill/sqlite
 * registries are NOT provided: the slices that need them (iquest.c,
 * builtins_sql.c, bridge.c) are excluded from the Windows build by the
 * Makefile's $(IS_WINDOWS) branch, which links lang/bridge_stub.c instead. */
#include "sbuf.h"
#else
#include "minijson.h"
#include "tools.h"
#include "skills.h"
#include "sqlite_tool.h"
#endif

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

/* ---- 文件 / 环境 / 锁 / 时间(builtins_fs.c) ---- */
void native_env(VM *vm, int argc, Value *args, Value *out);
void native_argv(VM *vm, int argc, Value *args, Value *out);
void native_files(VM *vm, int argc, Value *args, Value *out);
void native_read_file(VM *vm, int argc, Value *args, Value *out);
void native_read_stdin(VM *vm, int argc, Value *args, Value *out);
void native_write_file(VM *vm, int argc, Value *args, Value *out);
void native_mkdir(VM *vm, int argc, Value *args, Value *out);
void native_lock_file(VM *vm, int argc, Value *args, Value *out);
void native_unlock_file(VM *vm, int argc, Value *args, Value *out);
void native_strftime(VM *vm, int argc, Value *args, Value *out);
void native_put(VM *vm, int argc, Value *args, Value *out);

/* ---- SQL(builtins_sql.c, host-only) ---- */
void native_sql_query(VM *vm, int argc, Value *args, Value *out);
void native_sql_write(VM *vm, int argc, Value *args, Value *out);

/* ---- 工具 / 技能 / MCP / 目录(builtins_catalog.c) ---- */
#ifdef _WIN32
/* ---- 工具 / 技能目录:Windows 自持注册表(替 agent-httpd 的 tools/skills) ----
 * tools() / skills() / mcps() / catalog() keep the same surface here, but the
 * tables behind them are this build's own: this build links no agent-httpd, so
 * there is no agent tool/skill registry to read out of a host static library.
 * Nothing registers, so the counts stay 0 and those builtins report empty
 * lists — the alternative (deleting them) would fork the language surface
 * between platforms. Field layouts mirror agent-httpd's ToolDef / SkillInfo so
 * builtins_catalog.c compiles unchanged against either header.
 * SKILL_DESC_MAX is the /discovery description cap; catalog() carries full text. */
#ifndef TOOL_MAX
#define TOOL_MAX   96
#endif
#ifndef SKILL_DESC_MAX
#define SKILL_DESC_MAX 160
#endif

typedef struct {
    char name[65];
    char desc[161];
    char params[4097];/* JSON schema, "{}" when unknown */
} ToolDef;

typedef struct {
    char name[65];
    char desc[161];
    char path[4096];
} SkillInfo;

int tools_count(void);
const ToolDef *tools_get(int i);
int skills_count(void);
const SkillInfo *skills_get(int i);
#endif /* _WIN32 */
/* POSIX host build: ToolDef / SkillInfo / TOOL_MAX come from agent-httpd's
 * tools.h / skills.h above, and tools_count() / tools_get() / skills_*() are
 * implemented in agent-httpd's tools.c (libagenthttpd.a) — the registry block
 * lume-core carries here lives in its own builtins_catalog.c instead, which
 * the host keeps host-owned for exactly this reason. */

void native_tools(VM *vm, int argc, Value *args, Value *out);
void native_skills(VM *vm, int argc, Value *args, Value *out);
void native_mcps(VM *vm, int argc, Value *args, Value *out);
void native_discovery_endpoints(VM *vm, int argc, Value *args, Value *out);
void native_catalog(VM *vm, int argc, Value *args, Value *out);

/* ---- 出站 HTTP(builtins_http.c) ---- */
/* http_get(url, {headers?, timeout?, max_bytes?, allow_private?}) and
   http_post/put/patch(url, {headers?, body?, timeout?, max_bytes?,
   allow_private?}) / http_delete(url, ... same as get) ->
   {ok, status, body, err}. Raw sockets on purpose: this tree only links libc
   (plus the optional libLLVM and libssl), so there is no libcurl and the
   agent-httpd fetch_url in the submodule cannot be reached from here. The
   SSRF model mirrors the host one (public-only unless allow_private).
   Carrying methods add Content-Length and emit the body after the header
   block; 301/302/303 downgrade a body-carrying method to GET and drop
   Content-Length (RFC 7231 6.4.4), 307/308 keep method and body. */
void native_http_get(VM *vm, int argc, Value *args, Value *out);
void native_http_post(VM *vm, int argc, Value *args, Value *out);
void native_http_put(VM *vm, int argc, Value *args, Value *out);
void native_http_patch(VM *vm, int argc, Value *args, Value *out);
void native_http_delete(VM *vm, int argc, Value *args, Value *out);

/* ---- 高阶集合函数(builtins_hof.c) ---- */
void native_range(VM *vm, int argc, Value *args, Value *out);
void native_map(VM *vm, int argc, Value *args, Value *out);
void native_filter(VM *vm, int argc, Value *args, Value *out);
void native_reduce(VM *vm, int argc, Value *args, Value *out);

#endif /* LUME_BUILTINS_INTERNAL_H */
