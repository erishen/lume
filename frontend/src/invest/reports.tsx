/* Invest 助手 — 周报归档页。
 *
 * 数据来自 iquest 的 C 路由:GET /api/reports(列表)、GET /api/reports/<name>
 * (正文)。列表点击后在右侧(或下方)用共享 markdown 渲染正文。
 *
 * Build:  cd frontend && pnpm run build:reports   -> ../www/reports.js
 */
import React from "react";
import { createRoot } from "react-dom/client";
import { renderMarkdown } from "./markdown";
import { Report, ReportBody, fetchJson, fmtSize, fmtDate, titleDate } from "./report-utils";

function Reports(): React.ReactElement {
  const [list, setList] = React.useState<Report[]>([]);
  const [error, setError] = React.useState("");
  const [loading, setLoading] = React.useState(true);
  const [selected, setSelected] = React.useState<Report | null>(null);
  const [body, setBody] = React.useState<ReportBody | null>(null);
  const [bodyError, setBodyError] = React.useState("");

  React.useEffect(() => {
    fetchJson<{ reports: Report[] }>("/api/reports")
      .then((d) => {
        setList(d.reports || []);
        setLoading(false);
        if (d.reports && d.reports.length > 0) onPick(d.reports[0]);
      })
      .catch((e: Error) => {
        setLoading(false);
        setError(e.message);
      });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function onPick(r: Report): Promise<void> {
    setSelected(r);
    setBody(null);
    setBodyError("");
    try {
      setBody(await fetchJson<ReportBody>("/api/reports/" + encodeURIComponent(r.name)));
    } catch (e) {
      setBodyError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <main className="mx-auto max-w-6xl px-6 py-10">
      <h1 className="mb-1 text-3xl font-bold text-accent">周报归档</h1>
      <p className="mb-6 text-sm text-muted">
        weekly-investment 管线产出的历史周报,按修改时间倒序。
      </p>

      {error && (
        <div className="mb-4 rounded-lg border border-red-400/40 bg-red-500/10 px-4 py-3 text-sm text-red-300">
          {error}
        </div>
      )}

      <div className="grid gap-6 lg:grid-cols-[320px_1fr]">
        <aside className="space-y-2">
          {loading && <p className="text-sm text-muted">加载中…</p>}
          {!loading && list.length === 0 && (
            <p className="text-sm text-muted">还没有周报。去聊天页让 agent 跑一轮 weekly-investment。</p>
          )}
          {list.map((r) => {
            const active = selected && selected.name === r.name;
            return (
              <button
                key={r.name}
                onClick={() => void onPick(r)}
                className={
                  "block w-full rounded-xl border px-4 py-3 text-left transition-colors " +
                  (active
                    ? "border-accent bg-accent/10"
                    : "border-line bg-panel hover:border-accent/50")
                }
              >
                <div className="flex items-baseline justify-between gap-2">
                  <span className="truncate text-sm font-semibold text-ink">
                    {fmtDate(r.date)}
                  </span>
                  <span className="shrink-0 text-xs text-muted">{fmtSize(r.size)}</span>
                </div>
                <div className="mt-0.5 text-xs text-muted">{r.model}</div>
                <div className="mt-1.5 line-clamp-2 text-[13px] leading-snug text-muted">
                  {r.preview}
                </div>
              </button>
            );
          })}
        </aside>

        <section className="min-w-0">
          {bodyError && (
            <div className="mb-4 rounded-lg border border-red-400/40 bg-red-500/10 px-4 py-3 text-sm text-red-300">
              {bodyError}
            </div>
          )}
          {!selected && !loading && !error && (
            <div className="rounded-xl border border-line bg-panel p-8 text-center text-sm text-muted">
              从左侧选择一篇周报
            </div>
          )}
          {selected && !body && !bodyError && (
            <div className="rounded-xl border border-line bg-panel p-8 text-center text-sm text-muted">
              加载正文…
            </div>
          )}
          {body && (
            <article className="report-article">
              <div className="report-head">
                <h2 className="report-date">{titleDate(body.name)}</h2>
                <div className="report-meta">
                  {body.name} · {fmtSize(body.size)} · {body.mtime}
                </div>
              </div>
              <div className="md-body mx-auto max-w-[70ch]">{renderMarkdown(body.body)}</div>
            </article>
          )}
        </section>
      </div>
    </main>
  );
}

const host = document.getElementById("report-root");
if (host) createRoot(host).render(<Reports />);