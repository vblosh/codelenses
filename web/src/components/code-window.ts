import type {
  FileMetadataDto,
  FileContentDto,
  HighlightResponseDto,
  DiagnosticItem,
  OccurrenceDto,
  FileCompileCommandDto,
  SymbolOutlineNodeDto,
} from "../types";
import type { StateStore } from "../state";
import { api } from "../api";
import { highlightSource } from "../rendering/highlight";
import { renderSourceLines } from "../rendering/source-lines";
import { formatByteSize } from "../rendering/ranges";

export interface CodeWindowCallbacks {
  onSymbolClick?: (symbolId: number, line: number) => void;
  onCompileClick?: () => void;
}

function escapeCssAttr(value: string): string {
  if (typeof CSS !== "undefined" && typeof CSS.escape === "function") {
    return CSS.escape(value);
  }
  return value.replace(/["\\]/g, "\\$&");
}

export class CodeWindowComponent {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: CodeWindowCallbacks;
  private activeHoveredElement: HTMLElement | null = null;

  private pathElem!: HTMLElement;
  private langBadge!: HTMLElement;
  private compileBadge!: HTMLElement;
  private navControls!: HTMLElement;
  private viewerContainer!: HTMLElement;
  private bannerContainer!: HTMLElement;
  private tabsBar!: HTMLElement;

  private currentFile: FileMetadataDto | null = null;
  private currentContent: FileContentDto | null = null;
  private currentHighlights: HighlightResponseDto | null = null;
  private currentOccurrences: OccurrenceDto[] = [];
  private currentOutline: SymbolOutlineNodeDto[] = [];
  private currentDiagnostics: DiagnosticItem[] = [];
  private currentCompileCommand: FileCompileCommandDto | null = null;

  // In-memory cache of loaded files for quick tab switching
  private fileCache = new Map<number, {
    file: FileMetadataDto;
    content: FileContentDto;
    highlights: HighlightResponseDto | null;
    occurrences: OccurrenceDto[];
    outline: SymbolOutlineNodeDto[];
    compileCommand: FileCompileCommandDto | null;
  }>();

  // Range pagination for large files (> 5MB or > 5000 lines)
  private isRangePaged: boolean = false;
  private currentStartLine: number = 0; // 0-based for backend
  private pageSize: number = 1000;
  private loadRequestId: number = 0;
  private suppressScrollForLine: number | null = null;

  constructor(store: StateStore, callbacks: CodeWindowCallbacks = {}) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "pane code-window";
    this.element.id = "pane-code";
    this.render();
    this.initEvents();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  getCompileCommand(): FileCompileCommandDto | null {
    return this.currentCompileCommand;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="code-tabs-bar" role="tablist" style="display: none;"></div>
      <div class="code-toolbar">
        <div class="code-path">
          <span class="file-path-text">No file open</span>
          <span class="badge lang-badge" style="display: none;"></span>
          <button class="badge compile-badge" style="display: none;" title="Active compile command context (click to inspect)"></button>
        </div>
        <div class="code-nav">
          <span class="file-size-text"></span>
          <span class="line-count-text"></span>
        </div>
      </div>
      <div class="range-banner-area"></div>
      <div class="code-viewer-container">
        <div class="empty-state">
          <div class="empty-state-title">No file selected</div>
          <div class="empty-state-desc">Select a file from the explorer to view its contents.</div>
        </div>
      </div>
    `;

    this.tabsBar = this.element.querySelector(".code-tabs-bar")!;
    this.pathElem = this.element.querySelector(".file-path-text")!;
    this.langBadge = this.element.querySelector(".lang-badge")!;
    this.compileBadge = this.element.querySelector(".compile-badge")!;
    this.navControls = this.element.querySelector(".code-nav")!;
    this.bannerContainer = this.element.querySelector(".range-banner-area")!;
    this.viewerContainer = this.element.querySelector(".code-viewer-container")!;
  }

  private initEvents(): void {
    this.initHoverEvents();

    this.compileBadge.addEventListener("click", () => {
      if (this.callbacks.onCompileClick) {
        this.callbacks.onCompileClick();
      }
    });

    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("openTabs") || changedKeys.includes("selectedFileId")) {
        this.renderTabs();
      }
      if (changedKeys.includes("selectedFileId")) {
        this.loadFile(state.selectedFileId);
      }
      if (changedKeys.includes("selectedLine") && state.selectedLine !== null) {
        this.scrollToLine(state.selectedLine);
      } else if (
        changedKeys.includes("selectedSymbolId") &&
        state.selectedSymbolId !== null &&
        !changedKeys.includes("selectedLine")
      ) {
        const line = this.resolveSelectedOrSymbolLine();
        if (line !== null) {
          this.scrollToLine(line);
        }
      }
      if (changedKeys.includes("selectedSymbolId") || changedKeys.includes("selectedLine")) {
        this.updateSelectedSymbol();
      }
      if (changedKeys.includes("workspaceId")) {
        this.fileCache.clear();
      }
    });
  }

  setDiagnostics(diagnostics: DiagnosticItem[]): void {
    this.currentDiagnostics = diagnostics;
    if (this.currentContent) {
      this.renderCodeLines();
    }
  }

  async loadFile(fileId: number | null, pageStartLine: number = 0): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || !fileId) {
      this.clearFile();
      return;
    }

    const requestId = ++this.loadRequestId;

    // Check in-memory cache for fast tab switching
    const cached = this.fileCache.get(fileId);
    if (
      cached &&
      pageStartLine === 0 &&
      !cached.file.isBinary &&
      cached.content.totalSizeBytes <= 5 * 1024 * 1024
    ) {
      this.currentFile = cached.file;
      this.currentContent = cached.content;
      this.currentHighlights = cached.highlights;
      this.currentOccurrences = cached.occurrences;
      this.currentOutline = cached.outline || [];
      this.currentCompileCommand = cached.compileCommand;
      this.isRangePaged = false;
      this.currentStartLine = 0;

      this.pathElem.textContent = cached.file.relativePath || cached.file.path;
      this.langBadge.textContent = cached.file.language || "text";
      this.langBadge.style.display = "inline-block";

      if (cached.compileCommand?.hasCompileCommand && cached.compileCommand.compileCommand) {
        const std = cached.compileCommand.compileCommand.languageStandard?.toUpperCase() || "BUILD";
        this.compileBadge.textContent = "⚡ " + std;
        this.compileBadge.style.display = "inline-flex";
      } else {
        this.compileBadge.style.display = "none";
      }

      this.store.ensureTabOpen({
        fileId: cached.file.id,
        relativePath: cached.file.relativePath || cached.file.path,
        name: cached.file.name || cached.file.relativePath || `File #${cached.file.id}`,
      });

      this.updateToolbarMeta();
      this.renderRangeBanner();
      this.renderCodeLines();
      this.renderTabs();

      const selLine = this.resolveSelectedOrSymbolLine();
      if (selLine !== null) {
        this.scrollToLine(selLine);
      }
      this.updateSelectedSymbol();
      return;
    }

    try {
      this.viewerContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Loading file...</div>
        </div>
      `;

      // 1. Fetch File Metadata
      const meta = await api.getFileMetadata(wsId, fileId);

      // Discard stale response if selection changed
      if (
        requestId !== this.loadRequestId ||
        (this.store.getState().selectedFileId !== null &&
          this.store.getState().selectedFileId !== fileId) ||
        this.store.getState().workspaceId !== wsId
      ) {
        return;
      }

      this.currentFile = meta;
      this.pathElem.textContent = meta.relativePath || meta.path;
      this.langBadge.textContent = meta.language || "text";
      this.langBadge.style.display = "inline-block";

      // Ensure tab is open with metadata
      this.store.ensureTabOpen({
        fileId: meta.id,
        relativePath: meta.relativePath || meta.path,
        name: meta.name || meta.relativePath || `File #${meta.id}`,
      });
      this.renderTabs();

      // 2. Binary file state check
      if (meta.isBinary) {
        this.currentContent = null;
        this.currentCompileCommand = null;
        this.compileBadge.style.display = "none";
        this.bannerContainer.innerHTML = "";
        this.viewerContainer.innerHTML = `
          <div class="empty-state">
            <svg width="36" height="36" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
              <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path>
              <polyline points="14 2 14 8 20 8"></polyline>
              <circle cx="12" cy="14" r="2"></circle>
            </svg>
            <div class="empty-state-title">Binary file</div>
            <div class="empty-state-desc">
              Binary files cannot be displayed in the source viewer.<br>
              Size: ${formatByteSize(meta.sizeBytes)}
            </div>
          </div>
        `;
        return;
      }

      // 3. Determine if range loading is needed (files > 5MB)
      const isLargeFile = meta.sizeBytes > 5 * 1024 * 1024;
      this.isRangePaged = isLargeFile;
      this.currentStartLine = pageStartLine;

      // 4. Concurrently fetch Content, Tree-sitter Highlights, Occurrences, Outline, and Compile Command
      const contentPromise = isLargeFile
        ? api.getFileContent(
            wsId,
            fileId,
            this.currentStartLine,
            this.currentStartLine + this.pageSize - 1
          )
        : api.getFileContent(wsId, fileId);

      const highlightsPromise = api.getFileHighlights(wsId, fileId).catch(() => null);
      const occurrencesPromise = api.getFileOccurrences(wsId, fileId).catch(() => null);
      const outlinePromise = api.getFileOutline(wsId, fileId).catch(() => null);
      const compileCommandPromise = api.getFileCompileCommand(wsId, fileId).catch(() => null);

      const [contentRes, highlightsRes, occurrencesRes, outlineRes, compileRes] = await Promise.all([
        contentPromise,
        highlightsPromise,
        occurrencesPromise,
        outlinePromise,
        compileCommandPromise,
      ]);

      // Discard stale response if another file was selected while requests were in flight
      if (
        requestId !== this.loadRequestId ||
        (this.store.getState().selectedFileId !== null &&
          this.store.getState().selectedFileId !== fileId) ||
        this.store.getState().workspaceId !== wsId
      ) {
        return;
      }

      this.currentContent = contentRes;
      this.currentHighlights = highlightsRes;
      this.currentOccurrences = occurrencesRes?.occurrences || [];
      this.currentOutline = outlineRes?.outline || [];
      this.currentCompileCommand = compileRes;

      if (compileRes?.hasCompileCommand && compileRes.compileCommand) {
        const std = compileRes.compileCommand.languageStandard?.toUpperCase() || "BUILD";
        this.compileBadge.textContent = "⚡ " + std;
        this.compileBadge.style.display = "inline-flex";
      } else {
        this.compileBadge.style.display = "none";
      }

      // Save to cache for quick tab switching
      if (!isLargeFile && !meta.isBinary) {
        this.fileCache.set(fileId, {
          file: meta,
          content: contentRes,
          highlights: highlightsRes,
          occurrences: occurrencesRes?.occurrences || [],
          outline: outlineRes?.outline || [],
          compileCommand: compileRes,
        });
      }

      // Update Toolbar line/size info
      this.updateToolbarMeta();

      // Render Range Banner if large file
      this.renderRangeBanner();

      // Render Code Lines
      this.renderCodeLines();

      // Scroll to selected line or symbol if set
      const selLine = this.resolveSelectedOrSymbolLine();
      if (selLine !== null) {
        this.scrollToLine(selLine);
      }
      this.updateSelectedSymbol();
    } catch (err: any) {
      if (requestId === this.loadRequestId) {
        this.viewerContainer.innerHTML = `
          <div class="empty-state">
            <div class="empty-state-title" style="color: var(--error);">Error loading file</div>
            <div class="empty-state-desc">${err.message}</div>
          </div>
        `;
      }
    }
  }

  private updateToolbarMeta(): void {
    if (!this.currentFile || !this.currentContent) return;

    const sizeStr = formatByteSize(this.currentFile.sizeBytes);
    const linesStr = `${this.currentContent.totalLines} lines`;
    this.navControls.innerHTML = `
      <span class="file-size-text">${sizeStr}</span>
      <span>•</span>
      <span class="line-count-text">${linesStr}</span>
    `;
  }

  private renderRangeBanner(): void {
    if (!this.isRangePaged || !this.currentContent) {
      this.bannerContainer.innerHTML = "";
      return;
    }

    const start = this.currentStartLine + 1; // 1-based display
    const end = Math.min(
      this.currentContent.totalLines,
      this.currentStartLine + this.pageSize
    );
    const total = this.currentContent.totalLines;

    this.bannerContainer.innerHTML = `
      <div class="range-loading-banner">
        <span>Large file: Showing lines ${start}–${end} of ${total}</span>
        <div style="display: flex; gap: 8px;">
          <button class="btn-icon prev-range-btn" ${this.currentStartLine <= 0 ? "disabled" : ""}>◀ Prev</button>
          <button class="btn-icon next-range-btn" ${end >= total ? "disabled" : ""}>Next ▶</button>
        </div>
      </div>
    `;

    const prevBtn = this.bannerContainer.querySelector(".prev-range-btn") as HTMLButtonElement;
    const nextBtn = this.bannerContainer.querySelector(".next-range-btn") as HTMLButtonElement;

    prevBtn?.addEventListener("click", () => {
      const nextStart = Math.max(0, this.currentStartLine - this.pageSize);
      this.loadFile(this.currentFile!.id, nextStart);
    });

    nextBtn?.addEventListener("click", () => {
      const nextStart = this.currentStartLine + this.pageSize;
      this.loadFile(this.currentFile!.id, nextStart);
    });
  }

  private renderCodeLines(): void {
    if (!this.currentContent) return;

    const viewer = document.createElement("div");
    viewer.className = "code-viewer";

    const highlightedLines = highlightSource(
      this.currentContent.content,
      this.currentFile?.language,
      this.currentHighlights,
      this.currentContent.startLine ?? 0
    );

    renderSourceLines(viewer, {
      highlightedLines,
      startLineNumber: (this.currentContent.startLine ?? 0) + 1,
      selectedLine: this.store.getState().selectedLine,
      diagnostics: this.currentDiagnostics,
      occurrences: this.currentOccurrences,
      onLineClick: (lineNum) => {
        this.suppressScrollForLine = lineNum;
        this.store.selectLine(lineNum);
      },
    });

    this.clearHoverHighlights();
    this.viewerContainer.innerHTML = "";
    this.viewerContainer.appendChild(viewer);

    // Attach click events and tag occurrences inside code lines
    this.attachOccurrenceClicks(viewer);
  }

  private attachOccurrenceClicks(viewer: HTMLElement): void {
    // Build map from line number (1-based) to occurrences on that line
    const occByLine = new Map<number, OccurrenceDto[]>();
    if (this.currentOccurrences) {
      for (const occ of this.currentOccurrences) {
        const line = occ.range.start.line; // 1-based for matching DOM data-line
        const list = occByLine.get(line) || [];
        list.push(occ);
        occByLine.set(line, list);
      }
    }

    const lines = viewer.querySelectorAll<HTMLElement>(".code-line");
    lines.forEach((lineElem) => {
      const lineNum = parseInt(lineElem.dataset.line || "0", 10);
      const occs = occByLine.get(lineNum) || [];

      // Tag all symbol tokens on this line
      const spans = lineElem.querySelectorAll<HTMLElement>(".line-content span");
      spans.forEach((span) => {
        const text = span.textContent?.trim();
        if (!text) return;

        let matchedOcc = occs.find((o) => o.name === text);
        if (!matchedOcc) {
          matchedOcc = occs.find((o) => o.name && (text === o.name || text.includes(o.name)));
        }

        const symbolId = matchedOcc?.symbolId ?? this.findSymbolIdForName(text);

        if (matchedOcc) {
          span.classList.add("symbol-token");
          if (symbolId != null) {
            span.dataset.symbolId = String(symbolId);
          }
          span.dataset.symbolName = matchedOcc.name;
        } else {
          // Check if span is an identifier token from syntax highlighter
          const isNonSymbol =
            span.classList.contains("tok-keyword") ||
            span.classList.contains("tok-operator") ||
            span.classList.contains("tok-string") ||
            span.classList.contains("tok-number") ||
            span.classList.contains("tok-comment") ||
            span.classList.contains("hljs-keyword") ||
            span.classList.contains("hljs-operator") ||
            span.classList.contains("hljs-string") ||
            span.classList.contains("hljs-number") ||
            span.classList.contains("hljs-comment");

          if (!isNonSymbol && /^[$_a-zA-Z\xA0-\uFFFF][$_a-zA-Z0-9\xA0-\uFFFF]*$/.test(text)) {
            span.classList.add("symbol-token");
            span.dataset.symbolName = text;
            if (symbolId != null) {
              span.dataset.symbolId = String(symbolId);
            }
          }
        }
      });

      // When user clicks anywhere inside the code line
      lineElem.addEventListener("click", (e) => {
        const target = (e.target as HTMLElement).closest<HTMLElement>(
          ".symbol-token, [data-symbol-id], [data-symbol-name], .tok, span"
        );
        if (target && lineElem.contains(target) && target.closest(".line-content")) {
          const clickedText = target.dataset.symbolName || target.textContent?.trim();
          if (clickedText) {
            const isNonSymbol =
              target.classList.contains("tok-keyword") ||
              target.classList.contains("tok-operator") ||
              target.classList.contains("tok-string") ||
              target.classList.contains("tok-number") ||
              target.classList.contains("tok-comment") ||
              target.classList.contains("hljs-keyword") ||
              target.classList.contains("hljs-operator") ||
              target.classList.contains("hljs-string") ||
              target.classList.contains("hljs-number") ||
              target.classList.contains("hljs-comment");

            if (!isNonSymbol) {
              let symId: number | null = target.dataset.symbolId
                ? parseInt(target.dataset.symbolId, 10)
                : null;
              if (symId === null || isNaN(symId)) {
                const matchedOcc = occs.find((o) => o.name === clickedText);
                symId = matchedOcc?.symbolId ?? this.findSymbolIdForName(clickedText);
              }

              if (symId != null && !isNaN(symId)) {
                e.stopPropagation();
                this.suppressScrollForLine = lineNum;
                this.store.selectSymbol(symId, lineNum);
                this.updateSelectedSymbol();
                if (this.callbacks.onSymbolClick) {
                  this.callbacks.onSymbolClick(symId, lineNum);
                }
                return;
              } else if (
                target.classList.contains("symbol-token") ||
                /^[$_a-zA-Z\xA0-\uFFFF][$_a-zA-Z0-9\xA0-\uFFFF]*$/.test(clickedText)
              ) {
                // Known symbol or identifier token without backend ID
                e.stopPropagation();
                this.suppressScrollForLine = lineNum;
                this.store.selectLine(lineNum);
                this.selectSymbolByName(clickedText, target);
                return;
              }
            }
          }
        }
      });
    });
  }

  scrollToLine(lineNumber: number): void {
    const zeroBasedLine = lineNumber - 1;
    // If line is outside current page in large file, reload page
    if (
      this.isRangePaged &&
      this.currentContent &&
      (zeroBasedLine < this.currentStartLine ||
        zeroBasedLine >= this.currentStartLine + this.pageSize)
    ) {
      const newStart = Math.max(0, Math.floor(zeroBasedLine / this.pageSize) * this.pageSize);
      this.loadFile(this.currentFile!.id, newStart).then(() => {
        this.scrollToLine(lineNumber);
      });
      return;
    }

    // Find line element
    const lineElem = this.viewerContainer.querySelector<HTMLElement>(
      `.code-line[data-line="${lineNumber}"]`
    );

    if (lineElem) {
      // Remove selected class from previously selected lines
      this.viewerContainer.querySelectorAll(".code-line.selected").forEach((el) => {
        el.classList.remove("selected");
      });
      lineElem.classList.add("selected");

      // If user selected a visible line/symbol directly in the code window, or if the line is already visible, do not scroll
      const isSuppressed = this.suppressScrollForLine === lineNumber;
      this.suppressScrollForLine = null;
      if (isSuppressed || this.isLineVisible(lineElem)) {
        return;
      }

      // Instantly locate code in window a couple of lines above the selected symbol
      const contextLines = 2;
      const targetLineNumber = Math.max(1, lineNumber - contextLines);
      const targetElem =
        this.viewerContainer.querySelector<HTMLElement>(
          `.code-line[data-line="${targetLineNumber}"]`
        ) || lineElem;

      if (targetLineNumber <= 1) {
        this.viewerContainer.scrollTop = 0;
      } else if (targetElem) {
        let offsetTop = 0;
        let el: HTMLElement | null = targetElem;
        while (el && el !== this.viewerContainer) {
          offsetTop += el.offsetTop;
          el = el.offsetParent as HTMLElement | null;
        }
        this.viewerContainer.scrollTop = offsetTop;
      }

      targetElem.scrollIntoView?.({ behavior: "auto", block: "start", inline: "nearest" });
    }
  }

  isLineVisible(lineElem: HTMLElement): boolean {
    const containerRect = this.viewerContainer.getBoundingClientRect();
    const lineRect = lineElem.getBoundingClientRect();
    if (containerRect.height > 0) {
      return (
        lineRect.top >= containerRect.top - 2 &&
        lineRect.bottom <= containerRect.bottom + 2
      );
    }

    if (this.viewerContainer.clientHeight > 0) {
      let offsetTop = 0;
      let el: HTMLElement | null = lineElem;
      while (el && el !== this.viewerContainer) {
        offsetTop += el.offsetTop;
        el = el.offsetParent as HTMLElement | null;
      }
      const lineH = lineElem.offsetHeight || 20;
      return (
        offsetTop >= this.viewerContainer.scrollTop &&
        offsetTop + lineH <=
          this.viewerContainer.scrollTop + this.viewerContainer.clientHeight
      );
    }

    return false;
  }

  private findOutlineNodeById(
    nodes: SymbolOutlineNodeDto[],
    id: number
  ): SymbolOutlineNodeDto | null {
    for (const node of nodes) {
      if (node.id === id) return node;
      if (node.children && node.children.length > 0) {
        const found = this.findOutlineNodeById(node.children, id);
        if (found) return found;
      }
    }
    return null;
  }

  private findOutlineNodeByName(
    nodes: SymbolOutlineNodeDto[],
    name: string
  ): SymbolOutlineNodeDto | null {
    for (const node of nodes) {
      if (node.name === name) return node;
      if (node.children && node.children.length > 0) {
        const found = this.findOutlineNodeByName(node.children, name);
        if (found) return found;
      }
    }
    return null;
  }

  private findSymbolIdForName(name: string): number | null {
    if (!name) return null;

    if (this.currentOccurrences && this.currentOccurrences.length > 0) {
      const def = this.currentOccurrences.find(
        (o) =>
          o.name === name &&
          o.symbolId != null &&
          (o.occurrenceKind === "definition" || o.occurrenceKind === "declaration")
      );
      if (def && def.symbolId != null) return def.symbolId;

      const anyOcc = this.currentOccurrences.find(
        (o) => o.name === name && o.symbolId != null
      );
      if (anyOcc && anyOcc.symbolId != null) return anyOcc.symbolId;
    }

    if (this.currentOutline && this.currentOutline.length > 0) {
      const node = this.findOutlineNodeByName(this.currentOutline, name);
      if (node) return node.id;
    }

    return null;
  }

  updateSelectedSymbol(): void {
    this.viewerContainer
      .querySelectorAll(".symbol-selected, .symbol-occurrence-selected")
      .forEach((el) => {
        el.classList.remove("symbol-selected", "symbol-occurrence-selected");
      });

    const state = this.store.getState();
    const selSymbolId = state.selectedSymbolId;
    if (selSymbolId === null) {
      return;
    }

    let symName: string | null = null;
    const occ =
      this.currentOccurrences.find(
        (o) =>
          o.symbolId === selSymbolId &&
          (o.occurrenceKind === "definition" || o.occurrenceKind === "declaration")
      ) ||
      this.currentOccurrences.find((o) => o.symbolId === selSymbolId);

    if (occ) {
      symName = occ.name;
    } else if (this.currentOutline.length > 0) {
      const outlineNode = this.findOutlineNodeById(this.currentOutline, selSymbolId);
      if (outlineNode) {
        symName = outlineNode.name;
      }
    }

    const matches: HTMLElement[] = [];
    const idMatches = this.viewerContainer.querySelectorAll<HTMLElement>(
      `[data-symbol-id="${escapeCssAttr(String(selSymbolId))}"]`
    );
    idMatches.forEach((el) => matches.push(el));

    if (symName) {
      const nameMatches = this.viewerContainer.querySelectorAll<HTMLElement>(
        `[data-symbol-name="${escapeCssAttr(symName)}"]`
      );
      nameMatches.forEach((el) => {
        if (!matches.includes(el)) {
          matches.push(el);
        }
      });
    }

    if (matches.length === 0) {
      return;
    }

    const selLine = state.selectedLine;
    let primaryElem: HTMLElement | null = null;

    if (selLine !== null) {
      primaryElem =
        matches.find((el) => {
          const lineParent = el.closest(".code-line") as HTMLElement | null;
          return lineParent && lineParent.dataset.line === String(selLine);
        }) || null;
    }

    if (!primaryElem) {
      primaryElem = matches[0];
    }

    primaryElem.classList.add("symbol-selected");

    for (const el of matches) {
      if (el !== primaryElem) {
        el.classList.add("symbol-occurrence-selected");
      }
    }
  }

  private selectSymbolByName(name: string, primaryTarget?: HTMLElement): void {
    this.viewerContainer
      .querySelectorAll(".symbol-selected, .symbol-occurrence-selected")
      .forEach((el) => {
        el.classList.remove("symbol-selected", "symbol-occurrence-selected");
      });

    if (!name) return;

    const matches = this.viewerContainer.querySelectorAll<HTMLElement>(
      `[data-symbol-name="${escapeCssAttr(name)}"]`
    );
    matches.forEach((el) => {
      if (el === primaryTarget) {
        el.classList.add("symbol-selected");
      } else {
        el.classList.add("symbol-occurrence-selected");
      }
    });

    if (primaryTarget && !primaryTarget.classList.contains("symbol-selected")) {
      primaryTarget.classList.add("symbol-selected");
    }
  }

  private resolveSelectedOrSymbolLine(): number | null {
    const state = this.store.getState();
    if (state.selectedLine !== null) {
      return state.selectedLine;
    }
    if (state.selectedSymbolId !== null) {
      if (this.currentOccurrences.length > 0) {
        const occ =
          this.currentOccurrences.find(
            (o) =>
              o.symbolId === state.selectedSymbolId &&
              (o.occurrenceKind === "definition" || o.occurrenceKind === "declaration")
          ) ||
          this.currentOccurrences.find(
            (o) => o.symbolId === state.selectedSymbolId
          );
        if (occ) {
          return occ.range.start.line;
        }
      }
      if (this.currentOutline.length > 0) {
        const node = this.findOutlineNodeById(this.currentOutline, state.selectedSymbolId);
        if (node && node.range?.start?.line) {
          return node.range.start.line;
        }
      }
    }
    return null;
  }

  private initHoverEvents(): void {
    this.viewerContainer.addEventListener("mouseover", (e) => {
      const target = (e.target as HTMLElement).closest<HTMLElement>(
        ".symbol-token, .tok, span"
      );
      if (!target || !this.viewerContainer.contains(target)) return;
      if (!target.closest(".line-content")) return;

      const isSymbol =
        target.classList.contains("symbol-token") ||
        Boolean(target.dataset.symbolName) ||
        Boolean(target.dataset.symbolId) ||
        (target.classList.contains("tok") &&
          !target.classList.contains("tok-keyword") &&
          !target.classList.contains("tok-operator") &&
          !target.classList.contains("tok-string") &&
          !target.classList.contains("tok-number") &&
          !target.classList.contains("tok-comment"));

      if (!isSymbol) return;

      if (this.activeHoveredElement === target) return;

      this.clearHoverHighlights();
      this.activeHoveredElement = target;

      target.classList.add("symbol-hovered");

      const symId = target.dataset.symbolId;
      const symName = target.dataset.symbolName || target.textContent?.trim();

      if (symId) {
        const matches = this.viewerContainer.querySelectorAll<HTMLElement>(
          `[data-symbol-id="${escapeCssAttr(symId)}"]`
        );
        matches.forEach((el) => {
          if (el !== target) {
            el.classList.add("symbol-highlighted");
          }
        });
      } else if (symName) {
        const matches = this.viewerContainer.querySelectorAll<HTMLElement>(
          `[data-symbol-name="${escapeCssAttr(symName)}"]`
        );
        matches.forEach((el) => {
          if (el !== target) {
            el.classList.add("symbol-highlighted");
          }
        });
      }
    });

    this.viewerContainer.addEventListener("mouseout", (e) => {
      const related = e.relatedTarget as HTMLElement | null;
      if (this.activeHoveredElement) {
        if (!related || !this.activeHoveredElement.contains(related)) {
          this.clearHoverHighlights();
        }
      }
    });

    this.viewerContainer.addEventListener("mouseleave", () => {
      this.clearHoverHighlights();
    });
  }

  private clearHoverHighlights(): void {
    if (this.activeHoveredElement) {
      this.activeHoveredElement.classList.remove("symbol-hovered");
      this.activeHoveredElement = null;
    }
    this.viewerContainer
      .querySelectorAll(".symbol-hovered, .symbol-highlighted")
      .forEach((el) => {
        el.classList.remove("symbol-hovered", "symbol-highlighted");
      });
  }

  private clearFile(): void {
    this.clearHoverHighlights();
    ++this.loadRequestId;
    this.currentFile = null;
    this.currentContent = null;
    this.currentHighlights = null;
    this.currentOccurrences = [];
    this.currentOutline = [];
    this.currentCompileCommand = null;
    this.pathElem.textContent = "No file open";
    this.langBadge.style.display = "none";
    this.compileBadge.style.display = "none";
    this.navControls.innerHTML = "";
    this.bannerContainer.innerHTML = "";
    this.renderTabs();
    this.viewerContainer.innerHTML = `
      <div class="empty-state">
        <div class="empty-state-title">No file selected</div>
        <div class="empty-state-desc">Select a file from the explorer to view its contents.</div>
      </div>
    `;
  }

  private renderTabs(): void {
    const { openTabs, selectedFileId } = this.store.getState();
    if (!openTabs || openTabs.length === 0) {
      this.tabsBar.innerHTML = "";
      this.tabsBar.style.display = "none";
      return;
    }

    this.tabsBar.style.display = "flex";
    this.tabsBar.innerHTML = "";
    const fragment = document.createDocumentFragment();

    for (const tab of openTabs) {
      const tabDiv = document.createElement("div");
      tabDiv.className = `code-tab${tab.fileId === selectedFileId ? " active" : ""}`;
      tabDiv.dataset.fileId = String(tab.fileId);
      tabDiv.title = tab.relativePath || tab.name;

      const titleSpan = document.createElement("span");
      titleSpan.className = "code-tab-title";
      titleSpan.textContent = tab.name || tab.relativePath || `File #${tab.fileId}`;
      tabDiv.appendChild(titleSpan);

      const closeBtn = document.createElement("button");
      closeBtn.className = "code-tab-close";
      closeBtn.title = "Close tab";
      closeBtn.textContent = "✕";
      closeBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        this.store.closeTab(tab.fileId);
      });
      tabDiv.appendChild(closeBtn);

      tabDiv.addEventListener("click", () => {
        this.store.selectFile(tab.fileId);
      });

      fragment.appendChild(tabDiv);
    }

    this.tabsBar.appendChild(fragment);

    // Scroll active tab into view
    const activeTabElem = this.tabsBar.querySelector<HTMLElement>(".code-tab.active");
    activeTabElem?.scrollIntoView?.({ behavior: "auto", block: "nearest", inline: "nearest" });
  }
}
