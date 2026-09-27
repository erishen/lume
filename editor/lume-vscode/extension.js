// lume-vscode 语言功能：定义跳转 + 大纲符号
// 纯静态扫描（无 LSP）：同文件 F12/Ctrl+Click 跳转、跨 import 文件的
// export 跳转（ns.xxx → 被 import 文件里的定义）。扫描逻辑在 lume-symbols.js。
'use strict';

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const sym = require('./lume-symbols');

// 内置函数声明文件（由 interp.c bridge_seed_builtins() 注册表生成）：
// Ctrl+Click 内置函数时跳到这里的签名声明。
const BUILTINS_URI = vscode.Uri.file(path.join(__dirname, 'builtins.lume'));

function documentLines(document) {
  const lines = [];
  for (let i = 0; i < document.lineCount; i++) lines.push(document.lineAt(i).text);
  return lines;
}

// 内置函数查找：扫 builtins.lume（一次读取，静态文件）
let _builtinsCache = null;
function builtinLocation(name) {
  if (_builtinsCache === null) {
    try {
      _builtinsCache = sym.scanDocumentLines(
        fs.readFileSync(BUILTINS_URI.fsPath, 'utf8').split('\n')
      );
    } catch (e) {
      _builtinsCache = []; // 文件缺失则内置跳转不可用
    }
  }
  const s = sym.findInSymbols(_builtinsCache, name, false);
  return s ? new vscode.Location(BUILTINS_URI, new vscode.Position(s.line, s.col)) : null;
}

async function findDefinition(document, position) {
  const lines = documentLines(document);
  const w = sym.wordAtLine(lines[position.line], position.character);
  if (!w) return null;

  if (w.ns) {
    // 跨文件：ns.name → import 映射 → 目标文件里的 export 定义
    const imports = sym.scanImportLines(lines);
    const rel = imports[w.ns];
    if (!rel) return null;
    const targetUri = vscode.Uri.file(path.resolve(path.dirname(document.uri.fsPath), rel));
    let targetDoc;
    try {
      targetDoc = await vscode.workspace.openTextDocument(targetUri);
    } catch (e) {
      return null; // 目标文件不存在/打不开 → 无跳转
    }
    const s = sym.findInSymbols(sym.scanDocumentLines(documentLines(targetDoc)), w.name, true);
    return s ? new vscode.Location(targetUri, new vscode.Position(s.line, s.col)) : null;
  }

  // 同文件用户代码优先（用户可定义同名覆盖内置）
  const s = sym.findInSymbols(sym.scanDocumentLines(lines), w.name, false);
  if (s) return new vscode.Location(document.uri, new vscode.Position(s.line, s.col));

  // 兜底：内置函数 → builtins.lume 声明
  return builtinLocation(w.name);
}

function activate(context) {
  const defProvider = vscode.languages.registerDefinitionProvider('lume', {
    provideDefinition: (document, position) => findDefinition(document, position),
  });

  const symbolProvider = vscode.languages.registerDocumentSymbolProvider('lume', {
    provideDocumentSymbols: (document) => {
      const lines = documentLines(document);
      return sym.scanDocumentLines(lines).map((s) => {
        const isFunc = s.kind === 'func';
        const range = new vscode.Range(s.line, s.col, s.line, s.col + s.name.length);
        return new vscode.DocumentSymbol(
          s.name,
          s.exported ? (isFunc ? 'export func' : 'export let') : (isFunc ? 'func' : 'let'),
          isFunc ? vscode.SymbolKind.Function : vscode.SymbolKind.Variable,
          range,
          range
        );
      });
    },
  });

  context.subscriptions.push(defProvider, symbolProvider);
}

function deactivate() {}

module.exports = { activate, deactivate };
