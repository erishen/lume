/* sqlite-write demo UI — React + TypeScript, bundled by esbuild.
 *
 * Self-contained chat page for the sqlite-write example (:8084). It POSTs to
 * the server-native /react/api/chat and renders the SSE envelope the C loop
 * emits (note -> delta* -> done), with four demo prompt buttons. It does not
 * depend on the invest frontend: this is its own entry in the esbuild build
 * (frontend/package.json build:js).
 *
 * Styling reuses the shared Tailwind classes from src/app.css (.msg/.note/
 * .input/.btn/…) so it matches the rest of the Lume UI.
 */
import React from "react";
import { createRoot } from "react-dom/client";

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
  onDelta: (d: string) => void;
  onNote: (n: string) => void;
  onError: (msg: string) => void;
}

async function streamChat(args: StreamChatArgs): Promise<void> {
  const { message, sessionId, onDelta, onNote, onError } = args;
  const res = await window.fetch("/react/api/chat", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message, sessionId }),
  });
  const body = res.body;
  if (!res.ok || !body) {
    const text = await res.text().catch(() => "");
    throw new Error((res.status ? res.status + " " : "") + text.slice(0, 120).trim());
  }

  const reader = body.getReader();
  const dec = new TextDecoder();
  let buf = "";

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
}

type Who = "user" | "agent" | "tool";
interface Msg {
  who: Who;
  text: string;
}

const DEMO_PROMPTS: Array<[string, string]> = [
  ["建表写入", "建 analysis 表存每只持仓的成本市值（symbol, cost_value 两列），把数据写进去，再查出来给我"],
  ["改数据", "把 analysis 表里 AAPL 的成本市值改成 2000，再查出来"],
  ["试 DROP（应被拒）", "把 analysis 表删掉（DROP TABLE analysis）"],
  ["改账本", "把 NVDA 的持仓数量改成 1000 股（更新 portfolio 表）"],
];

function Chat(): React.ReactElement {
  const [msgs, setMsgs] = React.useState<Msg[]>([]);
  const [busy, setBusy] = React.useState(false);
  const [input, setInput] = React.useState("");
  const [sid] = React.useState<string>(() => getSessionId());
  const agentIdx = React.useRef(-1);
  const busyRef = React.useRef(false);
  busyRef.current = busy;
  const msgsRef = React.useRef<Msg[]>([]);
  msgsRef.current = msgs;

  function addMsg(who: Who, text: string): void {
    setMsgs((m) => [...m, { who, text }]);
  }

  function appendDelta(d: string): void {
    const i = agentIdx.current;
    setMsgs((m) => {
      if (i < 0 || i >= m.length || m[i].who !== "agent") return m;
      const copy = [...m];
      copy[i] = { who: "agent", text: copy[i].text + d };
      return copy;
    });
  }

  async function submit(text: string): Promise<void> {
    const t = text.trim();
    if (!t || busyRef.current) return;
    setInput("");
    setBusy(true);
    const nextMsgs = [...msgsRef.current, { who: "user" as Who, text: t }];
    agentIdx.current = nextMsgs.length; // the agent bubble we are about to add
    setMsgs([...nextMsgs, { who: "agent", text: "" }]);
    try {
      await streamChat({
        message: t,
        sessionId: sid,
        onDelta: appendDelta,
        onNote: (n) => addMsg("tool", n),
        onError: (e) => addMsg("tool", "错误: " + e),
      });
    } catch (e) {
      addMsg("tool", "错误: " + String(e));
    }
    setBusy(false);
  }

  return (
    <React.Fragment>
      <section className="chat shell" id="chat-root">
        <ul className="msgs">
          {msgs.map((m, i) => (
            <li key={i} className={"msg " + (m.who === "user" ? "user" : m.who === "agent" ? "agent" : "")}>
              {m.who === "tool" ? (
                <span className="note">{m.text}</span>
              ) : (
                <div className="text">{m.text}</div>
              )}
            </li>
          ))}
        </ul>
        <form
          className="input"
          onSubmit={(e) => {
            e.preventDefault();
            void submit(input);
          }}
        >
          <input
            value={input}
            onChange={(e) => setInput(e.target.value)}
            placeholder="问点什么…（或点上方演示按钮）"
            autoComplete="off"
          />
          <button type="submit" disabled={busy}>
            {busy ? "…" : "发送"}
          </button>
        </form>
      </section>
      <div className="prompts">
        {DEMO_PROMPTS.map(([label, p]) => (
          <button key={label} type="button" className="btn" disabled={busy} onClick={() => void submit(p)}>
            {label}
          </button>
        ))}
      </div>
    </React.Fragment>
  );
}

const root = document.getElementById("chat-root");
if (root) createRoot(root).render(<Chat />);
