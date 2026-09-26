/* ABAC demo UI — React + TypeScript, bundled by esbuild (frontend/src/abac/app.tsx
 * -> www/abac/app.js). Served by examples/abac.lume (:8086) under Basic Auth.
 *
 * 页面做的事:
 *   1. 决策测试器:选 主体/资源/动作 -> GET /decide -> 展示 PERMIT/DENY 与
 *      策略引擎逐步推理记录(reason trace)——"具体怎么做 ABAC"的核心演示。
 *   2. 审计视图:GET /audit -> .data/abac-audit.jsonl 里的决策记录。
 * 认证:页面与 API 都在 htpasswd 保护下,浏览器弹 Basic Auth;同源 fetch
 * 会自动携带已缓存的凭据,401 时提示重新登录。
 *
 * 视觉:深色 token(bg/panel/ink/muted/accent)与 invest/hub 一致;语义类
 * .abac-* 定义在 src/app.css components 层(Tailwind v4 @source 已含本文件)。
 *
 * Build:  cd frontend && pnpm run build     -> ../www/abac/app.js
 * Watch:  cd frontend && pnpm run dev
 */
import React from "react";
import { createRoot } from "react-dom/client";

const SUBJECTS = ["admin", "carol", "alice", "bob"];
const RESOURCES = ["board", "roadmap", "internal", "pipeline"];
const ACTIONS = ["read", "delete", "approve"];

const SUBJECT_ROWS: Array<[string, string, string, string]> = [
  ["admin", "EXEC", "5 · TOP", "CN"],
  ["carol", "ENG", "4 · CONF", "US"],
  ["alice", "ENG", "3 · INT", "CN"],
  ["bob", "SALES", "2 · RESTR", "CN"],
];

const RESOURCE_ROWS: Array<[string, string, string, string]> = [
  ["board", "5 · TOP", "EXEC", "read, approve"],
  ["roadmap", "4 · CONF", "ENG", "read, delete, approve"],
  ["internal", "3 · INT", "ENG", "read, delete"],
  ["pipeline", "2 · RESTR", "SALES", "read, delete"],
];

type Decision = {
  decision: string;
  reason: string;
  subject: string;
  resource: string;
  action: string;
};

type AuditEntry = {
  user?: string;
  resource?: string;
  action?: string;
  decision?: string;
  reason?: string;
  debug?: boolean;
};

function parseAudit(text: string): AuditEntry[] {
  return text
    .split("\n")
    .filter((l) => l.trim())
    .map((l) => {
      try {
        return JSON.parse(l) as AuditEntry;
      } catch {
        return null;
      }
    })
    .filter((e): e is AuditEntry => e !== null);
}

