/* Invest 助手 — 设置页。
 *
 * 审批开关(PSE_ALLOW_PAID)与模型来源(PSE_REVIEW_PROVIDER)落到
 * IQUEST_ENV_FILE 指向的 .env(dotenvValue 的读取源)。写走 iquest 的
 * POST /api/settings,同源 Origin 由浏览器自动携带;仅回显开关/来源,不碰
 * .env 里的密钥。
 *
 * Build:  cd frontend && pnpm run build:settings   -> ../www/settings.js
 */
import React from "react";
import { createRoot } from "react-dom/client";

type Settings = {
  allow_paid: boolean;
  provider: string;
  env_file: string;
  runtime: { LLM_API_URL: string; LLM_MODEL: string; ROUTER_API_URL: string };
};

const PROVIDERS = [
  { value: "router", label: "router(默认,免费,tsm-hub 网关)" },
  { value: "agnes", label: "agnes(免费,直连 apihub,偶发抽风)" },
  { value: "deepseek", label: "deepseek(付费约 ¥0.1–1/次,需先开审批)" },
] as const;

async function fetchJson<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await window.fetch(path, init);
  const text = await res.text();
  if (!res.ok) throw new Error((text || path) + " (HTTP " + res.status + ")");
  return JSON.parse(text) as T;
}

