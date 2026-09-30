/* Lume demo UI — the client side of the Lume SSR shell, in React + TypeScript.
 * The SSR shell (nav, hero, cards, stats) is rendered by Lume; this bundle
 * takes over the interactive widgets by mounting React roots into the SSR
 * containers (#clock, #counter-root, #chat-root) and replacing their filler.
 *
 * Styling is Tailwind: shared semantic classes (.hero/.counter/.msg/…) are
 * defined in src/app.css (components layer) because the Lume SSR markup emits
 * those same names.
 *
 * Build:  cd frontend && pnpm run build      -> ../www/invest/app.js (React
 * 分包进 ../www/chunk-*.js) + ../www/app.css
 * Watch:  cd frontend && npm run dev
 */
import React from "react";
import { createRoot } from "react-dom/client";

declare global {
  interface Window {
    __LUME_UI__?: string;
  }
}

window.__LUME_UI__ = "react-via-esbuild";

function isGateway(): boolean {
  return document.body.dataset.gateway === "";
}

function pad(n: number): string {
  return (n < 10 ? "0" : "") + n;
}

function useNow(stepMs: number): Date {
  const [now, setNow] = React.useState<Date>(() => new Date());
  React.useEffect(() => {
    const id = window.setInterval(() => setNow(new Date()), stepMs);
    return () => window.clearInterval(id);
  }, [stepMs]);
  return now;
}

function fetchJSON<T>(url: string): Promise<T> {
  return window.fetch(url).then((r) => {
    if (!r.ok) throw new Error(url + " -> " + r.status);
    return r.json() as Promise<T>;
  });
}

function Clock(): React.ReactElement {
  const now = useNow(1000);
  return (
    <React.Fragment>
      {pad(now.getHours())}:{pad(now.getMinutes())}:{pad(now.getSeconds())}
    </React.Fragment>
  );
}

type Counters = { count: number; hits: number };

function Counter(): React.ReactElement {
  const [count, setCount] = React.useState(0);
  const [status, setStatus] = React.useState("syncing…");

  React.useEffect(() => {
    fetchJSON<Counters>("/api/count").then(
      (j) => setCount(j.count),
      () => setStatus("cannot reach /api/count")
    );
  }, []);

  function sync(): void {
    setStatus("syncing…");
    fetchJSON<Counters>("/api/inc").then(
      (j) => {
        setCount(j.count);
        const hits = document.getElementById("hits");
        if (hits) hits.textContent = String(j.hits);
        setStatus("synced " + new Date().toLocaleTimeString());
      },
      (e: Error) => setStatus("error: " + e.message)
    );
  }

  return (
    <React.Fragment>
      <big id="count">{count}</big>
      <button id="inc" onClick={sync}>
        +1
      </button>
      <small id="status">{status}</small>
    </React.Fragment>
  );
}

function mount(selector: string, el: React.ReactElement): void {
  const host = document.querySelector(selector);
  if (host) createRoot(host).render(el);
}

/* ---------------------------------------------------------------------- *
 * Agent chat (/chat)                                                     *
 *                                                                        *
 * POSTs to the server-native /react/api/chat and renders the SSE envelope *
 * the C loop emits: note (status/tool lines) -> delta* (streamed tokens)  *
 * -> done. A server-minted-compatible session id (hex, no dots) brings    *
 * multi-turn memory; history is replayed server-side from the transcript.  *
 * ---------------------------------------------------------------------- */

const SID_KEY = "lume.agent.sid";

function makeSessionId(): string {
  const a = new Uint8Array(16);
  window.crypto.getRandomValues(a);
  let h = "sess-";
  for (const b of a) h += b.toString(16).padStart(2, "0");
  return h;
}

function getSessionId(): string {
  const existing = window.localStorage.getItem(SID_KEY);
  if (existing) return existing;
  const next = makeSessionId();
  window.localStorage.setItem(SID_KEY, next);
  return next;
}

type SseEvent =
  | { t: "delta"; d: string }
  | { t: "note"; d: string }
  | { t: "error"; d?: string }
  | { t: "done" };

interface StreamChatArgs {
  message: string;
  sessionId: string;
  signal: AbortSignal;
  onDelta: (d: string) => void;
  onNote: (n: string) => void;
  onError: (msg: string) => void;
  onDone: () => void;
}

