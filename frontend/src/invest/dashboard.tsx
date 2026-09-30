/* Invest 助手 — 首页仪表盘。
 *
 * 顶部三张入口卡(Agent/归档/设置)· 最近周报快捷列表 · 最新一篇周报全文
 * 直接用共享 renderMarkdown 内联渲染(不再跳转到 /reports)。
 *
 * Build:  cd frontend && pnpm run build:home   -> ../www/dashboard.js
 */
import React from "react";
import { createRoot } from "react-dom/client";
import { renderMarkdown } from "./markdown";
import { Report, ReportBody, fetchJson, fmtSize, titleDate } from "./report-utils";

type Settings = {
  allow_paid: boolean;
  provider: string;
};

function Home(): React.ReactElement {
  const [s, setS] = React.useState<Settings | null>(null);
  const [reports, setReports] = React.useState<Report[]>([]);
  const [loading, setLoading] = React.useState(true);
  const [error, setError] = React.useState("");
  const [selected, setSelected] = React.useState<Report | null>(null);
  const [body, setBody] = React.useState<ReportBody | null>(null);
  const [bodyError, setBodyError] = React.useState("");

  React.useEffect(() => {
    let alive = true;
    fetchJson<Settings>("/api/settings")
      .then((d) => alive && setS(d))
      .catch(() => {
        /* 设置读不到不影响周报展示 */
      });
    fetchJson<{ reports: Report[] }>("/api/reports")
      .then((d) => {
        if (!alive) return;
        const list = d.reports || [];
        setReports(list);
        setLoading(false);
        if (list.length > 0) onPick(list[0]);
      })
      .catch((e: Error) => {
        if (!alive) return;
        setLoading(false);
        setError(e.message);
      });
    return () => {
      alive = false;
    };
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

  const gate =
    s === null ? "…" : "审批 " + (s.allow_paid ? "已批准" : "未批准(deepseek 被闸门拦截)") + " · provider " + s.provider;

  return (
    <main className="mx-auto max-w-5xl px-6 py-10">
      <header className="mb-6">
        <h1 className="text-3xl font-bold text-ink">本地投资助手</h1>
        <p className="mt-1 text-sm text-muted">
          聊天对话、周报归档与审批设置的统一入口。数据全部来自本地快照(angest),请勿外发或提交到版本库。
        </p>
      </header>

      <div className="mb-5 rounded-lg border border-amber-400/30 bg-amber-500/5 px-4 py-2.5 text-[13px] text-amber-200">
        使用聊天/周报时,持仓快照与对话文本会发送到已配置的 LLM 服务商(可能位于境外)。
        <a href="/settings#privacy" className="underline">数据出境告知与隐私政策</a>
      </div>

      <div className="grid grid-cols-1 gap-4 md:grid-cols-3">
        <a href="/chat" className="card block hover:border-accent no-underline">
          <h3 className="font-semibold text-ink">Agent 对话</h3>
          <p className="text-sm text-muted">
            weekly-investment 周报管线、portfolio-check 持仓体检、记忆与沙箱文件访问。
          </p>
        </a>
        <a href="/reports" className="card block hover:border-accent no-underline">
          <h3 className="font-semibold text-ink">周报归档</h3>
          <p className="text-sm text-muted">
            {loading ? "…" : reports.length > 0 ? reports.length + " 篇 · 最新 " + titleDate(reports[0].name) : "还没有周报"}
          </p>
        </a>
        <a href="/settings" className="card block hover:border-accent no-underline">
          <h3 className="font-semibold text-ink">设置</h3>
          <p className="text-sm text-muted">{gate}</p>
        </a>
      </div>

      {error && (
        <div className="mb-4 mt-5 rounded-lg border border-red-400/40 bg-red-500/10 px-4 py-3 text-sm text-red-300">
          {error}
        </div>
      )}

      {reports.length > 0 && (
        <section className="mt-6">
          <h2 className="mb-2 text-base font-semibold text-ink">最近周报</h2>
          <div className="flex flex-wrap gap-2">
            {reports.slice(0, 5).map((r) => {
              const active = selected && selected.name === r.name;
              return (
                <button
                  key={r.name}
                  onClick={() => void onPick(r)}
                  className={
                    "rounded-lg border px-3 py-1.5 text-sm transition-colors " +
                    (active
                      ? "border-accent bg-accent/10 font-semibold text-ink"
                      : "border-line bg-panel text-muted hover:border-accent/50")
                  }
                >
                  {titleDate(r.name)} · {r.model}
                </button>
              );
            })}
          </div>
        </section>
      )}

      {!loading && reports.length === 0 && !error && (
        <p className="mt-6 text-sm text-muted">还没有周报。去 Agent 页跑一轮 weekly-investment。</p>
      )}

      <section className="mt-6">
        <h2 className="mb-2 text-base font-semibold text-ink">最新周报全文</h2>
        {bodyError && (
          <div className="mb-4 rounded-lg border border-red-400/40 bg-red-500/10 px-4 py-3 text-sm text-red-300">
            {bodyError}
          </div>
        )}
        {selected && !body && !bodyError && (
          <div className="rounded-xl border border-line bg-panel p-8 text-center text-sm text-muted">加载正文…</div>
        )}
        {body && (
          <article className="report-article">
            <div className="report-head">
              <h3 className="report-date">{titleDate(body.name)}</h3>
              <div className="report-meta">
                {body.name} · {fmtSize(body.size)} · {body.mtime}
              </div>
            </div>
            <div className="md-body mx-auto max-w-[70ch]">{renderMarkdown(body.body)}</div>
          </article>
        )}
        {!selected && !loading && !error && (
          <div className="rounded-xl border border-line bg-panel p-8 text-center text-sm text-muted">
            还没有周报,去 Agent 跑一轮。
          </div>
        )}
      </section>
    </main>
  );
}

const host = document.getElementById("home-root");
if (host) createRoot(host).render(<Home />);