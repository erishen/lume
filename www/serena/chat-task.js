// www/serena/chat-task.js — Agent Chat 读取 ?task= 参数并自动发送。
// chat bundle（/invest/app.js）是 React 受控输入，URL 无参数支持；
// 这里在 chat 页内用 native setter 触发 onChange，再点 Send 提交。
// 外部脚本文件（CSP script-src 'self' 允许），与 serena-demo 首页联动：
// 首页 prompt-card 点击 → /chat?task=<encodeURIComponent(prompt)>。
(function () {
  "use strict";
  var task = new URLSearchParams(location.search).get("task");
  if (!task) return;

  var tries = 0;
  var timer = setInterval(function () {
    tries++;
    var input = document.getElementById("chat-in");
    var form = document.querySelector("form.input");
    if (!input || !form) {
      if (tries > 40) clearInterval(timer); // ~12s 内 React 未渲染则放弃
      return;
    }
    clearInterval(timer);
    // React 受控 input：用原生 value setter 绕过 React 的 value 快照
    var setter = Object.getOwnPropertyDescriptor(
      window.HTMLInputElement.prototype, "value").set;
    setter.call(input, task);
    input.dispatchEvent(new Event("input", { bubbles: true }));
    // 等 React onChange 批处理完成后再提交（Send 是 type=submit, 点击触发表单 onSubmit）
    setTimeout(function () {
      var send = document.getElementById("send");
      if (send) send.click();
      else form.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }));
    }, 150);
  }, 300);
})();
