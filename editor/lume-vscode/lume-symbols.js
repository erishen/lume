// lume-symbols.js —— Lume 符号扫描纯逻辑（不依赖 vscode API，可在 node 单测）
// 支持：同文件函数/变量定义跳转、跨 import 文件的 export 定义跳转、大纲符号。
// 定位策略：轻量正则扫描（非完整编译）——足够覆盖 .lume 的实际写法（顶层
// func/let/import），不解析函数体。

'use strict';

const FUNC_RE = /^\s*(export\s+)?func\s+([A-Za-z_][A-Za-z0-9_]*)\b/;
const LET_RE = /^\s*(export\s+)?let\s+([A-Za-z_][A-Za-z0-9_]*)\b/;
const IMPORT_RE = /^\s*import\s+"([^"]+)"\s+as\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$/;

// 去掉行注释（// 之后）。字符串内出现 // 的写法极少，简化处理不影响跳转。
function stripComment(line) {
  const i = line.indexOf('//');
  return i >= 0 ? line.slice(0, i) : line;
}

// 返回符号表：[{ name, line, col, exported, kind }]（kind: 'func' | 'let'）
// 只收顶层声明：跟踪花括号深度（粗粒度行级计数），函数体/tool 体/路由体内
// 的局部 let 不收集。map 字面量与字符串里的花括号多数自行平衡，偶发误计
// 只影响大纲/跳转的启发式判断，不影响使用。
function scanDocumentLines(lines) {
  const syms = [];
  let depth = 0;
  for (let i = 0; i < lines.length; i++) {
    const line = stripComment(lines[i]);
    if (depth === 0) {
      let m;
      if ((m = line.match(FUNC_RE))) {
        const col = line.indexOf(m[2]);
        syms.push({ name: m[2], line: i, col: col >= 0 ? col : 0, exported: !!m[1], kind: 'func' });
      } else if ((m = line.match(LET_RE))) {
        const col = line.indexOf(m[2]);
        syms.push({ name: m[2], line: i, col: col >= 0 ? col : 0, exported: !!m[1], kind: 'let' });
      }
    }
    depth += countChar(line, '{') - countChar(line, '}');
    if (depth < 0) depth = 0;
  }
  return syms;
}

function countChar(s, ch) {
  let n = 0;
  for (let i = 0; i < s.length; i++) if (s[i] === ch) n++;
  return n;
}

// 返回 import 映射：{ ns: 相对路径 }（相对当前文件解析由调用方完成）
function scanImportLines(lines) {
  const imports = {};
  for (let i = 0; i < lines.length; i++) {
    const line = stripComment(lines[i]);
    const m = line.match(IMPORT_RE);
    if (m) imports[m[2]] = m[1];
  }
  return imports;
}

// 光标处标识符：行文本 + 字符偏移 → { ns, name } | null
// ns 非 null 表示是 "ns.name" 成员访问（如 data.load_stage_by_id 的光标落在 name 上）。
// 光标落在标识符中间时，向后吞并标识符字符取完整名字（VS Code 的
// getWordRangeAtPosition 语义，这里用纯文本实现以便 node 单测）。
function wordAtLine(line, character) {
  // 找光标前最近的标识符起点
  const before = line.slice(0, character);
  const m = before.match(/([A-Za-z_][A-Za-z0-9_]*)$/);
  if (!m) return null;
  let name = m[1];
  const nameStart = character - name.length;
  // 光标在标识符中间/结尾：向后吞并后续标识符字符
  let i = character;
  while (i < line.length && /[A-Za-z0-9_]/.test(line[i])) {
    name += line[i];
    i++;
  }
  // 名字前是 '.' → 取命名空间
  if (nameStart > 0 && line[nameStart - 1] === '.') {
    const nm = line.slice(0, nameStart - 1).match(/([A-Za-z_][A-Za-z0-9_]*)$/);
    if (nm) return { ns: nm[1], name };
  }
  return { ns: null, name };
}

function findInSymbols(syms, name, exportedOnly) {
  for (const s of syms) {
    if (s.name === name && (!exportedOnly || s.exported)) return s;
  }
  return null;
}

// ── 内置函数 → C 实现定位（纯逻辑，可 node 单测）──

// BFS 探测：从各 base 出发（深度 ≤ maxDepth，跳过 .*/node_modules/dist），
// 返回所有含 src/builtins.c 的目录（即 lume 仓库根）。调用方决定选哪个。
function findSourceRoots(bases, maxDepth) {
  const fs = require('fs');
  const path = require('path');
  const hits = [];
  const queue = [];
  for (const b of bases) if (b) queue.push({ dir: b, depth: 0 });
  let qi = 0;
  while (qi < queue.length) {
    const { dir, depth } = queue[qi++];
    let ok = false;
    try { ok = fs.existsSync(path.join(dir, 'src', 'builtins.c')); } catch (e) { ok = false; }
    if (ok) hits.push(dir);
    if (depth >= maxDepth) continue;
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { continue; }
    for (const e of entries) {
      if (!e.isDirectory()) continue;
      const n = e.name;
      if (n.startsWith('.') || n === 'node_modules' || n === 'dist') continue;
      queue.push({ dir: path.join(dir, n), depth: depth + 1 });
    }
  }
  return hits;
}

// 在内置名对应的 C 源码里找实现：优先 `native_<name>(` 定义（static 或非
// static，如 builtins_hof.c 的 void native_map），退回 `Value b_<name>(`
// （wrapper 或直接实现，如 b_run）。两遍扫描避免 wrapper 与 native 跨文件
// 先后干扰。返回 { file, line }。
function findNativeLine(lumeRoot, name) {
  const first = scanForNative(lumeRoot, '^(static\\s+)?void native_' + name + '\\(');
  if (first) return first;
  return scanForNative(lumeRoot, '^Value b_' + name + '\\(');
}

function scanForNative(lumeRoot, pattern) {
  const fs = require('fs');
  const path = require('path');
  const dir = path.join(lumeRoot, 'src');
  let files;
  try { files = fs.readdirSync(dir); } catch (e) { return null; }
  const re = new RegExp(pattern);
  for (const f of files) {
    if (!f.endsWith('.c')) continue;
    let lines;
    try { lines = fs.readFileSync(path.join(dir, f), 'utf8').split('\n'); } catch (e) { continue; }
    for (let i = 0; i < lines.length; i++) {
      if (re.test(lines[i].trim())) return { file: f, line: i };
    }
  }
  return null;
}

module.exports = {
  stripComment,
  scanDocumentLines,
  scanImportLines,
  wordAtLine,
  findInSymbols,
  findSourceRoots,
  findNativeLine,
  FUNC_RE,
  LET_RE,
  IMPORT_RE,
};