function SettingsApp(): React.ReactElement {
  const [s, setS] = React.useState<Settings | null>(null);
  const [error, setError] = React.useState("");
  const [saving, setSaving] = React.useState(false);
  const [savedMsg, setSavedMsg] = React.useState("");

  async function load(): Promise<void> {
    setError("");
    try {
      setS(await fetchJson<Settings>("/api/settings"));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  React.useEffect(() => {
    void load();
  }, []);

  async function save(patch: { allow_paid?: boolean; provider?: string }): Promise<void> {
    setSaving(true);
    setSavedMsg("");
    setError("");
    try {
      const next = await fetchJson<{ ok: boolean; allow_paid: boolean; provider: string }>(
        "/api/settings",
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(patch),
        },
      );
      setS((prev) =>
        prev
          ? { ...prev, allow_paid: next.allow_paid, provider: next.provider }
          : prev,
      );
      setSavedMsg("已保存到 " + next.provider + " / 审批 " + (next.allow_paid ? "开" : "关"));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  }

  return (
    <main className="mx-auto max-w-3xl px-6 py-10">
      <h1 className="mb-1 text-3xl font-bold text-accent">设置</h1>
      <p className="mb-6 text-sm text-muted">
        审批开关与模型来源写入 PSE 管线 .env,即时生效(免费 router/agnes 不受影响)。
      </p>

      {error && (
        <div className="mb-4 rounded-lg border border-red-400/40 bg-red-500/10 px-4 py-3 text-sm text-red-300">
          {error}
        </div>
      )}
      {savedMsg && (
        <div className="mb-4 rounded-lg border border-emerald-400/40 bg-emerald-500/10 px-4 py-3 text-sm text-emerald-300">
          {savedMsg}
        </div>
      )}

      {!s && !error && <p className="text-sm text-muted">读取设置中…</p>}

      {s && (
        <>
          <section className="mb-6 rounded-xl border border-line bg-panel p-6">
            <div className="flex items-center justify-between gap-4">
              <div>
                <h2 className="text-base font-semibold text-ink">付费审批开关</h2>
                <p className="mt-0.5 text-[13px] text-muted">
                  PSE_ALLOW_PAID。关闭时 provider=deepseek 会被闸门拦截,零模型调用零费用。
                </p>
              </div>
              <button
                onClick={() => void save({ allow_paid: !s.allow_paid })}
                disabled={saving}
                role="switch"
                aria-checked={s.allow_paid}
                aria-label="切换付费审批"
                className={
                  "relative h-7 w-12 shrink-0 rounded-full transition-colors disabled:opacity-50 " +
                  (s.allow_paid ? "bg-accent" : "bg-line")
                }
              >
                <span
                  className={
                    "absolute top-0.5 h-6 w-6 rounded-full bg-white shadow transition-all " +
                    (s.allow_paid ? "left-[22px]" : "left-0.5")
                  }
                />
              </button>
            </div>
            <p className="mt-3 text-xs text-muted">
              当前状态:{s.allow_paid ? "已批准(dotenvValue 读到 PSE_ALLOW_PAID=1)" : "未批准,deepseek 请求会被拦截"}
            </p>
          </section>

          <section className="mb-6 rounded-xl border border-line bg-panel p-6">
            <h2 className="text-base font-semibold text-ink">模型来源(PSE_REVIEW_PROVIDER)</h2>
            <div className="mt-3 space-y-2">
              {PROVIDERS.map((p) => {
                const active = s.provider === p.value;
                return (
                  <button
                    key={p.value}
                    onClick={() => void save({ provider: p.value })}
                    disabled={saving}
                    aria-pressed={active}
                    className={
                      "block w-full rounded-lg border px-4 py-2.5 text-left text-sm transition-colors disabled:opacity-50 " +
                      (active
                        ? "border-accent bg-accent/10 text-ink"
                        : "border-line text-muted hover:border-accent/40")
                    }
                  >
                    <span className="font-semibold">{p.value}</span>
                    <span className="ml-2 text-xs">{p.label}</span>
                  </button>
                );
              })}
            </div>
          </section>

          <section className="rounded-xl border border-line bg-panel p-6">
            <h2 className="text-base font-semibold text-ink">环境对照</h2>
            <dl className="mt-3 space-y-1.5 text-[13px]">
              <Row k=".env 文件" v={s.env_file ? "已配置" : "未配置"} />
              <Row k="运行时 LLM_API_URL" v={s.runtime.LLM_API_URL === "configured" ? "已配置" : "未配置"} />
              <Row k="运行时 LLM_MODEL" v={s.runtime.LLM_MODEL || "—"} mono />
              <Row k="运行时 ROUTER_API_URL" v={s.runtime.ROUTER_API_URL === "configured" ? "已配置" : "未配置"} />
            </dl>
          </section>

          <section id="privacy" className="mt-6 rounded-xl border border-amber-400/30 bg-amber-500/5 p-6">
            <h2 className="text-base font-semibold text-amber-200">数据出境告知</h2>
            <ul className="mt-3 space-y-2 text-[13px] leading-relaxed text-ink/90">
              <li>
                聊天与周报功能依赖外部 LLM：一次对话/生成会把<b>持仓快照</b>（代码、数量、
                均价、名称）与<b>你的对话文本</b>随请求发送到本页「环境对照」所显示的
                <code className="mx-1 rounded bg-line/60 px-1 py-0.5 font-mono text-xs">LLM_API_URL</code>
                服务商；默认 router 网关的下游模型服务商可能位于<b>中国大陆境外</b>。
              </li>
              <li>
                深度周报管线（pse-review）<b>另外</b>经 autogen-pse/.env 配置的模型链路
                （默认 router 网关，或 deepseek）生成周报——持仓与快照数据同样发送到该
                服务商，由 autogen-pse/.env 的
                <code className="mx-1 rounded bg-line/60 px-1 py-0.5 font-mono text-xs">PSE_ALLOW_PAID</code>
                审批闸门控制是否放行付费模型。
              </li>
              <li>
                发送内容仅用于生成你的回答与周报，不用于其他用途；我们不在服务端留存副本，
                服务商侧的留存以其各自隐私政策为准。
              </li>
              <li>
                不会外发的内容：<code className="mx-1 rounded bg-line/60 px-1 py-0.5 font-mono text-xs">.env</code> 密钥
                （API 密钥等服务端脱敏、页面拿不到）、MCP 沙箱内的文件（除非你在对话中显式
                让工具读取并纳入上下文）。
              </li>
              <li>
                如不同意：不使用聊天/周报即可，仪表盘与归档仍为本地功能；本地账本不会因此
                离开本机。
              </li>
            </ul>
          </section>

          <section className="mt-6 rounded-xl border border-line bg-panel p-6">
            <h2 className="text-base font-semibold text-ink">隐私政策</h2>
            <div className="mt-3 space-y-2 text-[13px] leading-relaxed text-ink/90">
              <p><b>1. 本地存储。</b>投资账本（<code className="rounded bg-line/60 px-1 py-0.5 font-mono text-xs">.data/portfolio.json</code>）、
              周报归档（<code className="rounded bg-line/60 px-1 py-0.5 font-mono text-xs">.data/reports/</code>）、
              会话与 MCP 配置均只保存在本机 <code className="rounded bg-line/60 px-1 py-0.5 font-mono text-xs">.data/</code>
              目录（权限 0700），不提交版本库、不上传任何存储服务。</p>
              <p><b>2. 数据出境。</b>仅当你使用聊天/周报功能时，持仓快照与对话文本会发送到你在本页配置的
              LLM 服务商，可能位于中国大陆境外；详见上方「数据出境告知」。其余功能（仪表盘、归档、设置）不出本机。</p>
              <p><b>3. 无第三方统计。</b>本工具页面为自托管静态资源，不含第三方 SDK、统计脚本或外链资源。</p>
              <p><b>4. 删除。</b>删除 <code className="rounded bg-line/60 px-1 py-0.5 font-mono text-xs">.data/</code>
              目录即删除全部本地数据；已发送给 LLM 服务商的内容需联系对应服务商处理。</p>
              <p><b>5. 变更与联系。</b>本政策随版本更新；问题或建议请提交到 GitHub
              <code className="mx-1 rounded bg-line/60 px-1 py-0.5 font-mono text-xs">erishen/lume</code> issues。
              生效日期：2026-09-24。</p>
            </div>
          </section>
        </>
      )}
    </main>
  );
}

function Row({ k, v, mono }: { k: string; v: string; mono?: boolean }): React.ReactElement {
  return (
    <div className="flex flex-wrap gap-x-3">
      <dt className="w-44 shrink-0 text-muted">{k}</dt>
      <dd className={"min-w-0 flex-1 truncate text-ink " + (mono ? "font-mono text-xs" : "")}>{v}</dd>
    </div>
  );
}

const host = document.getElementById("settings-root");
if (host) createRoot(host).render(<SettingsApp />);