async function streamChat(args: StreamChatArgs): Promise<void> {
  const { message, sessionId, signal, onDelta, onNote, onError, onDone } = args;
  const res = await window.fetch("/react/api/chat", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message, sessionId }),
    signal,
  });
  const body = res.body;
  if (!res.ok || !body) {
    const text = await res.text().catch(() => "");
    throw new Error((res.status ? res.status + " " : "") + text.slice(0, 120).trim());
  }

  const reader = body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  let sawDone = false;

  function handleEvent(line: string): void {
    const data = line.startsWith("data: ") ? line.slice(6) : line;
    if (!data.startsWith("{")) return; // ":" heartbeats and blank lines
    let ev: SseEvent;
    try {
      ev = JSON.parse(data) as SseEvent;
    } catch {
      return;
    }
    if (ev.t === "delta" && typeof ev.d === "string") onDelta(ev.d);
    else if (ev.t === "note" && typeof ev.d === "string") onNote(ev.d);
    else if (ev.t === "error") onError(typeof ev.d === "string" ? ev.d : "agent error");
    else if (ev.t === "done") {
      sawDone = true;
      onDone();
    }
  }

  function handleBlock(block: string): void {
    for (const ln of block.split("\n")) {
      if (ln.startsWith("data: ")) handleEvent(ln.slice(6));
      // ":" heartbeats and "event:"/"id:" metadata are ignored by design
    }
  }

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let idx: number;
    while ((idx = buf.indexOf("\n\n")) >= 0) {
      const block = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      handleBlock(block);
    }
  }
  // The final event may arrive without a trailing blank line; flush it before
  // deciding the stream ended, or the last delta/done is silently dropped.
  handleBlock(buf);
  if (!sawDone) throw new Error("stream ended without a done event");
}

type ChatMessage = { role: "user" | "agent"; text: string; notes: string[] };

/* ---------------------------------------------------------------------- *
 * Lightweight markdown for agent deltas (zero deps).                       *
 *                                                                          *
 * Covers the shapes agent/skill output actually uses: ``` fences, #       *
 * headings, -/1. lists, > quotes, and inline **bold**, *italic*, `code`.      *
 * Deltas stream in one token at a time, so this must stay a cheap single   *
 * pass over the text (no nesting / no tree parse). React escapes all text. *
 * ---------------------------------------------------------------------- */

import { renderMarkdown } from "./markdown";

/* Serialize the in-memory transcript as a plain-text log. The session id is
 * included so a copied exchange can be tied back to the conversation it
 * came from (replaying it server-side needs only that id). */
function transcript(messages: ChatMessage[], session: string): string {
  const lines: string[] = [`agent chat · session ${session}`, ""];
  for (const m of messages) {
    lines.push((m.role === "user" ? "you: " : "agent: ") + (m.text || "…"));
    for (const n of m.notes) lines.push("    (" + n + ")");
    lines.push("");
  }
  return lines.join("\n").trimEnd() + "\n";
}

function copyToClipboard(text: string): Promise<void> {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    return navigator.clipboard.writeText(text);
  }
  /* Legacy fallback (non-secure contexts): hidden textarea + execCommand. */
  return new Promise((resolve, reject) => {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    try {
      const ok = document.execCommand("copy");
      document.body.removeChild(ta);
      if (ok) resolve();
      else reject(new Error("copy failed"));
    } catch (err) {
      document.body.removeChild(ta);
      reject(err as Error);
    }
  });
}

/* ---------------------------------------------------------------------- *
 * Discovery status (/chat)                                               *
 *                                                                        *
 * The agent chat page carries a compact readout of what llm-router       *
 * integration found at boot: the tool registry (local builtins + DSL +   *
 * MCP + router proxies), the SKILL.md index, and the MCP catalog the     *
 * sync wrote. It is a details/summary strip that fetches the Lume        *
 * /discovery endpoint once; every section degrades to a muted placeholder*
 * when the server is offline.                                            *
 * ---------------------------------------------------------------------- */

type Discovery = {
  endpoints: { llm?: string | null; router?: string | null; model?: string | null };
  skills: { name: string; desc?: string }[];
  tools: string[];
  mcps: { id?: string; command?: string }[] | null;
};

function DiscoChip({ children }: { children: React.ReactNode }): React.ReactElement {
  return <code className="disco-chip">{children}</code>;
}

