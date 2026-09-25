/* sqlite-write demo UI — React + TypeScript, bundled by esbuild.
 *
 * Self-contained chat page for the sqlite-write example (:8084). It POSTs to
 * the server-native /react/api/chat and renders the SSE envelope the C loop
 * emits (note -> delta* -> done), with four demo prompt buttons. It reuses
 * the shared chat chrome from the invest frontend (.msg.user/.msg.agent/
 * .note-strip/.busy-dots/…) so the visual language matches the rest of Lume.
 *
 * Styling: shared app.css classes + Tailwind utilities (scanned via @source
 * in src/app.css). No dependence on the invest bundle.
 */
import React from "react";
import { createRoot } from "react-dom/client";
import { renderMarkdown } from "../invest/markdown";

const SID_KEY = "lume.sqlite.sid";

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
}

async function streamChat(args: StreamChatArgs): Promise<void> {
  const { message, sessionId, signal, onDelta, onNote, onError } = args;
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

  function handleBlock(block: string): void {
    for (const ln of block.split("\n")) {
      if (!ln.startsWith("data: ")) continue; // ":" heartbeats / event: / id:
      let ev: SseEvent;
      try {
        ev = JSON.parse(ln.slice(6)) as SseEvent;
      } catch {
        continue;
      }
      if (ev.t === "delta" && typeof ev.d === "string") onDelta(ev.d);
      else if (ev.t === "note" && typeof ev.d === "string") onNote(ev.d);
      else if (ev.t === "error") onError(typeof ev.d === "string" ? ev.d : "agent error");
      else if (ev.t === "done") sawDone = true;
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
  handleBlock(buf); // final event may arrive without trailing blank line
  if (!sawDone) throw new Error("stream ended without a done event");
}

type ChatMessage = { role: "user" | "agent"; text: string; notes: string[] };

const DEMO_PROMPTS: Array<[string, string]> = [
  ["建表写入", "建 analysis 表存每只持仓的成本市值（symbol, cost_value 两列），把数据写进去，再查出来给我"],
  ["改数据", "把 analysis 表里 AAPL 的成本市值改成 2000，再查出来"],
  ["试 DROP（应被拒）", "把 analysis 表删掉（DROP TABLE analysis）"],
  ["改账本", "把 NVDA 的持仓数量改成 1000 股（更新 portfolio 表）"],
];

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
      <span className="busy-elapsed">agent is thinking… · {stamp}</span>
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
  const busyRef = React.useRef(false);
  busyRef.current = busy;
  const messagesRef = React.useRef<ChatMessage[]>([]);
  messagesRef.current = messages;

  React.useEffect(() => {
    if (listRef.current) listRef.current.scrollTop = listRef.current.scrollHeight;
  }, [messages, busy]);

  async function copyChat(): Promise<void> {
    try {
      await copyToClipboard(transcript(messages, session));
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1500);
    } catch {
      setCopied(false);
    }
  }

  function reset(): void {
    abortRef.current = null;
    window.localStorage.removeItem(SID_KEY);
    setSession(makeSessionId());
    setMessages([]);
  }

  function stop(): void {
    if (abortRef.current) abortRef.current.abort();
  }

  async function send(text: string): Promise<void> {
    const t = text.trim();
    if (!t || busyRef.current) return;
    setInput("");

    const controller = new AbortController();
    abortRef.current = controller;
    startRef.current = Date.now();

    setBusy(true);
    setMessages((m) => [...m, { role: "user", text: t, notes: [] }, { role: "agent", text: "", notes: [] }]);

    const notes: string[] = [];
    try {
      await streamChat({
        message: t,
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
      });
    } catch (err) {
      const e2 = err as Error;
      if (e2.name === "AbortError") {
        setMessages((m) =>
          m.map((x, i) => (i === m.length - 1 ? { ...x, notes: [...notes, "stopped by user"] } : x))
        );
      } else if (/stream ended without a done event/.test(e2.message)) {
        setMessages((m) =>
          m.map((x, i) =>
            i === m.length - 1
              ? { ...x, notes: [...notes, "⚠ 连接提前结束（未收到完成标记），结果可能不完整——请重试。"] }
              : x
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

  const lastAgent = messages[messages.length - 1];
  const lastNote = lastAgent?.role === "agent" ? lastAgent.notes[lastAgent.notes.length - 1] ?? "" : "";

  return (
    <React.Fragment>
      <div className="chat-fab">
        <span className="fab-brand">Lume · sqlite</span>
        <span className="fab-session">
          session <code>{session}</code>
        </span>
        <span className="fab-actions">
          <button className="ghost" onClick={() => void copyChat()} disabled={messages.length === 0}>
            {copied ? "Copied" : "复制对话"}
          </button>
          {busy ? (
            <button className="ghost" onClick={stop}>
              停止
            </button>
          ) : (
            <button className="ghost" onClick={reset}>
              新会话
            </button>
          )}
        </span>
      </div>
      <div className="prompt-bar">
        {DEMO_PROMPTS.map(([label, p]) => (
          <button key={label} className="prompt-btn" disabled={busy} onClick={() => void send(p)}>
            {label}
          </button>
        ))}
        <a key="dsl" className="prompt-btn" href="/dsl" target="_blank" rel="noreferrer">
          DSL 直连演示 ↗
        </a>
      </div>
      <ul className="msgs" ref={listRef} role="log" aria-live="polite">
        {messages.length === 0 && (
          <li className="note">
            这是 SQLite 写能力演示：本 profile 放行了 <code>sql_write</code>。点上方演示按钮，
            或直接输入指令——模型可建分析表、写入、查询（portfolio 镜像表只读）。
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
      <form
        className="input"
        onSubmit={(e) => {
          e.preventDefault();
          void send(input);
        }}
      >
        <input
          value={input}
          onChange={(e) => setInput(e.target.value)}
          placeholder="问点什么…（或点上方演示按钮）"
          autoComplete="off"
        />
        <button type="submit" disabled={busy}>
          发送
        </button>
      </form>
    </React.Fragment>
  );
}

const root = document.getElementById("chat-root");
if (root) createRoot(root).render(<Chat />);
