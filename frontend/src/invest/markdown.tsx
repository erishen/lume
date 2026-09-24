/* Shared client pieces: the zero-dependency markdown renderer (agent deltas
 * and weekly reports), and small UI atoms used across the invest pages.
 *
 * The markdown shapes agent/skill output actually uses: ``` fences, #...#
 * headings, -/1. lists, > quotes, |...| tables, and inline **bold**, *italic*,
 * `code`. It must stay a cheap single pass over the text (no nesting / no
 * tree parse). React escapes all text.
 */
import React from "react";

const MD_INLINE = /(\*\*[^*\n]+\*\*|\*[^*\n]+\*|`[^`\n]+`)/g;
const MD_HEADING = /^(#{1,6})\s+(.*)$/;
const MD_BULLET = /^\s*[-*+]\s+(.*)$/;
const MD_ORDERED = /^\s*(\d+)[.)]\s+(.*)$/;

function renderInline(s: string): React.ReactNode[] {
  return s.split(MD_INLINE).map((p, i) => {
    if (p.startsWith("**") && p.endsWith("**") && p.length > 4)
      return <strong key={i}>{p.slice(2, -2)}</strong>;
    if (p.startsWith("`") && p.endsWith("`") && p.length > 2)
      return <code key={i}>{p.slice(1, -1)}</code>;
    if (p.startsWith("*") && p.endsWith("*") && p.length > 2)
      return <em key={i}>{p.slice(1, -1)}</em>;
    return <React.Fragment key={i}>{p}</React.Fragment>;
  });
}

function cellsOf(row: string): string[] {
  const t = row.trim();
  const inner = t.startsWith("|") ? t.slice(1) : t;
  const trimmed = inner.endsWith("|") ? inner.slice(0, -1) : inner;
  return trimmed.split("|").map((c) => c.trim());
}

export function renderMarkdown(text: string): React.ReactNode[] {
  const lines = text.split("\n");
  const out: React.ReactNode[] = [];
  let fence: string[] | null = null;
  let ul: string[] = [];
  let ol: string[] = [];
  let para: string[] = [];
  let k = 0;

  const flushUl = (): void => {
    if (!ul.length) return;
    out.push(
      <ul key={"k" + k++}>
        {ul.map((t, i) => (
          <li key={i}>{renderInline(t)}</li>
        ))}
      </ul>,
    );
    ul = [];
  };
  const flushOl = (): void => {
    if (!ol.length) return;
    out.push(
      <ol key={"k" + k++}>
        {ol.map((t, i) => (
          <li key={i}>{renderInline(t)}</li>
        ))}
      </ol>,
    );
    ol = [];
  };
  const flushPara = (): void => {
    if (!para.length) return;
    const kids: React.ReactNode[] = [];
    para.forEach((t, i) => {
      if (i > 0) kids.push(<br key={"br" + i} />);
      kids.push(...renderInline(t));
    });
    out.push(<p key={"k" + k++}>{kids}</p>);
    para = [];
  };

  const tableRow = (cells: string[], head: boolean, rowKey: string): React.ReactElement => {
    const tag = head ? "th" : "td";
    return (
      <tr key={rowKey}>
        {cells.map((c, i) =>
          React.createElement(
            tag,
            { key: "c" + i, className: "md-tcell" },
            renderInline(c),
          ),
        )}
      </tr>
    );
  };

  const isSep = (row: string): boolean => {
    if (cellsOf(row).length === 0) return false;
    return cellsOf(row).every((c) => /^:?-{2,}:?$/.test(c));
  };

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (/^```/.test(line)) {
      flushUl();
      flushOl();
      flushPara();
      if (fence === null) fence = [];
      else {
        out.push(
          <pre key={"k" + k++} className="md-pre">
            <code>{fence.join("\n")}</code>
          </pre>,
        );
        fence = null;
      }
      continue;
    }
    if (fence !== null) {
      fence.push(line);
      continue;
    }

    if (line.trimStart().startsWith("|")) {
      const rows: string[] = [line];
      while (i + 1 < lines.length && lines[i + 1].trimStart().startsWith("|")) {
        i++;
        rows.push(lines[i]);
      }
      flushUl();
      flushOl();
      flushPara();
      if (rows.length >= 2 && isSep(rows[1])) {
        out.push(
          <table key={"k" + k++} className="md-table">
            <thead>{tableRow(cellsOf(rows[0]), true, "th")}</thead>
            <tbody>
              {rows.slice(2).map((r, j) => tableRow(cellsOf(r), false, "tb-" + j))}
            </tbody>
          </table>,
        );
      } else {
        out.push(
          <table key={"k" + k++} className="md-table">
            <tbody>{rows.map((r, j) => tableRow(cellsOf(r), false, "tb-" + j))}</tbody>
          </table>,
        );
      }
      continue;
    }

    const h = MD_HEADING.exec(line);
    if (h) {
      flushUl();
      flushOl();
      flushPara();
      const level = h[1].length;
      const cls = "md-h" + (level > 3 ? 3 : level);
      const Tag = ("h" + Math.min(level, 3)) as "h1" | "h2" | "h3";
      out.push(
        <Tag key={"k" + k++} className={cls}>
          {renderInline(h[2])}
        </Tag>,
      );
      continue;
    }
    const b = MD_BULLET.exec(line);
    if (b) {
      flushOl();
      flushPara();
      ul.push(b[1]);
      continue;
    }
    const o = MD_ORDERED.exec(line);
    if (o) {
      flushUl();
      flushPara();
      ol.push(o[2]);
      continue;
    }
    if (/^\s*>\s?/.test(line)) {
      flushUl();
      flushOl();
      flushPara();
      out.push(
        <blockquote key={"k" + k++} className="md-quote">
          {renderInline(line.replace(/^\s*>\s?/, ""))}
        </blockquote>,
      );
      continue;
    }
    if (line.trim() === "") {
      flushUl();
      flushOl();
      flushPara();
      continue;
    }
    flushUl();
    flushOl();
    para.push(line);
  }
  flushUl();
  flushOl();
  flushPara();
  if (fence !== null) {
    out.push(
      <pre key="k-open" className="md-pre">
        <code>{fence.join("\n")}</code>
      </pre>,
    );
  }
  return out;
}