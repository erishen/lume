#!/usr/bin/env python3
"""Migrate the invest JSON ledger (.data/portfolio.json) into the SQLite
database served by the sqlite MCP server (.data/lume.db).

Why: the lume DSL has no SQL builtins, so the typed domain tools
(portfolio_add/remove) keep writing the JSON ledger. This script makes that
same data queryable via the read-only SQL MCP tools (read_query /
list_tables / describe_table) — the model can then run analyses (concentration,
cost basis, history) without touching writes.

Idempotent: holdings are upserted by symbol; deleted symbols are removed.
Run it again after any portfolio_add/remove to refresh the SQLite copy.

Usage:
    .venv-sqlite/bin/python tools/sqlite-migrate.py [--db .data/lume.db]
"""

import argparse
import json
import os
import sqlite3
import sys

DB_DEFAULT = os.path.join(".data", "lume.db")
LEDGER = os.path.join(".data", "portfolio.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", default=DB_DEFAULT, help="SQLite db path")
    args = ap.parse_args()

    if not os.path.exists(LEDGER):
        print(f"[sqlite-migrate] {LEDGER} 不存在，跳过（首次使用无需迁移）")
        return 0

    raw = open(LEDGER, encoding="utf-8").read()
    try:
        ledger = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"[sqlite-migrate] 账本解析失败，终止: {e}", file=sys.stderr)
        return 1

    holdings = ledger.get("holdings", {})
    updated = ledger.get("updated", 0)
    os.makedirs(os.path.dirname(args.db) or ".", exist_ok=True)

    conn = sqlite3.connect(args.db)
    cur = conn.cursor()
    cur.execute(
        """CREATE TABLE IF NOT EXISTS portfolio (
            symbol   TEXT PRIMARY KEY,
            name     TEXT,
            units    REAL NOT NULL,
            avg_cost REAL NOT NULL
        )"""
    )
    cur.execute("CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT)")

    for sym, h in holdings.items():
        cur.execute(
            "INSERT INTO portfolio(symbol, name, units, avg_cost) VALUES(?,?,?,?) "
            "ON CONFLICT(symbol) DO UPDATE SET name=excluded.name, units=excluded.units, avg_cost=excluded.avg_cost",
            (sym, h.get("name", ""), float(h.get("units", 0)), float(h.get("avg_cost", 0))),
        )
    # symbols that disappeared from the ledger are stale
    cur.execute("DELETE FROM portfolio WHERE symbol NOT IN (%s)" % ",".join("?" * len(holdings)),
                list(holdings.keys())) if holdings else cur.execute("DELETE FROM portfolio")
    cur.execute("INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                ("ledger_updated", str(updated)))

    conn.commit()
    n = cur.execute("SELECT COUNT(*) FROM portfolio").fetchone()[0]
    conn.close()
    print(f"[sqlite-migrate] 已同步 {len(holdings)} 个持仓 -> {args.db}（表 portfolio 共 {n} 行）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
