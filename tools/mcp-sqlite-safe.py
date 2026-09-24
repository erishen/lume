#!/usr/bin/env python3
"""Restricted SQLite MCP server for lume.

Same tool surface as the official mcp-server-sqlite, but with hard write
guardrails that the official package lacks (its write_query does NOT block
DROP/ALTER — verified empirically):

- write_query  allows only INSERT / UPDATE / DELETE; UPDATE/DELETE must carry
  a WHERE clause (no full-table rewrites)
- create_table allows only CREATE TABLE (a new table; existing-name creates
  fail naturally); no other DDL
- DROP / ALTER / TRUNCATE / VACUUM / ATTACH / DETACH / REINDEX / PRAGMA /
  GRANT / REVOKE / COPY are rejected outright
- the `portfolio` mirror table is read-only: any write touching a table named
  portfolio is rejected (the authoritative ledger is .data/portfolio.json;
  the mirror is re-seeded on every `make invest` by tools/sqlite-migrate.py)

Usage (stdio, newline JSON-RPC, same framing agent-httpd's mcp.c expects):
    .venv-sqlite/bin/python tools/mcp-sqlite-safe.py [--db-path .data/lume.db]
"""

import argparse
import re
import sqlite3
from typing import Any

from mcp.server import Server
from mcp.types import TextContent, Tool

DANGEROUS = re.compile(
    r"^\s*(drop|alter|truncate|vacuum|attach|detach|reindex|pragma|grant|revoke|copy)\b",
    re.IGNORECASE,
)
WRITE_ALLOWED = re.compile(r"^\s*(insert|update|delete)\b", re.IGNORECASE)
HAS_WHERE = re.compile(r"\bwhere\b", re.IGNORECASE)
PORTFOLIO = re.compile(r"\bportfolio\b", re.IGNORECASE)

CONN: sqlite3.Connection | None = None


def conn(db: str) -> sqlite3.Connection:
    global CONN
    if CONN is None:
        CONN = sqlite3.connect(db)
        CONN.row_factory = sqlite3.Row
    return CONN


def guard(sql: str, tool: str) -> str | None:
    """Return an error message if the statement must not run, else None."""
    if DANGEROUS.search(sql):
        return f"{tool}: rejected — dangerous statement (DROP/ALTER/TRUNCATE/…)"
    if PORTFOLIO.search(sql):
        return f"{tool}: rejected — the portfolio mirror table is read-only"
    if tool == "write_query":
        if not WRITE_ALLOWED.match(sql):
            return "write_query: only INSERT / UPDATE / DELETE allowed"
        if WRITE_ALLOWED.match(sql).group(1).lower() in ("update", "delete") and not HAS_WHERE.search(sql):
            return "write_query: UPDATE/DELETE must include a WHERE clause"
    return None


def strip_comments(sql: str) -> str:
    """Strip leading SQL comments so the statement-type check starts at real SQL."""
    s = sql
    for _ in range(64):
        t = s.lstrip()
        if t.startswith("--"):
            nl = t.find("\n")
            s = "" if nl < 0 else t[nl + 1 :]
        elif t.startswith("/*"):
            end = t.find("*/")
            if end < 0:
                return ""
            s = t[end + 2 :]
        else:
            break
    return s


def validate_read(sql: str) -> str | None:
    """Return an error if the statement is not a safe single read-only SELECT."""
    t = (sql or "").strip()
    if not t:
        return "read_query: empty SQL"
    if not t.endswith(";") and ";" in t:
        return "read_query: multiple statements are not allowed"
    body = strip_comments(t)
    if not re.match(r"^\s*select\b", body, re.IGNORECASE):
        return "read_query: only SELECT statements are allowed"
    try:
        conn(DB_PATH).execute(body)
        return None
    except sqlite3.Error as e:
        return f"read_query: {e}"


def rows_text(cur: sqlite3.Cursor) -> str:
    cols = [d[0] for d in cur.description] if cur.description else []
    out = [dict(zip(cols, r)) for r in cur.fetchall()]
    return str(out)


server = Server("sqlite-safe")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(name="list_tables", description="List all tables in the SQLite database",
             inputSchema={"type": "object", "properties": {}}),
        Tool(name="describe_table", description="Show the schema (columns/types) of a table",
             inputSchema={"type": "object", "properties": {"table_name": {"type": "string"}},
                          "required": ["table_name"]}),
        Tool(name="read_query", description="Run a read-only SELECT query (single statement only; no writes, no DDL, no PRAGMA)",
             inputSchema={"type": "object", "properties": {"query": {"type": "string"}},
                          "required": ["query"]}),
        Tool(name="write_query", description="Run a write query: INSERT / UPDATE / DELETE only (WHERE required for UPDATE/DELETE); DDL and the portfolio table are blocked",
             inputSchema={"type": "object", "properties": {"query": {"type": "string"}},
                          "required": ["query"]}),
        Tool(name="create_table", description="Create a new table with CREATE TABLE (existing name fails naturally)",
             inputSchema={"type": "object", "properties": {"query": {"type": "string"}},
                          "required": ["query"]}),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    db = DB_PATH
    try:
        if name == "list_tables":
            c = conn(db).execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")
            return [TextContent(type="text", text=str([r[0] for r in c.fetchall()]))]
        if name == "describe_table":
            t = (arguments or {}).get("table_name", "")
            if not t:
                return [TextContent(type="text", text="describe_table: missing table_name", isError=True)]
            safe = t.replace('"', '""')
            c = conn(db).execute('PRAGMA table_info("%s")' % safe)
            return [TextContent(type="text", text=str([dict(r) for r in c.fetchall()]))]
        sql = (arguments or {}).get("query", "")
        if not sql:
            return [TextContent(type="text", text=f"{name}: missing query", isError=True)]
        if name == "read_query":
            err = validate_read(sql)
            if err:
                return [TextContent(type="text", text=err, isError=True)]
            c = conn(db).execute(sql)
            return [TextContent(type="text", text=rows_text(c))]
        if name in ("write_query", "create_table"):
            err = guard(sql, name)
            if err:
                return [TextContent(type="text", text=err, isError=True)]  # isError=True: 拦截必须被模型识别为失败
            c = conn(db).execute(sql)
            conn(db).commit()
            return [TextContent(type="text", text=str([{"affected_rows": c.rowcount}]) if name == "write_query" else "Table created successfully")]
        return [TextContent(type="text", text=f"unknown tool: {name}", isError=True)]
    except sqlite3.Error as e:
        return [TextContent(type="text", text=f"Database error: {e}", isError=True)]


DB_PATH = ".data/lume.db"


async def main() -> None:
    global DB_PATH
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db-path", default=DB_PATH)
    args, _ = ap.parse_known_args()
    DB_PATH = args.db_path
    from mcp.server import NotificationOptions
    from mcp.server.stdio import stdio_server
    opts = server.create_initialization_options(notification_options=NotificationOptions())
    async with stdio_server() as (r, w):
        await server.run(r, w, opts)


if __name__ == "__main__":
    import asyncio

    asyncio.run(main())
