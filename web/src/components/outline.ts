import type { SymbolOutlineNodeDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";

export interface OutlineCallbacks {
  onCliMacroClick?: (macroName: string) => void;
}

export class OutlineComponent {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: OutlineCallbacks;
  private filterInput!: HTMLInputElement;
  private listContainer!: HTMLElement;
  private outlineData: SymbolOutlineNodeDto[] = [];

  constructor(store: StateStore, callbacks: OutlineCallbacks = {}) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "inspector-pane-view active";
    this.element.id = "inspector-outline";
    this.render();
    this.initEvents();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div style="padding: 8px 10px; border-bottom: 1px solid var(--border-color);">
        <input type="text" class="search-input outline-filter-input" placeholder="Filter symbols..." style="background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 4px; padding: 4px 8px; width: 100%; font-size: 11px; color: var(--text-primary); outline: none;" />
      </div>
      <div class="outline-list" style="flex: 1; overflow-y: auto;">
        <div class="empty-state">
          <div class="empty-state-title">No outline</div>
          <div class="empty-state-desc">Select a file to see its symbol outline.</div>
        </div>
      </div>
    `;

    this.filterInput = this.element.querySelector(".outline-filter-input")!;
    this.listContainer = this.element.querySelector(".outline-list")!;
  }

  private initEvents(): void {
    this.filterInput.addEventListener("input", () => {
      this.renderOutlineTree();
    });

    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("selectedFileId")) {
        this.loadOutline(state.selectedFileId);
      }
      if (changedKeys.includes("selectedSymbolId")) {
        this.highlightSelectedNode();
      }
    });
  }

  async loadOutline(fileId: number | null): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || !fileId) {
      this.outlineData = [];
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No file selected</div>
          <div class="empty-state-desc">Select a file to see its symbol outline.</div>
        </div>
      `;
      return;
    }

    try {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Loading outline...</div>
        </div>
      `;

      const res = await api.getFileOutline(wsId, fileId);
      this.outlineData = res.outline || [];
      this.renderOutlineTree();
    } catch (err: any) {
      this.outlineData = [];
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No outline available</div>
          <div class="empty-state-desc">${err.message}</div>
        </div>
      `;
    }
  }

  private renderOutlineTree(): void {
    if (!this.outlineData || this.outlineData.length === 0) {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No symbols</div>
          <div class="empty-state-desc">No outline symbols found in this file.</div>
        </div>
      `;
      return;
    }

    const query = this.filterInput.value.trim().toLowerCase();
    this.listContainer.innerHTML = "";
    const fragment = document.createDocumentFragment();

    this.renderNodes(this.outlineData, 0, query, fragment);

    if (fragment.children.length === 0) {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No match</div>
          <div class="empty-state-desc">No symbols matched "${query}".</div>
        </div>
      `;
      return;
    }

    this.listContainer.appendChild(fragment);
    this.highlightSelectedNode();
  }

  private renderNodes(
    nodes: SymbolOutlineNodeDto[],
    level: number,
    query: string,
    container: DocumentFragment | HTMLElement
  ): boolean {
    let hasMatchedAny = false;

    for (const node of nodes) {
      const nameMatches = query ? node.name.toLowerCase().includes(query) : true;
      const childContainer = document.createElement("div");

      let childrenMatched = false;
      if (node.children && node.children.length > 0) {
        childrenMatched = this.renderNodes(node.children, level + 1, query, childContainer);
      }

      if (nameMatches || childrenMatched) {
        hasMatchedAny = true;
        const item = document.createElement("div");
        item.className = "outline-item";
        item.dataset.symbolId = String(node.id);
        item.style.paddingLeft = `${level * 14 + 10}px`;

        const kindBadge = document.createElement("span");
        kindBadge.className = `kind-badge ${node.kind.toLowerCase()}`;
        kindBadge.textContent = node.kind.slice(0, 4);

        const nameSpan = document.createElement("span");
        nameSpan.className = "outline-name";
        nameSpan.style.fontFamily = "var(--font-mono)";
        nameSpan.style.overflow = "hidden";
        nameSpan.style.textOverflow = "ellipsis";
        nameSpan.style.whiteSpace = "nowrap";
        nameSpan.textContent = node.name;
        if (node.signature) {
          nameSpan.title = `${node.name}${node.signature}`;
        }

        const isCliMacro =
          node.kind.toLowerCase() === "macro" &&
          node.range.start.line === 0 &&
          node.range.end.line === 0 &&
          node.range.start.byte === 0 &&
          node.range.end.byte === 0;

        item.appendChild(kindBadge);
        item.appendChild(nameSpan);

        if (isCliMacro) {
          const cliBadge = document.createElement("span");
          cliBadge.className = "badge cli-macro-badge";
          cliBadge.textContent = "CLI";
          cliBadge.title = "Defined via compile-command argument (-D)";
          item.appendChild(cliBadge);
        }

        item.addEventListener("click", () => {
          this.store.selectSymbol(node.id, isCliMacro ? null : node.range.start.line);
          if (isCliMacro && this.callbacks.onCliMacroClick) {
            this.callbacks.onCliMacroClick(node.name);
          }
        });

        container.appendChild(item);

        if (childContainer.children.length > 0) {
          container.appendChild(childContainer);
        }
      }
    }

    return hasMatchedAny;
  }

  private highlightSelectedNode(): void {
    const selId = this.store.getState().selectedSymbolId;
    this.listContainer.querySelectorAll(".outline-item.selected").forEach((el) => {
      el.classList.remove("selected");
    });

    if (selId) {
      const match = this.listContainer.querySelector(
        `.outline-item[data-symbol-id="${selId}"]`
      );
      if (match) {
        match.classList.add("selected");
        match.scrollIntoView?.({ behavior: "smooth", block: "nearest" });
      }
    }
  }
}
