/* Windows stand-ins for agent-httpd's tool / skill registries.
 *
 * lang/builtins_catalog.c implements the discovery builtins — tools(),
 * skills(), mcps(), catalog() — by reading agent-httpd's tool and skill tables
 * (tools_count / tools_get / skills_count / skills_get, from libagenthttpd.a).
 * The Windows build links no agent-httpd, so those four accessors have to come
 * from somewhere; the types they return (ToolDef / SkillInfo) are declared in
 * lang/builtins_internal.h under _WIN32 with the same field layout.
 *
 * There is nothing to register: those tables are populated by an agent process
 * that indexes MCP tools and SKILL.md roots at start-up, and this build has
 * neither. So the registry is empty by construction — count 0, and get() never
 * called because the loops are bounded by count.
 *
 * Empty rather than removed on purpose: tools() / skills() / mcps() / catalog()
 * must keep working and keep returning empty lists, so a script that reads them
 * behaves identically on both platforms instead of failing only on Windows.
 */

#include "builtins_internal.h"

int tools_count(void) { return 0; }

const ToolDef *tools_get(int i) { (void)i; return NULL; }

int skills_count(void) { return 0; }

const SkillInfo *skills_get(int i) { (void)i; return NULL; }