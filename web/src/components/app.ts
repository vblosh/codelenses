import type { StateStore } from "../state";
import type { DiagnosticItem, SourceSearchHitDto, SymbolSearchHitDto } from "../types";
import { api } from "../api";
import { ToolbarComponent } from "./toolbar";
import { ExplorerComponent } from "./explorer";
import { CodeWindowComponent } from "./code-window";
import { OutlineComponent } from "./outline";
import { ReferencesComponent } from "./references";
import { DiagnosticsComponent } from "./diagnostics";
import { CompileCommandComponent } from "./compile-command";
import { WorkspaceSettingsModal } from "./workspace-settings";
import { AddWorkspaceModal } from "./add-workspace";
import { LibraryManagerModal } from "./library-manager";

export class AppComponent {
  private container: HTMLElement;
  private store: StateStore;

  private toolbar!: ToolbarComponent;
  private explorer!: ExplorerComponent;
  private codeWindow!: CodeWindowComponent;
  private outline!: OutlineComponent;
  private references!: ReferencesComponent;
  private diagnostics!: DiagnosticsComponent;
  private compileCommand!: CompileCommandComponent;
  private settingsModal!: WorkspaceSettingsModal;
  private addWorkspaceModal!: AddWorkspaceModal;
  private libraryManagerModal!: LibraryManagerModal;

  private mainGrid!: HTMLElement;
  private mobileTabsBar!: HTMLElement;

  // Search modal elements
  private searchModalBackdrop!: HTMLElement;
  private searchModalInput!: HTMLInputElement;
  private searchResultsList!: HTMLElement;
  private searchModeSelect!: HTMLSelectElement;

  constructor(container: HTMLElement, store: StateStore) {
    this.container = container;
    this.store = store;
    this.initLayout();
    this.initSearchModal();
    this.initMobileTabs();
    this.initEvents();
  }

  private initLayout(): void {
    this.container.innerHTML = "";

    // 1. Modals
    this.libraryManagerModal = new LibraryManagerModal(this.store, {
      onLibrariesChanged: () => {
        this.settingsModal.refreshLibraries();
      },
    });

    this.settingsModal = new WorkspaceSettingsModal(this.store, {
      onSaved: () => {
        this.toolbar.loadWorkspaces();
        this.reconcile();
      },
      onDeleted: () => {
        this.toolbar.loadWorkspaces();
        this.reconcile();
      },
      onManageLibraries: () => {
        this.libraryManagerModal.open();
      },
      onLibrariesChanged: () => {
        this.libraryManagerModal.loadLibraries();
      },
    });

    this.addWorkspaceModal = new AddWorkspaceModal({
      onCreated: (ws) => {
        this.toolbar.loadWorkspaces(ws.id);
        this.store.setWorkspace(ws.id);
        this.reconcile();
      },
    });

    // 2. Toolbar
    this.toolbar = new ToolbarComponent(this.store, {
      onSearch: (q, type) => this.openSearch(q, type),
      onOpenSettings: () => this.settingsModal.open(),
      onAddWorkspace: () => this.addWorkspaceModal.open(),
      onManageLibraries: () => this.libraryManagerModal.open(),
    });
    this.container.appendChild(this.toolbar.getElement());

    // 3. Mobile Tabs Bar (for narrow screens)
    this.mobileTabsBar = document.createElement("div");
    this.mobileTabsBar.className = "mobile-tabs";
    this.mobileTabsBar.innerHTML = `
      <button class="mobile-tab-btn active" data-tab="explorer">Files</button>
      <button class="mobile-tab-btn" data-tab="code">Code</button>
      <button class="mobile-tab-btn" data-tab="outline">Outline</button>
      <button class="mobile-tab-btn" data-tab="inspector">Inspector</button>
    `;
    this.container.appendChild(this.mobileTabsBar);

    // 4. Main workspace panels
    this.mainGrid = document.createElement("main");
    this.mainGrid.className = "main-grid";
    this.mainGrid.dataset.mobileActive = "explorer";

    // Explorer pane
    this.explorer = new ExplorerComponent(this.store);
    this.mainGrid.appendChild(this.explorer.getElement());

    // Code window pane
    this.codeWindow = new CodeWindowComponent(this.store, {
      onSymbolClick: (symId) => {
        this.switchInspectorTab("references");
        this.references.loadReferences(symId);
      },
      onCompileClick: () => {
        this.switchInspectorTab("compile-command");
      },
    });
    this.mainGrid.appendChild(this.codeWindow.getElement());

    // Outline pane
    const outlinePane = document.createElement("div");
    outlinePane.className = "pane outline-pane";
    outlinePane.id = "pane-outline";
    outlinePane.innerHTML = `<div class="pane-header"><span>Outline</span></div>`;
    this.outline = new OutlineComponent(this.store, {
      onCliMacroClick: () => this.switchInspectorTab("compile-command"),
    });
    outlinePane.appendChild(this.outline.getElement());
    this.mainGrid.appendChild(outlinePane);

    const outlineResizer = document.createElement("div");
    outlineResizer.className = "panel-resizer outline-resizer";
    outlineResizer.setAttribute("role", "separator");
    outlineResizer.setAttribute("aria-orientation", "vertical");
    outlineResizer.setAttribute("aria-label", "Resize Outline panel");
    outlineResizer.setAttribute("aria-valuemin", "180");
    outlineResizer.setAttribute("aria-valuemax", "480");
    outlineResizer.tabIndex = 0;
    this.mainGrid.appendChild(outlineResizer);
    this.initOutlineResizer(outlineResizer);

    // Inspector pane
    const inspectorPane = document.createElement("div");
    inspectorPane.className = "pane";
    inspectorPane.id = "pane-inspector";

    // Inspector tab bar
    const inspectorTabBar = document.createElement("div");
    inspectorTabBar.className = "inspector-tabs";
    inspectorTabBar.innerHTML = `
      <button class="inspector-tab-btn active" data-target="references">References</button>
      <button class="inspector-tab-btn" data-target="diagnostics">
        <span>Diagnostics</span>
        <span class="count-pill diag-count-pill" style="display: none;">0</span>
      </button>
      <button class="inspector-tab-btn" data-target="compile-command">
        <span>Build</span>
        <span class="count-pill build-badge" style="display: none; background: rgba(59, 130, 246, 0.2); color: #60a5fa;">active</span>
      </button>
    `;
    inspectorPane.appendChild(inspectorTabBar);

    // Inspector content views
    this.references = new ReferencesComponent(this.store);
    this.diagnostics = new DiagnosticsComponent(this.store);
    this.compileCommand = new CompileCommandComponent(this.store, {
      onConfigureWorkspace: () => this.settingsModal.open(),
      onConfigureWorkspaceDefault: () => this.settingsModal.open(undefined, true),
    });

    inspectorPane.appendChild(this.references.getElement());
    inspectorPane.appendChild(this.diagnostics.getElement());
    inspectorPane.appendChild(this.compileCommand.getElement());

    this.mainGrid.appendChild(inspectorPane);
    this.container.appendChild(this.mainGrid);
    this.switchInspectorTab(this.store.getState().activeInspectorTab);
  }

