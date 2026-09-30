/* tsm-hub discovery page (www/hub/discovery.html, data-page="hub-discovery").
 * What the current profile actually loaded, from the /discovery JSON endpoint:
 * the same lightweight shape the chat sidebar feed depends on (tools is a name
 * array). Sibling pages: hub-home.tsx / hub-catalog.tsx; shared atoms live in
 * hub-shared.tsx.
 *
 * Build:  cd frontend && pnpm run build:hub:discovery:js  -> ../www/hub-discovery.js
 */
import { createRoot } from "react-dom/client";
import { Nav, Loading, Err, Section, Chips, useJson, type Discovery } from "./hub-shared";

function HubDiscovery() {
  const { data, error } = useJson<Discovery>("/discovery");
  if (error) return <><Nav active="discovery" /><main className="chat-page"><Err msg={error} /></main></>;
  if (!data) return <><Nav active="discovery" /><main className="chat-page"><Loading /></main></>;
  // /discovery 是 JSON, 但 mcps() 在 router 同步文件缺失/解析失败时返回 null
  // 而不是 [] (见 src/interp.c native_mcps)。网关同步没跑成功时这里就是
  // null, 直接 .length 会炸 —— 统一归一成空数组, 下面就能照常 map。
  const skills = data.skills ?? [];
  const tools = data.tools ?? [];
  const mcps = data.mcps ?? [];
  return (
    <>
      <Nav active="discovery" />
      <main className="chat-page">
        <header className="hero chat-hero">
          <h1>当前 Profile 实际加载</h1>
          <p>
            与聊天框上方发现条同一份数据(轻量摘要;完整台账见{" "}
            <a className="link" href="/catalog.html">Catalog</a>)。原始 JSON 在{" "}
            <a className="link" href="/discovery">/discovery</a>。
          </p>
        </header>
        <Section title="Endpoints" count={0}>
          <table className="cat-table">
            <thead><tr><th>key</th><th>value</th></tr></thead>
            <tbody>
              {(["llm", "router", "model"] as const).map((k) => (
                <tr key={k}><td className="mono">{k}</td><td className="mono">{data.endpoints?.[k] ?? ""}</td></tr>
              ))}
            </tbody>
          </table>
        </Section>
        <Section title="Skills" count={skills.length}>
          <table className="cat-table">
            <thead><tr><th>skill</th><th>description</th></tr></thead>
            <tbody>
              {skills.map((s, i) => (
                <tr key={i}><td className="mono">{s.name}</td><td>{s.desc}</td></tr>
              ))}
            </tbody>
          </table>
        </Section>
        <Section title="Tools" count={tools.length}>
          <Chips items={tools} />
        </Section>
        <Section title="MCP servers" count={mcps.length}>
          <table className="cat-table">
            <thead><tr><th>id</th><th>transport</th><th>command</th><th>approval</th></tr></thead>
            <tbody>
              {mcps.map((m, i) => (
                <tr key={i}>
                  <td className="mono">{m.id}</td>
                  <td className="mono">{m.transport ?? ""}</td>
                  <td className="mono cat-path">{m.command ?? ""}</td>
                  <td>{m.approval === true ? "yes" : m.approval === false ? "no" : ""}</td>
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
if (host) createRoot(host).render(<HubDiscovery />);