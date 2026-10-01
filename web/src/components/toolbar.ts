import type { WorkspaceStatusDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";

export interface ToolbarCallbacks {
  onSearch: (query: string, type: "source" | "symbol") => void;
  onOpenSettings?: () => void;
  onAddWorkspace?: () => void;
  onManageLinks?: () => void;
}

export class ToolbarComponent {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: ToolbarCallbacks;
  private workspaceSelect!: HTMLSelectElement;
  private settingsBtn!: HTMLButtonElement;
  private addWsBtn!: HTMLButtonElement;
  private manageLibsBtn!: HTMLButtonElement;
  private statusBadge!: HTMLElement;
  private statusText!: HTMLElement;
  private indexBtn!: HTMLButtonElement;
  private searchInput!: HTMLInputElement;
  private searchModeSelect!: HTMLSelectElement;
  private statusPollTimer: number | null = null;

  constructor(store: StateStore, callbacks: ToolbarCallbacks) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("header");
    this.element.className = "app-toolbar";
    this.render();
    this.initEvents();
    this.loadWorkspaces();
    this.startPollingStatus();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="toolbar-section">
        <a href="#" class="brand" title="CodeLenses Browser">
          <svg class="brand-icon" viewBox="0 0 24 24">
            <path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/>
          </svg>
          <span>CodeLenses</span>
        </a>

        <div class="workspace-select-wrapper" style="display: flex; align-items: center; gap: 6px;">
          <select class="workspace-select" aria-label="Select Workspace" title="Select Workspace">
            <option value="">Loading workspaces...</option>
          </select>
          <button class="btn-icon ws-settings-btn" title="Workspace settings & compilation database">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="12" cy="12" r="3"></circle>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path>
            </svg>
          </button>
          <button class="btn-icon add-workspace-btn" title="Add workspace">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="12" y1="5" x2="12" y2="19"></line>
              <line x1="5" y1="12" x2="19" y2="12"></line>
            </svg>
            <span>Add</span>
          </button>
          <button class="btn-icon manage-links-btn" title="Manage linked workspaces">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
              <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
            </svg>
            <span>Links</span>
          </button>
        </div>
      </div>

      <div class="toolbar-section">
        <div class="search-box-wrapper">
          <div class="search-input-group">
            <svg class="search-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="11" cy="11" r="8"></circle>
              <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
            </svg>
            <input type="text" class="search-input" placeholder="Search codebase (/ to focus)..." aria-label="Search" />
            <select class="search-mode-select" title="Search mode">
              <option value="source">Source</option>
              <option value="symbol">Symbol</option>
            </select>
          </div>
        </div>
      </div>

      <div class="toolbar-section">
        <div class="index-status-badge status-idle" title="Indexing status">
          <span class="status-dot"></span>
          <span class="status-text">Idle</span>
        </div>
        <button class="btn-icon index-btn" title="Re-index workspace">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          <span>Index</span>
        </button>
      </div>
    `;

    this.workspaceSelect = this.element.querySelector(".workspace-select")!;
    this.settingsBtn = this.element.querySelector(".ws-settings-btn")!;
    this.addWsBtn = this.element.querySelector(".add-workspace-btn")!;
    this.manageLibsBtn = this.element.querySelector(".manage-links-btn")!;
    this.statusBadge = this.element.querySelector(".index-status-badge")!;
    this.statusText = this.element.querySelector(".status-text")!;
    this.indexBtn = this.element.querySelector(".index-btn")!;
    this.searchInput = this.element.querySelector(".search-input")!;
    this.searchModeSelect = this.element.querySelector(".search-mode-select")!;
  }

  private initEvents(): void {
    this.settingsBtn.addEventListener("click", () => {
      if (this.callbacks.onOpenSettings) {
        this.callbacks.onOpenSettings();
      }
    });

    this.addWsBtn.addEventListener("click", () => {
      if (this.callbacks.onAddWorkspace) {
        this.callbacks.onAddWorkspace();
      }
    });

    this.manageLibsBtn.addEventListener("click", () => {
      if (this.callbacks.onManageLinks) {
        this.callbacks.onManageLinks();
      }
    });

    this.workspaceSelect.addEventListener("change", () => {
      const val = this.workspaceSelect.value;
      const wsId = val ? parseInt(val, 10) : null;
      this.store.setWorkspace(wsId);
      this.fetchStatus();
    });

    this.indexBtn.addEventListener("click", async () => {
      const wsId = this.store.getState().workspaceId;
      if (!wsId) return;
      try {
        this.indexBtn.disabled = true;
        this.updateStatusDisplay("running", "Starting index...");
        await api.triggerIndexing(wsId, "incremental", false);
        this.fetchStatus();
      } catch (err: any) {
        alert(`Indexing trigger failed: ${err.message}`);
      } finally {
        this.indexBtn.disabled = false;
      }
    });

    // Search events
    const triggerSearch = () => {
      const query = this.searchInput.value.trim();
      if (query) {
        const mode = this.searchModeSelect.value as "source" | "symbol";
        this.callbacks.onSearch(query, mode);
      }
    };

    this.searchInput.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        triggerSearch();
      }
    });

    // Global shortcut '/' to focus search
    window.addEventListener("keydown", (e) => {
      if (e.key === "/" && document.activeElement !== this.searchInput && !["INPUT", "TEXTAREA"].includes(document.activeElement?.tagName || "")) {
        e.preventDefault();
        this.searchInput.focus();
        this.searchInput.select();
      }
    });

    // Store sync
    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("workspaceId") && state.workspaceId !== null) {
        if (this.workspaceSelect.value !== String(state.workspaceId)) {
          this.workspaceSelect.value = String(state.workspaceId);
        }
        this.fetchStatus();
      }
    });
  }

  async loadWorkspaces(preferredWorkspaceId?: number): Promise<void> {
    try {
      const response = await api.getWorkspaces();
      this.workspaceSelect.innerHTML = "";

      if (!response.workspaces || response.workspaces.length === 0) {
        const opt = document.createElement("option");
        opt.value = "";
        opt.textContent = "No workspaces found";
        this.workspaceSelect.appendChild(opt);
        return;
      }

      for (const ws of response.workspaces) {
        const opt = document.createElement("option");
        opt.value = String(ws.id);
        opt.textContent = `${ws.name || ws.rootPath} (#${ws.id})`;
        this.workspaceSelect.appendChild(opt);
      }

      const targetId = preferredWorkspaceId ?? this.store.getState().workspaceId;
      const targetExists = targetId ? response.workspaces.some((w) => w.id === targetId) : false;

      if (targetExists && targetId) {
        this.workspaceSelect.value = String(targetId);
        if (this.store.getState().workspaceId !== targetId) {
          this.store.setWorkspace(targetId);
        }
      } else {
        const firstId = response.workspaces[0].id;
        this.workspaceSelect.value = String(firstId);
        this.store.setWorkspace(firstId);
      }
      this.fetchStatus();
    } catch (err: any) {
      this.workspaceSelect.innerHTML = `<option value="">Error: ${err.message}</option>`;
    }
  }

  private startPollingStatus(): void {
    if (this.statusPollTimer) {
      clearInterval(this.statusPollTimer);
    }
    this.statusPollTimer = window.setInterval(() => {
      this.fetchStatus();
    }, 3000);
  }

  async fetchStatus(): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId) return;

    try {
      const statusDto: WorkspaceStatusDto = await api.getWorkspaceStatus(wsId);
      let statusKind: "idle" | "running" | "failed" = "idle";
      let text = "Idle";

      if (statusDto.status === "indexing" || statusDto.status === "running") {
        statusKind = "running";
        if (statusDto.latestJob) {
          text = `Indexing (${statusDto.latestJob.filesProcessed}/${statusDto.latestJob.filesTotal || "?"})`;
        } else {
          text = "Indexing...";
        }
      } else if (statusDto.status === "failed") {
        statusKind = "failed";
        text = "Index failed";
      } else {
        text = `Indexed (${statusDto.fileCount} files)`;
      }

      this.updateStatusDisplay(statusKind, text);
      this.store.setIndexStatus(statusKind);
    } catch {
      // Quietly ignore intermittent status poll failure
    }
  }

  private updateStatusDisplay(status: "idle" | "running" | "failed", text: string): void {
    this.statusBadge.className = `index-status-badge status-${status}`;
    this.statusText.textContent = text;
  }

  destroy(): void {
    if (this.statusPollTimer) {
      clearInterval(this.statusPollTimer);
      this.statusPollTimer = null;
    }
  }
}
