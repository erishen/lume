// www/rea/discovery.js — /discovery 数据的 UI 渲染（外部脚本，满足 CSP script-src 'self'）
(function () {
  "use strict";

  var el = function (id) { return document.getElementById(id); };

  function esc(s) {
    var d = document.createElement("div");
    d.textContent = s == null ? "" : String(s);
    return d.innerHTML;
  }

  // 接入状态徽章
  function renderEndpoints(ep) {
    var grid = el("ep-grid");
    grid.innerHTML = "";
    var items = [
      ["LLM 上游", ep.llm === "configured" ? "已配置" : "未配置",
       ep.llm === "configured" ? "ok" : "na"],
      ["Router 同步", ep.router === "configured" ? "已配置" : "未配置",
       ep.router === "configured" ? "ok" : "na"],
      ["模型", ep.model || "auto", "ok"]
    ];
    items.forEach(function (it) {
      var b = document.createElement("div");
      b.className = "ep-badge";
      b.innerHTML = '<div class="k">' + esc(it[0]) + '</div>' +
                    '<div class="v ' + (it[2] === "ok" ? "ok" : "na") + '">' + esc(it[1]) + "</div>";
      grid.appendChild(b);
    });
  }

  // MCP 服务器表
  function renderMcps(mcps) {
    var box = el("mcp-table");
    el("mcp-count").textContent = mcps.length ? "(" + mcps.length + ")" : "";
    if (!mcps.length) { box.innerHTML = '<div class="empty">暂无 MCP 服务器</div>'; return; }
    var rows = mcps.map(function (m) {
      var src = m.source === "local" ? '<span class="tag local">local</span>' : '<span class="tag gateway">gateway</span>';
      var rea = /(^|[-_])rea([-_]|$)/.test(m.id || "") ? '<span class="tag rea">rea</span>' : "";
      var cmd = (m.command || "") + (m.args ? " " + m.args : "");
      var cmdShow = cmd.length > 90 ? cmd.slice(0, 88) + "…" : cmd;
      return "<tr><td><code>" + esc(m.id || "?") + "</code> " + src + " " + rea + "</td>" +
             "<td>" + esc(m.transport || "stdio") + "</td>" +
             "<td>" + (m.approval ? "需确认" : "自动") + "</td>" +
             "<td><code>" + esc(cmdShow) + "</code></td></tr>";
    }).join("");
    box.innerHTML = "<table><thead><tr><th>ID</th><th>传输</th><th>审批</th><th>启动命令</th></tr></thead>" +
                    "<tbody>" + rows + "</tbody></table>";
  }

  // 技能卡片
  function renderSkills(skills) {
    var grid = el("skill-grid");
    el("skill-count").textContent = skills.length ? "(" + skills.length + ")" : "";
    grid.innerHTML = "";
    if (!skills.length) { grid.innerHTML = '<div class="empty">暂无技能</div>'; return; }
    skills.forEach(function (s) {
      var card = document.createElement("div");
      card.className = "skill-card";
      card.innerHTML = "<h3>" + esc(s.name) + "</h3><p>" + esc(s.desc) + "</p>";
      grid.appendChild(card);
    });
  }

  // 工具列表（rea__* 与其他分组 + 过滤）
  var lastTools = [];
  function renderTools() {
    var q = (el("tool-q").value || "").trim().toLowerCase();
    var rea = [], other = [];
    lastTools.forEach(function (t) {
      if (q && t.toLowerCase().indexOf(q) < 0) return;
      (t.indexOf("rea__") === 0 ? rea : other).push(t);
    });
    var box = el("tool-list");
    el("tool-count").textContent = lastTools.length ? "(" + lastTools.length + ")" : "";
    var html = "";
    if (rea.length) {
      html += '<div class="tool-group"><h3>rea__* 逆向工程（' + rea.length + "）</h3>" +
              '<div class="tool-chips">' + rea.map(function (t) {
                return '<span class="chip rea">' + esc(t) + "</span>";
              }).join("") + "</div></div>";
    }
    if (other.length) {
      html += '<div class="tool-group"><h3>其他（' + other.length + "）</h3>" +
              '<div class="tool-chips">' + other.map(function (t) {
                return '<span class="chip">' + esc(t) + "</span>";
              }).join("") + "</div></div>";
    }
    if (!rea.length && !other.length) html = '<div class="empty">没有匹配的工具</div>';
    box.innerHTML = html;
  }

  function load() {
    fetch("/discovery")
      .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
      .then(function (d) {
        renderEndpoints(d.endpoints || {});
        renderMcps(d.mcps || []);
        renderSkills(d.skills || []);
        lastTools = (d.tools || []).map(function (t) {
          return typeof t === "string" ? t : (t && t.name) || "";
        }).filter(function (n) { return !!n; });
        renderTools();
      })
      .catch(function () {
        var msg = '<div class="empty">加载失败：无法访问 /discovery</div>';
        el("mcp-table").innerHTML = msg;
        el("skill-grid").innerHTML = msg;
        el("tool-list").innerHTML = msg;
      });
  }

  el("btn-refresh").addEventListener("click", load);
  el("tool-q").addEventListener("input", renderTools);
  load();
})();
