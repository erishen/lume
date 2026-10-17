// www/serena/app.js — serena-demo 首页交互（外部脚本，满足 CSP script-src 'self'）
(function () {
  "use strict";

  // 示例 prompt 点击：带任务跳转到 Agent Chat（?task= 由 /serena/chat-task.js 读取并自动发送）
  document.querySelectorAll(".prompt-card").forEach(function (el) {
    el.addEventListener("click", function () {
      var text = el.textContent.trim();
      location.href = "/chat?task=" + encodeURIComponent(text);
    });
  });

  // 实时接入状态：/discovery
  fetch("/discovery")
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      var names = (d.tools || []).map(function (t) {
        return typeof t === "string" ? t : (t && t.name) || "";
      });
      var el = function (id) { return document.getElementById(id); };
      el("s-tools").textContent = names.length;
      el("s-rea").textContent = names.filter(function (n) {
        return n.indexOf("serena__") === 0;
      }).length;
      el("s-mcps").textContent = (d.mcps || []).map(function (m) {
        return m.id || m;
      }).join(", ") || "0";
    })
    .catch(function (err) {
      var el = document.getElementById("s-tools");
      if (el) el.textContent = "—";
    });
})();
