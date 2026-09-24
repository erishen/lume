/* tsm-hub landing page (www/hub/index.html, data-page="hub-home").
 * Renders from /catalog: capability counts, entry points, pointers into the
 * catalog/discovery pages. Sibling pages: hub-catalog.tsx / hub-discovery.tsx;
 * shared atoms live in hub-shared.tsx. The invest home (dashboard.tsx) is a
 * separate bundle on the same docroot.
 *
 * Build:  cd frontend && pnpm run build:hub:home:js  -> ../www/hub-home.js
 */
import { createRoot } from "react-dom/client";
import { Nav, useJson, type Catalog } from "./hub-shared";

function HubHome() {
  const { data, error } = useJson<Catalog>("/catalog");
  return (
    <>
      <Nav active="home" />
      <main className="chat-page">
        <header className="hero chat-hero">
          <h1>tsm-hub 网关能力演示</h1>
          <p>把 llm-router(:19070)的全量目录铺平成一组页面,直接对话使唤。</p>
        </header>
        <section className="hub-stats">
          <div className="hub-stat"><b>{(data?.skills ?? []).length}</b><span>Skills · skill-run 加载执行</span></div>
          <div className="hub-stat"><b>{(data?.tools ?? []).length}</b><span>Tools · router 内置 + MCP</span></div>
          <div className="hub-stat"><b>{(data?.mcps ?? []).length}</b><span>MCP · 网关 + 本地合并</span></div>
          <div className="hub-stat"><b>2</b><span>投资 MCP · portfolio/pse</span></div>
        </section>
        <div className="hub-cta">
          <a className="btn" href="/chat">去 Agent 对话</a>
          <a className="btn btn-ghost" href="/catalog.html">看完整台账</a>
        </div>
        <section className="grid">
          <div className="card">
            <h3>技能</h3>
            <p>
              code-review / rust-review / hot-news-post / post-comment /
              weekly-investment,通过 skill-run 按 SKILL.md 执行业务流程。
            </p>
            <p><a className="link" href="/catalog.html#skills">查看完整描述 →</a></p>
          </div>
          <div className="card">
            <h3>工具</h3>
            <p>
              calc / query_exchange_rate / fetch_url / system_info / recall /
              remember 等 router 内置,以及各 MCP 暴露的 __ 前缀工具。
            </p>
            <p><a className="link" href="/catalog.html#tools">看 schema →</a></p>
          </div>
          <div className="card">
            <h3>MCP 服务器</h3>
            <p>
              网关同步 fs / memory / think;本地投资档 portfolio-check 体检与
              pse-review 三表审阅,标 source 区分。
            </p>
            <p><a className="link" href="/catalog.html#mcps">看合并列表 →</a></p>
          </div>
        </section>
        <p className="foot">
          profile 由 make hub 注入白名单(HUB_SKILLS / HUB_TOOLS / HUB_MCPS);
          聊天框上方的发现条与 <a href="/discovery.html" className="link">Discovery</a>{" "}
          反映当前实际加载,<a href="/catalog.html" className="link">Catalog</a> 是完整台账
          (原文简介 + schema + 合并 MCP)。原始 JSON:/discovery、/catalog。
          {error ? ` (${error})` : ""}
        </p>
      </main>
    </>
  );
}

const host = document.getElementById("hub-root");
if (host) createRoot(host).render(<HubHome />);