function DiscoveryPanel(): React.ReactElement {
  const [d, setD] = React.useState<Discovery | null>(null);
  const [err, setErr] = React.useState("");

  React.useEffect(() => {
    fetchJSON<Discovery>("/discovery").then(setD, (e: Error) => setErr(e.message));
  }, []);

  const toolsCount = d?.tools?.length ?? 0;
  const skillsCount = d?.skills?.length ?? 0;
  const mcpsCount = d?.mcps?.length ?? 0;

  return (
    <details className="disco">
      <summary className="disco-summary">
        {err ? (
          <span>
            discovery: <em className="muted">{err}</em>
          </span>
        ) : !d ? (
          <span>discovery: loading…</span>
        ) : (
          <span>
            discovery · tools <b>{toolsCount}</b> · skills <b>{skillsCount}</b> · mcps{" "}
            <b>{mcpsCount}</b>
            {d.endpoints?.model ? (
              <>
                {" "}
                · model <b>{d.endpoints.model}</b>
              </>
            ) : null}
          </span>
        )}
      </summary>
      {d && (
        <div className="disco-body">
          <div className="disco-row">
            <span className="disco-label">tools</span>
            {d.tools.length === 0 ? (
              <span className="muted">none</span>
            ) : (
              <span className="disco-chips">
                {d.tools.map((t) => (
                  <DiscoChip key={t}>{t}</DiscoChip>
                ))}
              </span>
            )}
          </div>
          <div className="disco-row">
            <span className="disco-label">skills</span>
            {d.skills.length === 0 ? (
              <span className="muted">none</span>
            ) : (
              <span className="disco-chips">
                {d.skills.map((s) => (
                  <DiscoChip key={s.name}>{s.name}</DiscoChip>
                ))}
              </span>
            )}
          </div>
          <div className="disco-row">
            <span className="disco-label">mcps</span>
            {!d.mcps ? (
              <span className="muted">no sync yet</span>
            ) : d.mcps.length === 0 ? (
              <span className="muted">none</span>
            ) : (
              <span className="disco-chips">
                {d.mcps.map((m) => (
                  <DiscoChip key={m.id ?? m.command ?? "?"}>{m.id || m.command || "?"}</DiscoChip>
                ))}
              </span>
            )}
          </div>
          <div className="disco-row">
            <span className="disco-label">endpoints</span>
            <span className="muted">
              model {d.endpoints?.model || "—"} · llm {d.endpoints?.llm || "—"} · router{" "}
              {d.endpoints?.router || "—"}
            </span>
          </div>
        </div>
      )}
    </details>
  );
}

/* Live "agent is thinking…" pill: a ticking elapsed counter + the most recent
 * tool/status note, so multi-minute tool steps (asset-lens, PSE pipeline) read
 * as busy-with-progress instead of a frozen spinner. */
function BusyPill({ startedAt, lastNote }: { startedAt: number; lastNote: string }): React.ReactElement {
  const [now, setNow] = React.useState<number>(() => Date.now());
  React.useEffect(() => {
    const id = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(id);
  }, []);
  const tick = Math.max(0, Math.floor((now - startedAt) / 1000));
  const mm = Math.floor(tick / 60);
  const ss = tick % 60;
  const stamp = mm > 0 ? `${mm}m${String(ss).padStart(2, "0")}s` : `${ss}s`;
  return (
    <li className="note busy">
      <span className="busy-dots" aria-hidden="true">
        <i />
        <i />
        <i />
      </span>
      <span className="busy-elapsed">
        agent is thinking… · {stamp}
      </span>
      {lastNote && <span className="busy-last">⟳ {lastNote}</span>}
    </li>
  );
}