  private initOutlineResizer(resizer: HTMLElement): void {
    let storedWidth = 0;
    try {
      storedWidth = Number(localStorage.getItem("codelenses-outline-width"));
    } catch {
      // Keep the default width when browser storage is unavailable.
    }
    let width = Number.isFinite(storedWidth) && storedWidth >= 180 && storedWidth <= 480
      ? storedWidth
      : 260;

    const saveWidth = (): void => {
      try {
        localStorage.setItem("codelenses-outline-width", String(width));
      } catch {
        // Resizing still works for this session when browser storage is unavailable.
      }
    };

    const applyWidth = (nextWidth: number): void => {
      width = Math.max(180, Math.min(480, Math.round(nextWidth)));
      this.mainGrid.style.setProperty("--outline-width", `${width}px`);
      resizer.setAttribute("aria-valuenow", String(width));
    };

    applyWidth(width);

    resizer.addEventListener("pointerdown", (event) => {
      if (window.innerWidth <= 860) return;
      event.preventDefault();
      const startX = event.clientX;
      const startWidth = width;

      const onPointerMove = (moveEvent: PointerEvent): void => {
        applyWidth(startWidth + moveEvent.clientX - startX);
      };
      const onPointerUp = (): void => {
        document.removeEventListener("pointermove", onPointerMove);
        saveWidth();
      };

      document.addEventListener("pointermove", onPointerMove);
      document.addEventListener("pointerup", onPointerUp, { once: true });
    });

    resizer.addEventListener("keydown", (event: KeyboardEvent) => {
      if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
        event.preventDefault();
        applyWidth(width + (event.key === "ArrowRight" ? 10 : -10));
        saveWidth();
      }
    });
  }

  private initMobileTabs(): void {
    const tabButtons = this.mobileTabsBar.querySelectorAll<HTMLButtonElement>(".mobile-tab-btn");
    tabButtons.forEach((btn) => {
      btn.addEventListener("click", () => {
        const tab = btn.dataset.tab as "explorer" | "code" | "outline" | "inspector";
        tabButtons.forEach((b) => b.classList.remove("active"));
        btn.classList.add("active");
        this.mainGrid.dataset.mobileActive = tab;
        this.store.setActiveMobileTab(tab);
      });
    });
  }

  private initSearchModal(): void {
    this.searchModalBackdrop = document.createElement("div");
    this.searchModalBackdrop.className = "search-modal-backdrop";
    this.searchModalBackdrop.style.display = "none";

    this.searchModalBackdrop.innerHTML = `
      <div class="search-modal">
        <div class="search-modal-header">
          <input type="text" class="search-modal-input" placeholder="Type to search..." />
          <select class="search-mode-select search-modal-mode" style="background: var(--bg-primary); color: var(--text-primary); border: 1px solid var(--border-color); border-radius: 6px; padding: 4px 8px; font-size: 12px;">
            <option value="source">Source</option>
            <option value="symbol">Symbol</option>
          </select>
          <button class="btn-icon close-search-btn" title="Close search (Esc)">✕</button>
        </div>
        <div class="search-results-list">
          <div class="empty-state">
            <div class="empty-state-title">Search codebase</div>
            <div class="empty-state-desc">Enter a query to search symbols or source text.</div>
          </div>
        </div>
      </div>
    `;

    document.body.appendChild(this.searchModalBackdrop);

    this.searchModalInput = this.searchModalBackdrop.querySelector(".search-modal-input")!;
    this.searchResultsList = this.searchModalBackdrop.querySelector(".search-results-list")!;
    this.searchModeSelect = this.searchModalBackdrop.querySelector(".search-modal-mode")!;
    const closeBtn = this.searchModalBackdrop.querySelector(".close-search-btn")!;

    closeBtn.addEventListener("click", () => this.closeSearch());
    this.searchModalBackdrop.addEventListener("click", (e) => {
      if (e.target === this.searchModalBackdrop) {
        this.closeSearch();
      }
    });

    let debounceTimer: number | null = null;
    this.searchModalInput.addEventListener("input", () => {
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = window.setTimeout(() => {
        this.performSearch();
      }, 250);
    });

    this.searchModeSelect.addEventListener("change", () => {
      this.performSearch();
    });
  }

  private initEvents(): void {
    // Inspector tab switching
    const inspectorTabs = this.container.querySelectorAll<HTMLButtonElement>(".inspector-tab-btn");
    inspectorTabs.forEach((btn) => {
      btn.addEventListener("click", () => {
        const target = btn.dataset.target as "references" | "diagnostics" | "compile-command";
        this.switchInspectorTab(target);
      });
    });

    // Global keyboard shortcuts
    window.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && this.searchModalBackdrop.style.display !== "none") {
        this.closeSearch();
      }
      if ((e.ctrlKey || e.metaKey) && e.key === "k") {
        e.preventDefault();
        this.openSearch();
      }
    });

    // Store sync for mobile tabs, inspector tabs, diagnostics, and symbol references
    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("activeMobileTab")) {
        this.mainGrid.dataset.mobileActive = state.activeMobileTab;
        const btns = this.mobileTabsBar.querySelectorAll<HTMLButtonElement>(".mobile-tab-btn");
        btns.forEach((b) => {
          b.classList.toggle("active", b.dataset.tab === state.activeMobileTab);
        });
      }
      if (changedKeys.includes("activeInspectorTab")) {
        this.switchInspectorTab(state.activeInspectorTab);
      }
      if (changedKeys.includes("selectedFileId")) {
        this.updateBuildBadge(state.selectedFileId);
      }
      if (changedKeys.includes("workspaceId") || changedKeys.includes("selectedFileId")) {
        if (state.workspaceId !== null) {
          this.loadDiagnostics(state.workspaceId, state.selectedFileId);
        } else {
          this.diagnostics.setDiagnostics([]);
          this.codeWindow.setDiagnostics([]);
          this.updateDiagnosticsCount(0);
          this.updateBuildBadge(null);
        }
      }
      if (changedKeys.includes("selectedSymbolId") && state.selectedSymbolId !== null) {
        this.switchInspectorTab("references");
        this.references.loadReferences(state.selectedSymbolId);
      }
    });
  }

  async loadDiagnostics(workspaceId: number, fileId?: number | null): Promise<void> {
    try {
      const [wsDiagsRes, fileDiagsRes] = await Promise.all([
        api.getWorkspaceDiagnostics(workspaceId).catch(() => null),
        fileId ? api.getFileDiagnostics(workspaceId, fileId).catch(() => null) : Promise.resolve(null),
      ]);

      const normalize = (d: any): DiagnosticItem => ({
        ...d,
        line: (d.line != null ? d.line : 0) + 1,
        column: d.column != null ? d.column + 1 : undefined,
        endLine: d.endLine != null ? d.endLine + 1 : undefined,
        endColumn: d.endColumn != null ? d.endColumn + 1 : undefined,
      });

      const wsDiags: DiagnosticItem[] = (wsDiagsRes?.diagnostics || []).map(normalize);
      const fileDiags: DiagnosticItem[] = fileDiagsRes
        ? (fileDiagsRes.diagnostics || []).map(normalize)
        : (fileId ? wsDiags.filter((d) => d.fileId === fileId) : []);

      // 1. Pass file diagnostics to CodeWindow gutter
      this.codeWindow.setDiagnostics(fileId ? fileDiags : []);

      // 2. Pass diagnostics to DiagnosticsComponent inspector pane
      const displayDiags = fileId && fileDiags.length > 0 ? fileDiags : wsDiags;
      this.diagnostics.setDiagnostics(displayDiags);

      // 3. Update tab count pill
      this.updateDiagnosticsCount(displayDiags.length);
    } catch (err) {
      console.error("Failed to load diagnostics:", err);
    }
  }

  private updateDiagnosticsCount(count: number): void {
    const diagCountPill = this.container.querySelector(".diag-count-pill") as HTMLElement | null;
    if (diagCountPill) {
      diagCountPill.textContent = String(count);
      diagCountPill.style.display = count > 0 ? "inline-block" : "none";
    }
  }

  async reconcile(): Promise<void> {
    const state = this.store.getState();
    if (state.workspaceId !== null) {
      const promises: Promise<any>[] = [
        this.toolbar.fetchStatus(),
        this.explorer.loadRootTree(),
        this.loadDiagnostics(state.workspaceId, state.selectedFileId),
      ];

      if (state.selectedFileId !== null) {
        promises.push(this.codeWindow.loadFile(state.selectedFileId));
        promises.push(this.outline.loadOutline(state.selectedFileId));
        promises.push(this.compileCommand.loadCompileCommand(state.selectedFileId));
        promises.push(this.updateBuildBadge(state.selectedFileId));
      } else {
        promises.push(this.updateBuildBadge(null));
      }

      if (state.selectedSymbolId !== null) {
        this.switchInspectorTab("references");
        promises.push(this.references.loadReferences(state.selectedSymbolId));
      }

      await Promise.all(promises);

      if (state.selectedLine !== null) {
        this.codeWindow.scrollToLine(state.selectedLine);
      }
    }
  }

  private switchInspectorTab(
    tab: "references" | "diagnostics" | "compile-command"
  ): void {
    const tabs = this.container.querySelectorAll<HTMLButtonElement>(".inspector-tab-btn");
    tabs.forEach((b) => {
      b.classList.toggle("active", b.dataset.target === tab);
    });

    this.references.getElement().classList.toggle("active", tab === "references");
    this.diagnostics.getElement().classList.toggle("active", tab === "diagnostics");
    this.compileCommand.getElement().classList.toggle("active", tab === "compile-command");

    this.store.setActiveInspectorTab(tab);
  }

  private async updateBuildBadge(fileId: number | null): Promise<void> {
    const badge = this.container.querySelector(".build-badge") as HTMLElement | null;
    if (!badge) return;
    const wsId = this.store.getState().workspaceId;
    if (!wsId || !fileId) {
      badge.style.display = "none";
      return;
    }
    try {
      const cmd = await api.getFileCompileCommand(wsId, fileId);
      if (cmd.hasCompileCommand && cmd.compileCommand) {
        badge.style.display = "inline-block";
        badge.textContent = cmd.compileCommand.languageStandard?.toUpperCase() || "active";
      } else {
        badge.style.display = "none";
      }
    } catch {
      badge.style.display = "none";
    }
  }

  openSearch(initialQuery: string = "", type: "source" | "symbol" = "source"): void {
    this.searchModalBackdrop.style.display = "flex";
    this.searchModeSelect.value = type;
    if (initialQuery) {
      this.searchModalInput.value = initialQuery;
      this.performSearch();
    } else {
      this.searchModalInput.focus();
    }
  }

  closeSearch(): void {
    this.searchModalBackdrop.style.display = "none";
  }

  private async performSearch(): Promise<void> {
    const query = this.searchModalInput.value.trim();
    const wsId = this.store.getState().workspaceId;

    if (!wsId || !query) {
      this.searchResultsList.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Search codebase</div>
          <div class="empty-state-desc">Enter a query to search symbols or source text.</div>
        </div>
      `;
      return;
    }

    const mode = this.searchModeSelect.value as "source" | "symbol";

    try {
      this.searchResultsList.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Searching...</div>
        </div>
      `;

      if (mode === "symbol") {
        const res = await api.searchSymbols(wsId, query, 50);
        this.renderSymbolSearchResults(res.items || []);
      } else {
        const res = await api.searchSource(wsId, query, 50);
        this.renderSourceSearchResults(res.items || []);
      }
    } catch (err: any) {
      this.searchResultsList.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title" style="color: var(--error);">Search failed</div>
          <div class="empty-state-desc">${err.message}</div>
        </div>
      `;
    }
  }

  private renderSourceSearchResults(items: SourceSearchHitDto[]): void {
    if (items.length === 0) {
      this.searchResultsList.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No results</div>
          <div class="empty-state-desc">No source code matched your query.</div>
        </div>
      `;
      return;
    }

    this.searchResultsList.innerHTML = "";
    const fragment = document.createDocumentFragment();

    for (const item of items) {
      const div = document.createElement("div");
      div.className = "search-hit-item";

      const titleDiv = document.createElement("div");
      titleDiv.className = "search-hit-title";
      const pathSpan = document.createElement("span");
      pathSpan.textContent = item.relativePath || "File";
      titleDiv.appendChild(pathSpan);
      this.appendLibraryOriginBadge(titleDiv, item);

      const snippetDiv = document.createElement("div");
      snippetDiv.className = "search-hit-snippet";
      snippetDiv.textContent = item.snippet || "";

      div.appendChild(titleDiv);
      div.appendChild(snippetDiv);

      div.addEventListener("click", () => {
        this.store.selectFile(item.fileId);
        this.closeSearch();
      });

      fragment.appendChild(div);
    }

    this.searchResultsList.appendChild(fragment);
  }

  private renderSymbolSearchResults(items: SymbolSearchHitDto[]): void {
    if (items.length === 0) {
      this.searchResultsList.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No symbols found</div>
          <div class="empty-state-desc">No symbols matched your query.</div>
        </div>
      `;
      return;
    }

    this.searchResultsList.innerHTML = "";
    const fragment = document.createDocumentFragment();

    for (const item of items) {
      const div = document.createElement("div");
      div.className = "search-hit-item";

      const titleDiv = document.createElement("div");
      titleDiv.className = "search-hit-title";

      const nameSpan = document.createElement("span");
      nameSpan.textContent = item.name || "";
      titleDiv.appendChild(nameSpan);

      const kindBadge = document.createElement("span");
      kindBadge.className = `kind-badge ${(item.kind || "symbol").toLowerCase()}`;
      kindBadge.textContent =
        item.kind === "unresolved_call" ? "unresolved call" : item.kind || "symbol";
      titleDiv.appendChild(kindBadge);
      this.appendLibraryOriginBadge(titleDiv, item);

      const snippetDiv = document.createElement("div");
      snippetDiv.className = "search-hit-snippet";
      const rel = item.relativePath || "";
      const qual = item.qualifiedName ? ` (${item.qualifiedName})` : "";
      snippetDiv.textContent = `${rel}${qual}`;

      div.appendChild(titleDiv);
      div.appendChild(snippetDiv);

      div.addEventListener("click", () => {
        this.store.selectFile(item.fileId);
        this.store.selectSymbol(item.id, item.line);
        this.closeSearch();
      });

      fragment.appendChild(div);
    }

    this.searchResultsList.appendChild(fragment);
  }

  private appendLibraryOriginBadge(
    container: HTMLElement,
    item: {
      origin?: string;
      targetFramework?: string | null;
      libraryProfileId?: number | null;
      ownerWorkspaceId?: number;
    },
  ): void {
    if (item.origin !== "library") return;
    const badge = document.createElement("span");
    badge.className = "badge";
    badge.textContent = item.targetFramework
      ? `Library · ${item.targetFramework}`
      : "Library source";
    badge.title = item.libraryProfileId
      ? `Source from library profile #${item.libraryProfileId}`
      : `Source from workspace #${item.ownerWorkspaceId ?? "?"}`;
    badge.style.background = "rgba(16, 185, 129, 0.15)";
    badge.style.color = "#34d399";
    badge.style.fontSize = "10px";
    container.appendChild(badge);
  }
}