function App() {
  const [user, setUser] = React.useState("carol");
  const [res, setRes] = React.useState("roadmap");
  const [action, setAction] = React.useState("read");
  const [result, setResult] = React.useState<Decision | null>(null);
  const [err, setErr] = React.useState<string>("");
  const [busy, setBusy] = React.useState(false);
  const [audit, setAudit] = React.useState<AuditEntry[] | null>(null);
  const [auditErr, setAuditErr] = React.useState("");

  async function runDecide() {
    setBusy(true);
    setErr("");
    try {
      const r = await window.fetch(
        `/decide?user=${encodeURIComponent(user)}&res=${encodeURIComponent(res)}&action=${encodeURIComponent(action)}`
      );
      if (r.status === 401) throw new Error("未认证：请在浏览器弹出的登录框输入账号密码（admin/admin123 等）");
      if (!r.ok) throw new Error("HTTP " + r.status);
      const d = await r.json();
      setResult(d.result as Decision);
    } catch (e) {
      setErr(e instanceof Error ? e.message : String(e));
      setResult(null);
    } finally {
      setBusy(false);
    }
  }

  async function loadAudit() {
    setAuditErr("");
    try {
      const r = await window.fetch("/audit");
      if (r.status === 401) throw new Error("未认证：请先完成 Basic Auth 登录");
      if (!r.ok) throw new Error("HTTP " + r.status);
      const d = await r.json();
      const entries = parseAudit(d.lines ?? "");
      setAudit(entries.slice(-20).reverse());
    } catch (e) {
      setAuditErr(e instanceof Error ? e.message : String(e));
    }
  }

  const traceSteps = result
    ? result.reason
        .split("] [")
        .map((s) => s.replace(/^\[/, "").replace(/\]\s*$/, ""))
        .filter((s) => s.trim())
    : [];

  return (
    <>
      <nav>
        <a href="/" className="brand">
          Lume
        </a>
        <a href="/" className="active">
          ABAC demo
        </a>
      </nav>

      <main className="abac-page">
        <header className="abac-hero">
          <h1>ABAC · 属性访问控制</h1>
          <p>
            策略就是 Lume 函数，解释器就是策略引擎——不引入 SpEL / XACML / 单独策略服务。
            选主体、资源、动作跑一次 <code className="abac-code">/decide</code>，看决策与逐步推理；
            <code className="abac-code">/audit</code> 展示每次决策的审计留痕（
            <code className="abac-code">.data/abac-audit.jsonl</code>）。
          </p>
        </header>

        {/* 决策测试器 */}
        <section className="abac-panel">
          <h2>决策测试器</h2>
          <div className="flex flex-wrap items-end gap-x-5 gap-y-4">
            {([
              ["主体 subject", user, setUser, SUBJECTS],
              ["资源 resource", res, setRes, RESOURCES],
              ["动作 action", action, setAction, ACTIONS],
            ] as const).map(([label, value, setter, options]) => (
              <label key={label} className="block">
                <span className="abac-label">{label}</span>
                <select
                  className="abac-select"
                  value={value}
                  onChange={(e) => setter(e.target.value)}
                >
                  {options.map((o) => (
                    <option key={o}>{o}</option>
                  ))}
                </select>
              </label>
            ))}
            <div className="flex items-center gap-3">
              <button className="abac-btn" disabled={busy} onClick={runDecide}>
                {busy ? "决策中…" : "运行决策"}
              </button>
              <button className="abac-btn-ghost" onClick={loadAudit}>
                刷新审计
              </button>
            </div>
          </div>

          {err && <p className="mt-4 text-sm text-rose-300">{err}</p>}

          {result && (
            <div
              className={
                "mt-5 rounded-xl border p-4.5 " +
                (result.decision === "PERMIT"
                  ? "border-emerald-400/30 bg-emerald-400/5"
                  : "border-rose-400/30 bg-rose-400/5")
              }
            >
              <div className="flex items-center gap-3">
                <span
                  className={
                    "abac-badge " +
                    (result.decision === "PERMIT"
                      ? "abac-badge-permit"
                      : "abac-badge-deny")
                  }
                >
                  {result.decision}
                </span>
                <span className="font-mono text-[13.5px] text-ink">
                  {result.subject} → {result.resource} : {result.action}
                </span>
              </div>
              {traceSteps.length > 0 && (
                <ol className="abac-trace">
                  {traceSteps.map((s, i) => (
                    <li key={i}>{s}</li>
                  ))}
                </ol>
              )}
            </div>
          )}
        </section>

        {/* 属性表 */}
        <div className="abac-grid">
          <section className="abac-panel">
            <h2>主体属性 · subjects</h2>
            <table className="abac-table">
              <thead>
                <tr>
                  <th>账号</th>
                  <th>部门</th>
                  <th>clearance</th>
                  <th>区域</th>
                </tr>
              </thead>
              <tbody>
                {SUBJECT_ROWS.map((r) => (
                  <tr key={r[0]}>
                    <td className="font-mono">{r[0]}</td>
                    <td>{r[1]}</td>
                    <td>{r[2]}</td>
                    <td>{r[3]}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </section>

          <section className="abac-panel">
            <h2>资源属性 · resources</h2>
            <table className="abac-table">
              <thead>
                <tr>
                  <th>资源</th>
                  <th>clearance</th>
                  <th>部门</th>
                  <th>允许动作</th>
                </tr>
              </thead>
              <tbody>
                {RESOURCE_ROWS.map((r) => (
                  <tr key={r[0]}>
                    <td className="font-mono">{r[0]}</td>
                    <td>{r[1]}</td>
                    <td>{r[2]}</td>
                    <td>{r[3]}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </section>
        </div>

        <p className="abac-footnote">
          决策链（任一 DENY 即拒绝，deny-by-default）：主体未知 → 资源未知 → 动作不允许 → clearance
          → 部门（EXEC 豁免）→ CONFIDENTIAL 限 CN → 管理动作限北京时间 09–18。
        </p>

        {/* 审计 */}
        <section className="abac-panel mt-5">
          <h2>决策审计 · 最近 20 条</h2>
          {auditErr && <p className="text-sm text-rose-300">{auditErr}</p>}
          {audit ? (
            <table className="abac-table">
              <thead>
                <tr>
                  <th>主体</th>
                  <th>资源</th>
                  <th>动作</th>
                  <th>决策</th>
                  <th>推理记录</th>
                </tr>
              </thead>
              <tbody>
                {audit.map((e, i) => (
                  <tr key={i}>
                    <td className="font-mono">{e.user ?? "—"}</td>
                    <td className="font-mono">{e.resource ?? "—"}</td>
                    <td>{e.action ?? "—"}</td>
                    <td>
                      <span
                        className={
                          "inline-flex rounded px-1.5 py-0.5 font-semibold " +
                          (e.decision === "PERMIT"
                            ? "bg-emerald-400/15 text-emerald-300"
                            : "bg-rose-400/15 text-rose-300")
                        }
                      >
                        {e.decision ?? "—"}
                      </span>
                    </td>
                    <td className="text-muted">{e.reason ?? ""}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : (
            <p className="text-sm text-muted">点「刷新审计」加载记录。</p>
          )}
        </section>
      </main>

      <footer>
        账号：admin/admin123 · carol/carol123 · alice/alice123 · bob/bob123　|　服务：examples/abac.lume
      </footer>
    </>
  );
}

const root = document.getElementById("root");
if (root) createRoot(root).render(<App />);
