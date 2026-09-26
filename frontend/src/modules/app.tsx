/* Modules demo UI — React + TypeScript, bundled by esbuild
 * (frontend/src/modules/app.tsx -> www/modules/app.js). Served by
 * examples/modules-server.lume (:8090).
 *
 * 页面做的事:
 *   1. 税额计算器:输入金额、选区域 -> GET /api/tax -> 展示税率与税额。
 *      后端路由 handler 里 import 了 examples/modules/tax.lume 模块
 *      (tax.apply / tax.rate_for / tax.BASE_RATE)——页面看到的结果就是
 *      多文件 import/export 在真实请求里的样子。
 *   2. 模块导出清单:展示 tax.lume 导出了什么(常量 / 函数 / 类型),以及
 *      与 CLI 版共用同一个库的说明。
 *
 * 视觉:深色 token(bg/panel/ink/muted/accent)与 invest/abac 一致;语义类
 * .modules-* 定义在 src/app.css components 层(Tailwind v4 @source 已含本文件)。
 *
 * Build:  cd frontend && pnpm run build     -> ../www/modules/app.js
 */
import { useEffect, useState } from "react";
import { createRoot } from "react-dom/client";

const ZONES: Array<[string, string, string]> = [
  ["mainland", "中国大陆", "标准税率"],
  ["free-trade", "自贸区", "优惠税率"],
];

const EXPORTS: Array<[string, string, string]> = [
  ["export let", "BASE_RATE", "0.13 — 标准税率"],
  ["export let", "PREFERENTIAL_RATE", "0.06 — 自贸区优惠税率"],
  ["export func", "rate_for(zone)", "按区域返回税率"],
  ["export func", "apply(order)", "按订单(金额+区域)算税额"],
  ["export func", "describe()", "返回税率描述字符串"],
  ["export type", "Order", "{ amount: float, zone: string }"],
];

const PRIVATE_ROWS: Array<[string, string]> = [
  ["internal_note", "未导出的顶层变量,入口读取报 no export 错误"],
  ["hidden in apply", "模块私有常量只被模块内函数引用,入口不可见"],
];

type TaxResult = {
  amount: number;
  zone: string;
  rate: number;
  tax: number;
  base_rate: number;
  preferential_rate: number;
};

function App() {
  const [amount, setAmount] = useState("1000");
  const [zone, setZone] = useState("mainland");
  const [result, setResult] = useState<TaxResult | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  async function calc(nextAmount: string, nextZone: string) {
    setBusy(true);
    setErr(null);
    try {
      const r = await fetch(
        `/api/tax?amount=${encodeURIComponent(nextAmount)}&zone=${encodeURIComponent(nextZone)}`
      );
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      setResult(await r.json());
    } catch (e) {
      setErr(String(e));
      setResult(null);
    } finally {
      setBusy(false);
    }
  }

  useEffect(() => {
    calc(amount, zone);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div className="modules-app min-h-screen bg-[var(--bg)] text-[var(--ink)]">
      <header className="modules-hero">
        <div>
          <p className="modules-eyebrow">Lume · 多文件模块演示</p>
          <h1 className="modules-title">import / export</h1>
          <p className="modules-sub">
            同一个策略库 <code>examples/modules/tax.lume</code>，既被命令行
            (<code>make modules</code>) 也被服务器路由 import —— 库只写一次。
          </p>
        </div>
        <div className="modules-badge">模块顶层只执行一次 · 依赖图缓存</div>
      </header>

      <div className="modules-grid">
        <section className="modules-panel">
          <h2 className="modules-h2">税额计算器</h2>
          <p className="modules-hint">
            金额与区域经 <code>/api/tax</code> 路由进入服务端 —— 路由 handler
            里调用的是 <code>tax.apply(order)</code> /{" "}
            <code>tax.rate_for(zone)</code>。
          </p>

          <div className="modules-form">
            <label className="modules-label">
              订单金额
              <input
                className="modules-input"
                type="number"
                min="0"
                step="any"
                value={amount}
                onChange={(e) => {
                  const v = e.target.value;
                  setAmount(v);
                  calc(v, zone);
                }}
              />
            </label>
            <div className="modules-label">
              区域(zone)
              <div className="modules-zones">
                {ZONES.map(([id, name, hint]) => (
                  <button
                    key={id}
                    className={
                      "modules-zone" + (zone === id ? " modules-zone-active" : "")
                    }
                    onClick={() => {
                      setZone(id);
                      calc(amount, id);
                    }}
                  >
                    <span>{name}</span>
                    <small>{hint}</small>
                  </button>
                ))}
              </div>
            </div>
          </div>

          {busy && <p className="modules-err">计算中…</p>}
          {err && <p className="modules-err">请求失败:{err}</p>}

          {result && (
            <div className="modules-result">
              <div className="modules-result-row">
                <span>订单金额</span>
                <b>{result.amount}</b>
              </div>
              <div className="modules-result-row">
                <span>适用税率</span>
                <b>{result.rate * 100}%</b>
              </div>
              <div className="modules-result-row modules-result-total">
                <span>税额</span>
                <b>{result.tax}</b>
              </div>
              <div className="modules-result-note">
                标准税率 {result.base_rate} · 自贸区优惠 {result.preferential_rate}
              </div>
            </div>
          )}
        </section>

        <section className="modules-panel">
          <h2 className="modules-h2">tax.lume 导出了什么</h2>
          <p className="modules-hint">
            <code>export</code> 前缀显式标记导出;未标记的顶层绑定对入口不可见。
          </p>
          <table className="modules-table">
            <thead>
              <tr>
                <th>导出</th>
                <th>名字</th>
                <th>说明</th>
              </tr>
            </thead>
            <tbody>
              {EXPORTS.map(([kind, name, desc]) => (
                <tr key={name}>
                  <td>
                    <code className="modules-kind">{kind}</code>
                  </td>
                  <td>
                    <code className="modules-name">{name}</code>
                  </td>
                  <td className="modules-desc">{desc}</td>
                </tr>
              ))}
            </tbody>
          </table>

          <h3 className="modules-h3">未导出(入口不可见)</h3>
          <table className="modules-table">
            <tbody>
              {PRIVATE_ROWS.map(([name, desc]) => (
                <tr key={name}>
                  <td>
                    <code className="modules-name modules-name-private">{name}</code>
                  </td>
                  <td className="modules-desc">{desc}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      </div>

      <footer className="modules-foot">
        多文件 import/export · 相对路径解析(realpath 规范化)· 循环 import 检测 ·
        菱形依赖下库顶层只执行一次
      </footer>
    </div>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