function Chat(): React.ReactElement {
  const [messages, setMessages] = React.useState<ChatMessage[]>([]);
  const [busy, setBusy] = React.useState(false);
  const [input, setInput] = React.useState("");
  const [session, setSession] = React.useState<string>(getSessionId);
  const [copied, setCopied] = React.useState(false);
  const abortRef = React.useRef<AbortController | null>(null);
  const listRef = React.useRef<HTMLUListElement | null>(null);
  const startRef = React.useRef(0);

  async function copyChat(): Promise<void> {
    try {
      await copyToClipboard(transcript(messages, session));
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1500);
    } catch {
      setCopied(false);
    }
  }

  React.useEffect(() => {
    if (listRef.current) listRef.current.scrollTop = listRef.current.scrollHeight;
  }, [messages]);

  function reset(): void {
    abortRef.current = null;
    window.localStorage.removeItem(SID_KEY);
    setSession(makeSessionId());
    setMessages([]);
  }

  async function send(e: React.FormEvent<HTMLFormElement>): Promise<void> {
    e.preventDefault();
    const text = input.trim();
    if (!text || busy) return;
    setInput("");

    const controller = new AbortController();
    abortRef.current = controller;
    startRef.current = Date.now();

    setBusy(true);
    setMessages((m) => [...m, { role: "user", text, notes: [] }, { role: "agent", text: "", notes: [] }]);

    const notes: string[] = [];
    try {
      await streamChat({
        message: text,
        sessionId: session,
        signal: controller.signal,
        onDelta: (d) => {
          setMessages((m) => m.map((x, i) => (i === m.length - 1 ? { ...x, text: x.text + d } : x)));
        },
        onNote: (n) => {
          notes.push(n);
          setMessages((m) => m.map((x, i) => (i === m.length - 1 ? { ...x, notes: [...notes] } : x)));
        },
        onError: (msg) => {
          setMessages((m) =>
            m.map((x, i) =>
              i === m.length - 1 ? { ...x, text: x.text + "\n[agent error] " + msg } : x
            )
          );
        },
        onDone: () => {},
      });
    } catch (err) {
      const e2 = err as Error;
      if (e2.name === "AbortError") {
        setMessages((m) =>
          m.map((x, i) => (i === m.length - 1 ? { ...x, notes: [...notes, "stopped by user"] } : x))
        );
      } else if (/stream ended without a done event/.test(e2.message)) {
        /* The connection dropped mid-run (long tool gaps, upstream hiccup)
         * before the server's done marker. Surface it as a note, not an
         * inline error line, and never leave the input disabled. */
        setMessages((m) =>
          m.map((x, i) =>
            i === m.length - 1
              ? { ...x, notes: [...notes, "⚠ 连接提前结束（未收到完成标记），结果可能不完整——请重试或换个模式。"] } : x
          )
        );
      } else {
        setMessages((m) =>
          m.map((x, i) => (i === m.length - 1 ? { ...x, text: x.text + "\n[error] " + e2.message } : x))
        );
      }
    } finally {
      setBusy(false);
      abortRef.current = null;
    }
  }

  function stop(): void {
    if (abortRef.current) abortRef.current.abort();
  }

  const lastAgent = messages[messages.length - 1];
  const lastNote = lastAgent?.role === "agent" ? lastAgent.notes[lastAgent.notes.length - 1] ?? "" : "";

  return (
    <React.Fragment>
      <div className="chat-fab">
        <span className="fab-brand">Lume</span>
        <span className="fab-session">
          session <code>{session}</code>
        </span>
        <span className="fab-actions">
          <button id="copy" className="ghost" onClick={copyChat} disabled={messages.length === 0}>
            {copied ? "Copied" : "复制对话"}
          </button>
          {busy ? (
            <button id="stop" className="ghost" onClick={stop}>
              停止
            </button>
          ) : (
            <button id="clear" className="ghost" onClick={reset}>
              新会话
            </button>
          )}
        </span>
      </div>
      <DiscoveryPanel />
      <ul className="msgs" ref={listRef} role="log" aria-live="polite">
        {messages.length === 0 && (
          <li className="note">
            {isGateway()
              ? "我是 tsm-hub 网关助手。直接输入指令 — 试试"
              : "我是你的本地投资助手。直接输入指令 — 试试"}{" "}
            <code>weekly-investment</code>（周报）、<code>portfolio</code>（持仓），或 <code>hello</code>。
          </li>
        )}
        {messages.map((m, i) =>
          m.role === "user" ? (
            <li key={i} className="msg user">
              {m.text}
            </li>
          ) : (
            <li key={i} className="msg agent">
              {m.notes.length > 0 && (
                <div className="note-strip">
                  {m.notes.map((n, j) => (
                    <span key={j} className="note">
                      {n}
                    </span>
                  ))}
                </div>
              )}
              <div className="text">{renderMarkdown(m.text)}</div>
            </li>
          )
        )}
        {busy && <BusyPill startedAt={startRef.current} lastNote={lastNote} />}
      </ul>
      <form className="input" onSubmit={send}>
        <input
          id="chat-in"
          type="text"
          placeholder={busy ? "agent 正在处理…" : "输入指令，回车发送…"}
          autoComplete="off"
          value={input}
          onChange={(e: React.ChangeEvent<HTMLInputElement>) => setInput(e.target.value)}
          disabled={busy}
          autoFocus
        />
        <button id="send" type="submit" disabled={busy}>
          Send
        </button>
      </form>
    </React.Fragment>
  );
}

document.addEventListener("DOMContentLoaded", () => {
  const page = document.body.getAttribute("data-page");
  if (page === "home") mount("#clock", <Clock />);
  else if (page === "counter") mount("#counter-root", <Counter />);
  else if (page === "chat") mount("#chat-root", <Chat />);
});
