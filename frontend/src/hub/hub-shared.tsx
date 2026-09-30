/* Shared atoms for the tsm-hub gateway pages (hub-home / hub-catalog /
 * hub-discovery): the JSON shapes, the fetch-on-mount helper, the top nav and
 * the small primitives both tabular pages reuse. Class names are harvested by
 * Tailwind through the @source lines in src/app.css.
 */
import React, { useEffect, useState } from "react";

export interface SkillEntry { name?: string; desc?: string; path?: string }
export interface ToolEntry { name?: string; desc?: string; schema?: string }
export interface McpEntry {
  id?: string; source?: string; command?: string; args?: string;
  transport?: string; approval?: boolean;
}
export interface Catalog {
  skills: SkillEntry[];
  tools: ToolEntry[];
  mcps: McpEntry[];
}
export interface Discovery {
  endpoints?: { llm?: string; router?: string; model?: string };
  skills: SkillEntry[];
  tools: string[];
  mcps: McpEntry[];
}

export function useJson<T>(url: string): { data: T | null; error: string | null } {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let dead = false;
    fetch(url)
      .then((r) => (r.ok ? r.json() : Promise.reject("HTTP " + r.status)))
      .then((d) => !dead && setData(d as T))
      .catch((e) => !dead && setError(String(e)));
    return () => { dead = true; };
  }, [url]);
  return { data, error };
}

export function Nav({ active }: { active: "home" | "chat" | "discovery" | "catalog" }) {
  return (
    <nav>
      <a href="/" className="brand">Hub</a>
      <a href="/" className={active === "home" ? "active" : ""}>Home</a>
      <a href="/chat" className={active === "chat" ? "active" : ""}>Agent</a>
      <a href="/discovery.html" className={active === "discovery" ? "active" : ""}>Discovery</a>
      <a href="/catalog.html" className={active === "catalog" ? "active" : ""}>Catalog</a>
    </nav>
  );
}

export function Loading() { return <p className="muted">loading…</p>; }

export function Err({ msg }: { msg: string }) {
  return <p className="muted">(load failed: {msg})</p>;
}

export function Section({ id, title, count, children }:
  { id?: string; title: string; count: number; children: React.ReactNode }) {
  return (
    <section className="cat-section" id={id}>
      <h2>{title} ({count})</h2>
      {children}
    </section>
  );
}

export function Chips({ items }: { items: string[] }) {
  return (
    <p className="chips">
      {items.map((n, i) => <span className="cat-chip" key={i}>{n}</span>)}
    </p>
  );
}