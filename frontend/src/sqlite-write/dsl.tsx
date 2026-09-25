/* sqlite-write DSL demo page — React + TypeScript, bundled by esbuild.
 *
 * The /dsl page used to be rendered server-side (el/render inside the .lume
 * script). This client page replaces that SSR markup: it fetches the same
 * data from the /dsl/data JSON endpoint and renders the two tables client
 * side, so the page demonstrates the sql_query / sql_write builtins through
 * a plain API round-trip. The write-up sample is a static block so the page
 * teaches the call forms even before any data arrives.
 *
 * Styling: shared app.css classes (.hero/.shell/.card/…) + Tailwind
 * utilities. dsl.tsx is listed in app.css @source so the utility classes
 * used here are generated into www/app.css (missing before: the tables and
 * the bare <pre> had no styles at all, which is why the page looked bare).
 */
import { useEffect, useState } from "react";
import { createRoot } from "react-dom/client";

const DSL_EXAMPLE = `// 只读查询:值走 ? 占位符绑定,不拼进 SQL(注入无效)
let holdings = sql_query(
  "select symbol, units, avg_cost from portfolio where units >= ?", [1]);

// 护栏写:UPDATE/DELETE 必须带 WHERE;值同样走 ? 绑定
let n = sql_write(
  "update analysis set cost_value = ? where symbol = ?", [2000, "AAPL"]);

// 幂等建表:表存在则跳过(重复启动不报错)
let has = len(sql_query(
  "select name from sqlite_master where type = 'table' and name = ?",
  ["analysis"]));
if (has == 0) {
  sql_write("create table analysis (symbol text, cost_value real, note text)");
  sql_write("insert into analysis values (?, ?, ?)", ["AAPL", 1500, "dsl-demo"]);
}`;

type Cell = string | number;
interface Row {
  symbol: string;
  units?: number;
  avg_cost?: number;
  cost_value?: number;
  note?: string;
  [key: string]: Cell | undefined;
}
interface DslData {
  holdings: Row[];
  analysis: Row[];
  row_count: number;
}

function DataTable({
  rows,
  cols,
  emptyLabel = "（空）",
}: {
  rows: Row[];
  cols: { key: string; label: string }[];
  emptyLabel?: string;
}) {
  if (rows.length === 0) {
    return <p className="muted py-2">{emptyLabel}</p>;
  }
  return (
    <div className="mt-3 overflow-x-auto">
      <table className="w-full border-collapse text-[13px] leading-snug">
        <thead>
          <tr>
            {cols.map((c) => (
              <th
                key={c.key}
                className="border-b border-line px-3 py-2 text-left font-semibold text-muted"
              >
                {c.label}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((r, i) => (
            <tr key={i} className="odd:bg-bg/45 hover:bg-accent/6">
              {cols.map((c, j) => {
                const v = r[c.key];
                const numeric = typeof v === "number";
                return (
                  <td
                    key={c.key}
                    className={`border-b border-line/70 px-3 py-2 align-top ${
                      numeric ? "text-right tabular-nums" : ""
                    } ${j === 0 ? "font-mono text-sky-300" : ""}`}
                  >
                    {String(v ?? "")}
                  </td>
                );
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function DslPage() {
  const [data, setData] = useState<DslData | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    fetch("/dsl/data")
      .then((r) => (r.ok ? r.json() : Promise.reject("HTTP " + r.status)))
      .then((d: DslData) => alive && setData(d))
      .catch((e: unknown) => alive && setErr(String(e)));
    return () => {
      alive = false;
    };
  }, []);

  return (
    <main className="dsl-page shell">
      <header className="hero chat-hero">
        <h1>DSL 直连 SQL 演示</h1>
        <p>
          脚本层 <code>sql_query</code> / <code>sql_write</code>{" "}
          内建直接查/写 SQLite,不经过 LLM。
        </p>
      </header>

      {/* 数据状态条 + 原始 JSON 入口 */}
      <div className="mb-4 flex flex-wrap items-center gap-2 text-[13px]">
        {err ? (
          <span className="rounded-full border border-red-400/40 bg-red-500/10 px-3 py-1 font-medium text-red-300">
            加载失败:{err}
          </span>
        ) : data ? (
          <span className="rounded-full border border-emerald-400/30 bg-emerald-500/10 px-3 py-1 font-medium text-emerald-300">
            数据已就绪 · 共 {data.row_count} 行
          </span>
        ) : (
          <span className="rounded-full border border-line bg-panel px-3 py-1 text-muted">
            加载中…
          </span>
        )}
        <a
          href="/dsl/data"
          className="ml-auto rounded-full border border-line px-3 py-1 font-medium text-muted no-underline hover:border-accent hover:text-accent"
        >
          原始 JSON ↗
        </a>
      </div>

      {/* 写法示例 */}
      <section className="mb-3.5 rounded-xl border border-line bg-panel p-4.5">
        <h2 className="text-[15px] font-bold text-ink">
          写法示例<span className="ml-2 font-mono text-[11px] font-normal text-muted">脚本层直接调用,不经 LLM</span>
        </h2>
        <pre className="code-panel mt-3 overflow-x-auto rounded-lg border border-line bg-[linear-gradient(180deg,#121a30,#0d1426)] px-4 py-3.5 font-mono text-[12.5px] leading-[1.65] whitespace-pre-wrap text-[#9fe8a2]">
          {DSL_EXAMPLE}
        </pre>
      </section>

      {/* portfolio 镜像表 */}
      <section className="mb-3.5 rounded-xl border border-line bg-panel p-4.5">
        <h2 className="flex items-baseline justify-between text-[15px] font-bold text-ink">
          portfolio 镜像表
          <span className="font-mono text-[11px] font-normal text-muted">只读查询 · {data ? data.holdings.length : "—"} 行</span>
        </h2>
        {err ? (
          <p className="muted py-2">{err}</p>
        ) : data ? (
          <DataTable
            rows={data.holdings}
            cols={[
              { key: "symbol", label: "symbol" },
              { key: "units", label: "units" },
              { key: "avg_cost", label: "avg_cost" },
            ]}
          />
        ) : (
          <p className="muted py-2">加载中…</p>
        )}
      </section>

      {/* analysis 表 */}
      <section className="rounded-xl border border-line bg-panel p-4.5">
        <h2 className="flex items-baseline justify-between text-[15px] font-bold text-ink">
          analysis 表
          <span className="font-mono text-[11px] font-normal text-muted">DSL 幂等建表 + 写入</span>
        </h2>
        {data ? (
          <DataTable
            rows={data.analysis}
            cols={[
              { key: "symbol", label: "symbol" },
              { key: "cost_value", label: "cost_value" },
              { key: "note", label: "note" },
            ]}
            emptyLabel="（暂无数据,示例首次写入时创建）"
          />
        ) : (
          <p className="muted py-2">加载中…</p>
        )}
      </section>
    </main>
  );
}

const root = document.getElementById("dsl-root");
if (root) createRoot(root).render(<DslPage />);
