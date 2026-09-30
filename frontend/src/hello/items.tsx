/* Lume hello example — the /items page in React + TypeScript + Tailwind.
 *
 * The list lives in component state: write requests run in forked workers
 * (fresh VM copy each time), so the server keeps no state. Every button still
 * calls the real endpoint and we show the server's reply.
 *
 * Build:  cd frontend && npm run build:items   -> ../www/items.js + items.css
 * Watch:  cd frontend && npm run dev:items
 */
import React from "react";
import { createRoot } from "react-dom/client";

type Item = { id: number; name: string };
type ApiCall = { status: number; text: string };

async function call(method: string, body?: unknown): Promise<ApiCall> {
  const res = await window.fetch("/items", {
    method,
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  return { status: res.status, text };
}

const ACTION =
  "rounded-full border border-line px-3 py-1 text-xs font-semibold text-muted transition-colors hover:text-ink";
const ACTION_DANGER = ACTION + " hover:border-red-400 hover:text-red-300";

function Items(): React.ReactElement {
  const [items, setItems] = React.useState<Item[]>([]);
  const [name, setName] = React.useState("");
  const [log, setLog] = React.useState("-");
  const seq = React.useRef(0);

  async function send(method: string, body?: unknown): Promise<void> {
    const r = await call(method, body);
    setLog(method + " /items -> " + r.status + "\n" + r.text);
  }

  async function add(e: React.FormEvent<HTMLFormElement>): Promise<void> {
    e.preventDefault();
    const n = name.trim() || "unnamed";
    setName("");
    await send("POST", { name: n });
    setItems((xs) => [...xs, { id: ++seq.current, name: n }]);
  }

  async function rename(it: Item, method: string, suffix: string): Promise<void> {
    await send(method, { id: it.id, name: it.name });
    setItems((xs) => xs.map((x) => (x.id === it.id ? { ...x, name: x.name + suffix } : x)));
  }

  async function del(it: Item): Promise<void> {
    await send("DELETE", { id: it.id });
    setItems((xs) => xs.filter((x) => x.id !== it.id));
  }

  return (
    <main className="mx-auto max-w-2xl px-6 py-10">
      <h1 className="mb-2 text-3xl font-bold text-accent">Lume items</h1>
      <p className="mb-6 text-sm text-muted">
        List lives in the browser; each button calls the real API on this server and shows its reply.
      </p>
      <form onSubmit={add} className="mb-6 flex gap-2">
        <input
          placeholder="item name"
          value={name}
          onChange={(e: React.ChangeEvent<HTMLInputElement>) => setName(e.target.value)}
          className="flex-1 rounded-full border border-line bg-panel px-4 py-2 text-ink placeholder:text-muted focus:border-accent focus:outline-none"
        />
        <button
          type="submit"
          className="rounded-full bg-accent px-5 py-2 font-bold text-accent-ink transition-[filter] hover:brightness-110"
        >
          Add (POST)
        </button>
      </form>
      <table className="w-full overflow-hidden rounded-lg border border-line text-sm">
        <thead className="bg-panel text-left text-muted">
          <tr>
            <th className="px-3 py-2 font-semibold">id</th>
            <th className="px-3 py-2 font-semibold">name</th>
            <th className="px-3 py-2 font-semibold">actions</th>
          </tr>
        </thead>
        <tbody>
          {items.map((it) => (
            <tr key={it.id} className="border-t border-line">
              <td className="px-3 py-2 tabular-nums text-muted">{it.id}</td>
              <td className="px-3 py-2">{it.name}</td>
              <td className="space-x-2 px-3 py-2">
                <button className={ACTION} onClick={() => rename(it, "PUT", "!")}>
                  PUT
                </button>
                <button className={ACTION} onClick={() => rename(it, "PATCH", "*")}>
                  PATCH
                </button>
                <button className={ACTION_DANGER} onClick={() => del(it)}>
                  DELETE
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <h3 className="mb-2 mt-6 text-sm text-muted">last response</h3>
      <pre className="overflow-x-auto whitespace-pre-wrap rounded-lg border border-line bg-panel p-3 font-mono text-xs text-[#9fe8a2]">
        {log}
      </pre>
    </main>
  );
}

const host = document.getElementById("root");
if (host) createRoot(host).render(<Items />);
