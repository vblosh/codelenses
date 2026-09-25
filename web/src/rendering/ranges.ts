import type { PositionDto, RangeDto } from "../types";

export function escapeHtml(str: string): string {
  return str
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export function formatByteSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export function positionCompare(a: PositionDto, b: PositionDto): number {
  if (a.line !== b.line) return a.line - b.line;
  return a.column - b.column;
}

export function isPositionInRange(pos: PositionDto, range: RangeDto): boolean {
  const afterStart = positionCompare(pos, range.start) >= 0;
  const beforeEnd = positionCompare(pos, range.end) <= 0;
  return afterStart && beforeEnd;
}

export function isLineInRange(line: number, range: RangeDto): boolean {
  return line >= range.start.line && line <= range.end.line;
}
