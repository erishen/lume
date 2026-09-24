/* Invest 助手 — 周报相关公共类型与小工具(reports / dashboard 共用)。 */
export type Report = {
  name: string;
  model: string;
  date: string;
  size: number;
  mtime: string;
  preview: string;
};

export type ReportBody = { name: string; mtime: string; size: number; body: string };

export async function fetchJson<T>(path: string): Promise<T> {
  const res = await window.fetch(path);
  if (!res.ok) throw new Error(path + " -> HTTP " + res.status);
  return (await res.json()) as T;
}

export function fmtSize(n: number): string {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / 1024 / 1024).toFixed(1) + " MB";
}

export function fmtDate(date: string): string {
  if (!date || date.length !== 8) return date || "—";
  return date.slice(0, 4) + "-" + date.slice(4, 6) + "-" + date.slice(6);
}

export function titleDate(name: string): string {
  const m = /__weekly_review_(\d{8})/.exec(name);
  return m ? fmtDate(m[1]) : name;
}