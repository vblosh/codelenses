import type { OccurrenceDto, DiagnosticItem } from "../types";

export interface RenderLinesOptions {
  highlightedLines: string[];
  startLineNumber?: number; // 1-indexed (default: 1)
  selectedLine?: number | null;
  diagnostics?: DiagnosticItem[];
  occurrences?: OccurrenceDto[];
  onLineClick?: (lineNumber: number) => void;
  onSymbolClick?: (symbolId: number, lineNumber: number) => void;
}

export function createSourceLineElement(
  lineNumber: number,
  contentHtml: string,
  isSelected: boolean,
  lineDiagnostics?: DiagnosticItem[],
  onLineClick?: (lineNumber: number) => void
): HTMLElement {
  const lineDiv = document.createElement("div");
  lineDiv.className = `code-line${isSelected ? " selected" : ""}`;
  lineDiv.dataset.line = String(lineNumber);

  if (lineDiagnostics && lineDiagnostics.length > 0) {
    const hasError = lineDiagnostics.some((d) => d.severity === "error");
    const hasWarning = lineDiagnostics.some((d) => d.severity === "warning");
    const diagClass = hasError ? "has-error" : hasWarning ? "has-warning" : "has-info";
    lineDiv.classList.add(diagClass);

    // Gutter diagnostic indicator
    const marker = document.createElement("span");
    marker.className = `line-diag-marker ${diagClass}`;
    marker.title = lineDiagnostics.map((d) => `[${d.severity.toUpperCase()}] ${d.message}`).join("\n");
    lineDiv.appendChild(marker);
  }

  // Line number span
  const numSpan = document.createElement("span");
  numSpan.className = "line-number";
  numSpan.textContent = String(lineNumber);
  lineDiv.appendChild(numSpan);

  // Line content code element
  const codeElem = document.createElement("code");
  codeElem.className = "line-content";
  // Safe: contentHtml was constructed from escaped source text and verified token spans
  codeElem.innerHTML = contentHtml || "&nbsp;";
  lineDiv.appendChild(codeElem);

  if (onLineClick) {
    lineDiv.addEventListener("click", () => {
      onLineClick(lineNumber);
    });
  }

  return lineDiv;
}

export function renderSourceLines(
  container: HTMLElement,
  options: RenderLinesOptions
): void {
  container.innerHTML = "";
  const fragment = document.createDocumentFragment();
  const startNum = options.startLineNumber ?? 1;

  // Index diagnostics by line number
  const diagsByLine = new Map<number, DiagnosticItem[]>();
  if (options.diagnostics) {
    for (const d of options.diagnostics) {
      const list = diagsByLine.get(d.line) || [];
      list.push(d);
      diagsByLine.set(d.line, list);
    }
  }

  for (let i = 0; i < options.highlightedLines.length; i++) {
    const currentLineNum = startNum + i;
    const isSelected = options.selectedLine === currentLineNum;
    const lineDiags = diagsByLine.get(currentLineNum);

    const lineElem = createSourceLineElement(
      currentLineNum,
      options.highlightedLines[i],
      isSelected,
      lineDiags,
      options.onLineClick
    );

    fragment.appendChild(lineElem);
  }

  container.appendChild(fragment);
}
