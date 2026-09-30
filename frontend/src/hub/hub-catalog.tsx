/* tsm-hub catalog page (www/hub/catalog.html, data-page="hub-catalog").
 * Full registry rendered from the /catalog JSON endpoint: skills with their
 * complete SKILL.md descriptions, tools with OpenAI schema, and the merged
 * MCP list (gateway-synced + local) with source tagging. Home/discovery are
 * hub-home.tsx / hub-discovery.tsx; shared atoms live in hub-shared.tsx.
 *
 * Build:  cd frontend && pnpm run build:hub:catalog:js  -> ../www/hub-catalog.js
 */
import { useEffect } from "react";
import { createRoot } from "react-dom/client";
import { Nav, Loading, Err, Section, useJson, type Catalog } from "./hub-shared";

function HubCatalog() {
  const { data, error } = useJson<Catalog>("/catalog");
  useEffect(() => {
    if (location.hash) {
      const el = document.getElementById(location.hash.slice(1));
      if (el) el.scrollIntoView();
    }
  }, [data]);
  if (error) return <><Nav active="catalog" /><main className="chat-page"><Err msg={error} /></main></>;
  if (!data) return <><Nav active="catalog" /><main className="chat-page"><Loading /></main></>;
  // 同 hub-discovery:mcps() 在无同步数据时返回 null,先归一成空数组。
  const skills = data.skills ?? [];
  const tools = data.tools ?? [];
  const mcps = data.mcps ?? [];
  return (
    <>
      <Nav active="catalog" />
      <main className="chat-page">
        <header className="hero chat-hero">
          <h1>tsm-hub 能力台账</h1>
          <p>
            skills(全量 desc) · tools(desc + schema) · mcps(网关 + 本地合并)。
            原始 JSON 在 <a className="link" href="/catalog">/catalog</a>。
          </p>
        </header>
        <Section id="skills" title="Skills" count={skills.length}>
          <table className="cat-table">
            <thead><tr><th>skill</th><th>description</th><th>path</th></tr></thead>
            <tbody>
              {skills.map((s, i) => (
                <tr key={i}>
                  <td className="mono">{s.name}</td>
                  <td>{s.desc}</td>
                  <td className="cat-path">{s.path}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </Section>
        <Section id="tools" title="Tools" count={tools.length}>
          <table className="cat-table">
            <thead><tr><th>tool</th><th>description</th><th>schema</th></tr></thead>
            <tbody>
              {tools.map((t, i) => (
                <tr key={i}>
                  <td className="mono">{t.name}</td>
                  <td>{t.desc}</td>
                  <td>
                    <details><summary>params</summary>
                      <pre className="cat-schema">{t.schema}</pre>
                    </details>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </Section>
        <Section id="mcps" title="MCP servers" count={mcps.length}>
          <table className="cat-table">
            <thead><tr><th>id</th><th>source</th><th>transport</th><th>command</th></tr></thead>
            <tbody>
              {mcps.map((m, i) => (
                <tr key={i}>
                  <td className="mono">{m.id}</td>
                  <td className="mono">{m.source}</td>
                  <td className="mono">{m.transport ?? ""}</td>
                  <td className="mono cat-path">{m.command ?? ""}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </Section>
      </main>
    </>
  );
}

const host = document.getElementById("hub-root");
if (host) createRoot(host).render(<HubCatalog />);