/* sqlite-write DSL demo page — React + TypeScript, bundled by esbuild.
 *
 * The /dsl page used to be rendered server-side (el/render inside the .lume
 * script). This client page replaces that SSR markup: it fetches the same
 * data from the /dsl/data JSON endpoint and renders the two tables client
 * side, so the page demonstrates the sql_query / sql_write builtins through
 * a plain API round-trip. The write-up sample is a static block so the page
 * teaches the call forms even before any data arrives.
 *
 * Styling: shared app.css classes (.hero/.shell/.muted/…) + Tailwind
 * utilities, same as the chat page.
 */
import { useEffect, useState } from "react";
import { createRoot } from "react-dom/client";

const DSL_EXAMPLE = `// 只读查询 → 行 map 列表,可 get()/len()/遍历
let holdings = sql_query("select symbol, units, avg_cost from portfolio order by symbol");

// 护栏写 → 影响行数(DDL 为 0);UPDATE/DELETE 必须带 WHERE
let n = sql_write("update analysis set cost_value = 2000 where symbol = 'AAPL'");

// 幂等建表:表存在则跳过(重复启动不报错)
let has = len(sql_query("select name from sqlite_master where type = 'table' and name = 'analysis'"));
if (has == 0) {
  sql_write("create table analysis (symbol text, cost_value real, note text)");
  sql_write("insert into analysis values ('AAPL', 1500, 'dsl-demo')");
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
}: {
  rows: Row[];
  cols: { key: string; label: string }[];
}) {
  return (
    <table className="w-full border-collapse">
      <thead>
        <tr>
          {cols.map((c) => (
            <th key={c.key} className="text-left border-b border-gray-300 px-2 py-1">
              {c.label}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((r, i) => (
          <tr key={i}>
            {cols.map((c) => (
              <td key={c.key} className="border-b border-gray-200 px-2 py-1">
                {String(r[c.key] ?? "")}
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
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

      <h2>写法示例(脚本层直接调用,不经 LLM)</h2>
      <pre>{DSL_EXAMPLE}</pre>

      <h2>portfolio 镜像表(只读查询)</h2>
      {err ? (
        <p className="muted">加载失败:{err}</p>
      ) : data ? (
        <>
          <DataTable
            rows={data.holdings}
            cols={[
              { key: "symbol", label: "symbol" },
              { key: "units", label: "units" },
              { key: "avg_cost", label: "avg_cost" },
            ]}
          />
          <p className="muted">共 {data.row_count} 行</p>
        </>
      ) : (
        <p className="muted">loading…</p>
      )}

      <h2>analysis 表(DSL 幂等建表 + 写入)</h2>
      {data ? (
        <DataTable
          rows={data.analysis}
          cols={[
            { key: "symbol", label: "symbol" },
            { key: "cost_value", label: "cost_value" },
            { key: "note", label: "note" },
          ]}
        />
      ) : (
        <p className="muted">loading…</p>
      )}

      <p>
        <a href="/dsl/data">查看原始 JSON</a>
      </p>
    </main>
  );
}

const root = document.getElementById("dsl-root");
if (root) createRoot(root).render(<DslPage />);
