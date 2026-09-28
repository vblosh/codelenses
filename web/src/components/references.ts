import type { ReferencerDto, SymbolDetailDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";

export class ReferencesComponent {
  private element: HTMLElement;
  private store: StateStore;
  private headerContainer!: HTMLElement;
  private listContainer!: HTMLElement;

  private currentSymbolId: number | null = null;
  private currentItems: ReferencerDto[] = [];
  private totalCount: number = 0;
  private hasMore: boolean = false;
  private pageSize: number = 50;
  private isLoadingMore: boolean = false;

  constructor(store: StateStore) {
    this.store = store;
    this.element = document.createElement("div");
    this.element.className = "inspector-pane-view";
    this.element.id = "inspector-references";
    this.render();
    this.initEvents();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="symbol-detail-header" style="display: none;"></div>
      <div class="references-list" style="flex: 1; overflow-y: auto;">
        <div class="empty-state">
          <div class="empty-state-title">No symbol selected</div>
          <div class="empty-state-desc">Click on a symbol in the code window or outline to view references.</div>
        </div>
      </div>
    `;

    this.headerContainer = this.element.querySelector(".symbol-detail-header")!;
    this.listContainer = this.element.querySelector(".references-list")!;
  }

  private initEvents(): void {
    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("selectedSymbolId")) {
        this.loadReferences(state.selectedSymbolId);
      }
    });
  }

  async loadReferences(symbolId: number | null): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    this.currentSymbolId = symbolId;
    this.currentItems = [];
    this.totalCount = 0;
    this.hasMore = false;
    this.isLoadingMore = false;

    if (!wsId || !symbolId) {
      this.headerContainer.style.display = "none";
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No symbol selected</div>
          <div class="empty-state-desc">Click on a symbol in the code window or outline to view references.</div>
        </div>
      `;
      return;
    }

    try {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Loading references...</div>
        </div>
      `;

      // Fetch detail and references in parallel
      const [detailRes, refRes] = await Promise.all([
        api.getSymbolDetail(wsId, symbolId).catch(() => null),
        api.getSymbolReferences(wsId, symbolId, this.pageSize, 0),
      ]);

      this.currentItems = refRes.items || [];
      this.totalCount = refRes.total ?? this.currentItems.length;
      this.hasMore = refRes.hasMore ?? (this.currentItems.length < this.totalCount);

      this.renderHeader(detailRes);
      this.renderReferencesList();
    } catch (err: any) {
      this.headerContainer.style.display = "none";
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title" style="color: var(--error);">Error loading references</div>
          <div class="empty-state-desc">${err.message}</div>
        </div>
      `;
    }
  }

  async loadMoreReferences(): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || !this.currentSymbolId || this.isLoadingMore || !this.hasMore) {
      return;
    }

    this.isLoadingMore = true;
    const loadMoreBtn = this.listContainer.querySelector(".load-more-refs-btn") as HTMLButtonElement | null;
    if (loadMoreBtn) {
      loadMoreBtn.disabled = true;
      loadMoreBtn.textContent = "Loading more...";
    }

    try {
      const nextRes = await api.getSymbolReferences(
        wsId,
        this.currentSymbolId,
        this.pageSize,
        this.currentItems.length
      );

      const newItems = nextRes.items || [];
      this.currentItems = this.currentItems.concat(newItems);
      this.totalCount = nextRes.total ?? this.totalCount;
      this.hasMore = nextRes.hasMore ?? (this.currentItems.length < this.totalCount);

      this.renderReferencesList();
    } catch (err: any) {
      if (loadMoreBtn) {
        loadMoreBtn.disabled = false;
        loadMoreBtn.textContent = `Error loading more (${err.message}) - Retry`;
      }
    } finally {
      this.isLoadingMore = false;
    }
  }

  private renderHeader(detail: SymbolDetailDto | null): void {
    if (!detail || !detail.symbol) {
      this.headerContainer.style.display = "none";
      return;
    }

    this.headerContainer.innerHTML = "";
    this.headerContainer.style.display = "block";
    const sym = detail.symbol;

    const titleDiv = document.createElement("div");
    titleDiv.className = "symbol-detail-title";

    const kindBadge = document.createElement("span");
    kindBadge.className = `kind-badge ${sym.kind.toLowerCase()}`;
    kindBadge.textContent = sym.kind;
    titleDiv.appendChild(kindBadge);

    const nameSpan = document.createElement("span");
    nameSpan.textContent = sym.name;
    titleDiv.appendChild(nameSpan);

    this.headerContainer.appendChild(titleDiv);

    if (sym.signature) {
      const sigDiv = document.createElement("div");
      sigDiv.className = "symbol-signature";
      sigDiv.textContent = sym.signature;
      this.headerContainer.appendChild(sigDiv);
    }

    // Resolve target declaration: prefer explicit declaration from declarations,
    // falling back to symbol's own file and line
    let targetFileId = detail.file?.id ?? sym.fileId;
    let targetLine = sym.range?.start?.line ?? 1;
    let targetFilePath = detail.file?.relativePath || detail.file?.name || "Unknown";

    const decls = detail.declarations || [];
    const explicitDecl =
      decls.find((d) => d.isDeclaration && d.id !== sym.id) ||
      decls.find((d) => !d.isDefinition && d.id !== sym.id) ||
      decls.find((d) => d.isDeclaration) ||
      decls.find((d) => !d.isDefinition);

    if (explicitDecl) {
      targetFileId = explicitDecl.fileId;
      if (explicitDecl.range?.start?.line) {
        targetLine = explicitDecl.range.start.line;
      }
      if (explicitDecl.relativePath) {
        targetFilePath = explicitDecl.relativePath;
      } else if (explicitDecl.fileId === detail.file?.id) {
        targetFilePath = detail.file?.relativePath || detail.file?.name || "Unknown";
      } else {
        targetFilePath = `File #${explicitDecl.fileId}`;
      }
    }

    if (!targetLine || targetLine < 1) {
      targetLine = 1;
    }

    const declDiv = document.createElement("div");
    declDiv.className = "symbol-declaration";

    const declLabel = document.createElement("span");
    declLabel.className = "declaration-label";
    declLabel.textContent = "Declaration:";
    declDiv.appendChild(declLabel);

    const declLink = document.createElement("a");
    declLink.className = "declaration-link";
    declLink.href = "#";
    declLink.textContent = `${targetFilePath}:${targetLine}`;
    declLink.title = `Jump to declaration at ${targetFilePath}:${targetLine}`;

    if (
      explicitDecl &&
      explicitDecl.fileId !== detail.file?.id &&
      !explicitDecl.relativePath
    ) {
      const wsId = this.store.getState().workspaceId;
      if (wsId) {
        api
          .getFileMetadata(wsId, explicitDecl.fileId)
          .then((meta) => {
            if (meta?.relativePath) {
              targetFilePath = meta.relativePath;
              declLink.textContent = `${meta.relativePath}:${targetLine}`;
              declLink.title = `Jump to declaration at ${meta.relativePath}:${targetLine}`;
            }
          })
          .catch(() => {});
      }
    }

    declLink.addEventListener("click", (e) => {
      e.preventDefault();
      this.store.selectFile(
        targetFileId,
        targetLine,
        { relativePath: targetFilePath },
        explicitDecl?.id ?? this.currentSymbolId,
        sym.name
      );
    });

    declDiv.appendChild(declLink);
    this.headerContainer.appendChild(declDiv);

    const metricsDiv = document.createElement("div");
    metricsDiv.className = "symbol-metrics";

    const refMetric = document.createElement("span");
    refMetric.innerHTML = `<strong>${detail.referencersCount}</strong> references`;
    const callerMetric = document.createElement("span");
    callerMetric.innerHTML = `<strong>${detail.callersCount}</strong> callers`;
    const calleeMetric = document.createElement("span");
    calleeMetric.innerHTML = `<strong>${detail.calleesCount}</strong> callees`;

    metricsDiv.appendChild(refMetric);
    metricsDiv.appendChild(document.createTextNode(" • "));
    metricsDiv.appendChild(callerMetric);
    metricsDiv.appendChild(document.createTextNode(" • "));
    metricsDiv.appendChild(calleeMetric);

    this.headerContainer.appendChild(metricsDiv);
  }

  private renderReferencesList(): void {
    if (!this.currentItems || this.currentItems.length === 0) {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No references</div>
          <div class="empty-state-desc">No incoming references found for this symbol.</div>
        </div>
      `;
      return;
    }

    this.listContainer.innerHTML = "";
    const fragment = document.createDocumentFragment();

    for (const ref of this.currentItems) {
      const item = document.createElement("div");
      item.className = "reference-item";

      const lineNum = ref.range.start.line;
      const fileText = `${ref.relativePath || "Unknown file"}:${lineNum}`;
      const kind = ref.referenceKind || "ref";

      const refFileDiv = document.createElement("div");
      refFileDiv.className = "ref-file";

      const fileSpan = document.createElement("span");
      fileSpan.textContent = fileText;
      refFileDiv.appendChild(fileSpan);

      const badgesDiv = document.createElement("div");
      badgesDiv.style.display = "flex";
      badgesDiv.style.gap = "4px";
      badgesDiv.style.alignItems = "center";

      const kindSpan = document.createElement("span");
      kindSpan.className = "ref-kind";
      kindSpan.textContent = kind;
      badgesDiv.appendChild(kindSpan);

      if (ref.resolution === "unresolved") {
        const unresSpan = document.createElement("span");
        unresSpan.className = "badge";
        unresSpan.style.background = "rgba(239, 68, 68, 0.2)";
        unresSpan.style.color = "#f87171";
        unresSpan.textContent = "Unresolved";
        badgesDiv.appendChild(unresSpan);
      } else if (ref.resolution === "ambiguous") {
        const ambigSpan = document.createElement("span");
        ambigSpan.className = "badge";
        ambigSpan.style.background = "rgba(245, 158, 11, 0.2)";
        ambigSpan.style.color = "#fbbf24";
        ambigSpan.textContent = "Ambiguous";
        badgesDiv.appendChild(ambigSpan);
      }

      refFileDiv.appendChild(badgesDiv);

      const snippetDiv = document.createElement("div");
      snippetDiv.className = "ref-snippet";
      snippetDiv.textContent = `Line ${lineNum}: reference to ${ref.name}`;

      item.appendChild(refFileDiv);
      item.appendChild(snippetDiv);

      item.addEventListener("click", () => {
        // Navigate to referencing file and line
        this.store.selectFile(
          ref.fileId,
          lineNum,
          { relativePath: ref.relativePath },
          this.currentSymbolId,
          ref.name
        );
      });

      fragment.appendChild(item);
    }

    this.listContainer.appendChild(fragment);

    if (this.hasMore) {
      const footer = document.createElement("div");
      footer.className = "load-more-container";
      footer.style.padding = "10px";
      footer.style.textAlign = "center";

      const btn = document.createElement("button");
      btn.className = "load-more-btn load-more-refs-btn";
      btn.textContent = `Load more references (${this.currentItems.length} of ${this.totalCount})`;
      btn.addEventListener("click", () => {
        this.loadMoreReferences();
      });

      footer.appendChild(btn);
      this.listContainer.appendChild(footer);
    }
  }

  // Alias for backward-compatibility if called externally
  renderReferences(refs: ReferencerDto[]): void {
    this.currentItems = refs;
    this.renderReferencesList();
  }
}
