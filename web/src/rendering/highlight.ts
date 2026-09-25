import hljs from "highlight.js";
import type { HighlightResponseDto, HighlightToken } from "../types";
import { escapeHtml } from "./ranges";
export { escapeHtml };

// Map normalized language names / extensions to highlight.js language identifiers
const LANGUAGE_MAP: Record<string, string> = {
  c: "c",
  h: "c",
  cpp: "cpp",
  cxx: "cpp",
  cc: "cpp",
  hpp: "cpp",
  hxx: "cpp",
  cs: "csharp",
  csharp: "csharp",
  py: "python",
  python: "python",
  ts: "typescript",
  typescript: "typescript",
  tsx: "typescript",
  js: "javascript",
  javascript: "javascript",
  jsx: "javascript",
  mjs: "javascript",
  cjs: "javascript",
  go: "go",
  java: "java",
  sh: "bash",
  bash: "bash",
  zsh: "bash",
  json: "json",
  xml: "xml",
  html: "xml",
  css: "css",
  md: "markdown",
  markdown: "markdown",
  sql: "sql",
  yaml: "yaml",
  yml: "yaml",
};

export function normalizeLanguage(langOrExt?: string): string | undefined {
  if (!langOrExt) return undefined;
  const clean = langOrExt.toLowerCase().replace(/^\./, "");
  return LANGUAGE_MAP[clean] || (hljs.getLanguage(clean) ? clean : undefined);
}

/**
 * Splits HTML code lines while preserving and balancing span tags across newline boundaries.
 */
export function splitHtmlLines(html: string): string[] {
  const rawLines = html.split(/\r?\n/);
  const result: string[] = [];
  const openTagsStack: string[] = [];

  for (let i = 0; i < rawLines.length; i++) {
    let line = rawLines[i];
    let prefix = "";

    // Reopen tags that remained open from the previous line
    if (openTagsStack.length > 0) {
      prefix = openTagsStack.map((cls) => `<span class="${cls}">`).join("");
    }

    // Inspect open/closing spans in this line
    const spanRegex = /<span class="([^"]+)">|<\/span>/g;
    let match: RegExpExecArray | null;

    // Track state changes in this line
    while ((match = spanRegex.exec(line)) !== null) {
      if (match[0].startsWith("<span")) {
        openTagsStack.push(match[1]);
      } else if (match[0] === "</span>") {
        openTagsStack.pop();
      }
    }

    // Close any tags still open at the end of this line
    let suffix = "";
    if (openTagsStack.length > 0) {
      suffix = "</span>".repeat(openTagsStack.length);
    }

    result.push(prefix + line + suffix);
  }

  return result;
}

export function renderWithHighlightJs(sourceText: string, language?: string): string[] {
  const normalizedLang = normalizeLanguage(language);
  let highlightedHtml: string;

  if (normalizedLang && hljs.getLanguage(normalizedLang)) {
    try {
      highlightedHtml = hljs.highlight(sourceText, {
        language: normalizedLang,
        ignoreIllegals: true,
      }).value;
    } catch {
      highlightedHtml = escapeHtml(sourceText);
    }
  } else {
    try {
      const autoResult = hljs.highlightAuto(sourceText);
      highlightedHtml = autoResult.value || escapeHtml(sourceText);
    } catch {
      highlightedHtml = escapeHtml(sourceText);
    }
  }

  return splitHtmlLines(highlightedHtml);
}

function getTokenClassName(
  token: HighlightToken,
  legend: HighlightResponseDto["legend"]
): string {
  // Support both legend array (tokenTypes) and legend object mapping
  let typeName = "";

  if (typeof token.tokenType === "number" && legend.tokenTypes && legend.tokenTypes[token.tokenType]) {
    typeName = legend.tokenTypes[token.tokenType];
  } else if (typeof token.tokenType === "string") {
    typeName = token.tokenType;
  } else if (token.kind) {
    if (typeof legend[token.kind] === "string") {
      return legend[token.kind];
    }
    typeName = token.kind;
  }

  if (!typeName) {
    typeName = "variable";
  }

  return `tok-${typeName.toLowerCase()}`;
}

export function renderWithTreeSitterTokens(
  lines: string[],
  highlightResponse: HighlightResponseDto,
  startLine: number = 0
): string[] {
  const tokensByLine = new Map<number, HighlightToken[]>();

  for (const token of highlightResponse.tokens) {
    const list = tokensByLine.get(token.line) || [];
    list.push(token);
    tokensByLine.set(token.line, list);
  }

  const result: string[] = [];

  for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
    const lineText = lines[lineIdx];
    const absoluteLine = startLine + lineIdx;
    const lineTokens = tokensByLine.get(absoluteLine);

    if (!lineTokens || lineTokens.length === 0) {
      result.push(escapeHtml(lineText));
      continue;
    }

    // Sort tokens by start column
    lineTokens.sort((a, b) => {
      const startA = a.startColumn !== undefined ? a.startColumn : a.start || 0;
      const startB = b.startColumn !== undefined ? b.startColumn : b.start || 0;
      return startA - startB;
    });

    let lineHtml = "";
    let currentPos = 0;

    for (const token of lineTokens) {
      const tokenStart = token.startColumn !== undefined ? token.startColumn : token.start || 0;
      const tokenLen =
        token.length !== undefined
          ? token.length
          : token.end !== undefined
          ? token.end - tokenStart
          : 0;
      const tokenEnd = Math.min(lineText.length, tokenStart + tokenLen);

      if (tokenStart < currentPos || tokenStart >= lineText.length) {
        // Skip overlapping or out-of-bounds token
        continue;
      }

      // Append unhighlighted text before token
      if (tokenStart > currentPos) {
        lineHtml += escapeHtml(lineText.substring(currentPos, tokenStart));
      }

      // Append token span
      const tokenText = lineText.substring(tokenStart, tokenEnd);
      const className = getTokenClassName(token, highlightResponse.legend);
      lineHtml += `<span class="tok ${className}">${escapeHtml(tokenText)}</span>`;

      currentPos = tokenEnd;
    }

    // Append remaining line text
    if (currentPos < lineText.length) {
      lineHtml += escapeHtml(lineText.substring(currentPos));
    }

    result.push(lineHtml);
  }

  return result;
}

export function highlightSource(
  sourceText: string,
  language?: string,
  backendHighlights?: HighlightResponseDto | null,
  startLine: number = 0
): string[] {
  const lines = sourceText.split(/\r?\n/);

  if (
    backendHighlights &&
    Array.isArray(backendHighlights.tokens) &&
    backendHighlights.tokens.length > 0
  ) {
    try {
      return renderWithTreeSitterTokens(lines, backendHighlights, startLine);
    } catch (err) {
      console.warn("Tree-sitter highlighting failed, falling back to highlight.js:", err);
    }
  }

  return renderWithHighlightJs(sourceText, language);
}
