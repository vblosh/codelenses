import type { JobDto, WorkspaceStatusDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";

export interface ToolbarCallbacks {
  onSearch: (query: string, type: "source" | "symbol") => void;
  onOpenSettings?: () => void;
  onAddWorkspace?: () => void;
  onManageLinks?: () => void;
  onShowSummary?: () => void;
  onStatus?: (status: WorkspaceStatusDto) => void;
}

export class ToolbarComponent {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: ToolbarCallbacks;
  private workspaceSelect!: HTMLSelectElement;
  private settingsBtn!: HTMLButtonElement;
  private addWsBtn!: HTMLButtonElement;
  private manageLibsBtn!: HTMLButtonElement;
  private summaryBtn!: HTMLButtonElement;
  private statusBadge!: HTMLElement;
  private statusText!: HTMLElement;
  private indexBtn!: HTMLButtonElement;
  private searchInput!: HTMLInputElement;
  private searchModeSelect!: HTMLSelectElement;
  private indexBtnLabel!: HTMLSpanElement;
  private indexBtnIcon!: SVGPathElement;
  private statusPollTimer: number | null = null;
  private statusRequest: AbortController | null = null;
  private statusRequestWorkspaceId: number | null = null;
  private readonly activeJobs = new Map<number, JobDto>();
  private readonly indexingWorkspaces = new Set<number>();
  private readonly loadedStatusWorkspaces = new Set<number>();
  private readonly pendingIndexActions = new Map<number, "starting" | "cancelling">();
  private destroyed = false;

  constructor(store: StateStore, callbacks: ToolbarCallbacks) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("header");
    this.element.className = "app-toolbar";
    this.render();
    this.syncIndexButton();
    this.syncWorkspaceSummaryButton();
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
          <button class="btn-icon workspace-summary-btn" title="Show workspace summary">Summary</button>
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
        <button class="btn-icon index-btn" title="Re-index workspace" aria-label="Index workspace">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path class="index-btn-icon" d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          <span class="index-btn-label">Index</span>
        </button>
      </div>
    `;

    this.workspaceSelect = this.element.querySelector(".workspace-select")!;
    this.summaryBtn = this.element.querySelector(".workspace-summary-btn")!;
    this.settingsBtn = this.element.querySelector(".ws-settings-btn")!;
    this.addWsBtn = this.element.querySelector(".add-workspace-btn")!;
    this.manageLibsBtn = this.element.querySelector(".manage-links-btn")!;
    this.statusBadge = this.element.querySelector(".index-status-badge")!;
    this.statusText = this.element.querySelector(".status-text")!;
    this.indexBtn = this.element.querySelector(".index-btn")!;
    this.indexBtnLabel = this.element.querySelector(".index-btn-label")!;
    this.indexBtnIcon = this.element.querySelector(".index-btn-icon")!;
    this.searchInput = this.element.querySelector(".search-input")!;
    this.searchModeSelect = this.element.querySelector(".search-mode-select")!;
  }

  private initEvents(): void {
    this.summaryBtn.addEventListener("click", () => this.callbacks.onShowSummary?.());

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

    this.indexBtn.addEventListener("click", () => void this.handleIndexButtonClick());

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
      if (changedKeys.includes("workspaceId")) {
        if (state.workspaceId !== null && this.workspaceSelect.value !== String(state.workspaceId)) {
          this.workspaceSelect.value = String(state.workspaceId);
        }
        this.syncIndexButton();
        this.syncWorkspaceSummaryButton();
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
        this.syncIndexButton();
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
      this.syncIndexButton();
    }
  }

  private async handleIndexButtonClick(): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || this.destroyed || this.pendingIndexActions.has(wsId)) return;

    const activeJob = this.activeJobs.get(wsId);
    if (activeJob && ["queued", "running"].includes(activeJob.status)) {
      this.pendingIndexActions.set(wsId, "cancelling");
      if (this.statusRequestWorkspaceId === wsId) {
        this.statusRequest?.abort();
        this.statusRequest = null;
        this.statusRequestWorkspaceId = null;
      }
      this.syncIndexButton();
      this.updateStatusDisplay("running", "Cancelling index...");

      try {
        await api.cancelJob(activeJob.id);
      } catch (err: any) {
        this.pendingIndexActions.delete(wsId);
        this.syncIndexButton();
        if (!this.destroyed && this.store.getState().workspaceId === wsId) {
          alert(`Index cancellation failed: ${err.message}`);
        }
        await this.fetchStatus();
        return;
      }

      if (!this.destroyed) await this.fetchStatus();
      return;
    }

    if (this.indexingWorkspaces.has(wsId) || !this.loadedStatusWorkspaces.has(wsId)) return;

    this.pendingIndexActions.set(wsId, "starting");
    if (this.statusRequestWorkspaceId === wsId) {
      this.statusRequest?.abort();
      this.statusRequest = null;
      this.statusRequestWorkspaceId = null;
    }
    this.syncIndexButton();
    this.updateStatusDisplay("running", "Starting index...");

    try {
      const job = await api.triggerIndexing(wsId, "incremental", false);
      if (this.destroyed) return;
      if (["queued", "running", "cancelling"].includes(job.status)) {
        this.activeJobs.set(wsId, job);
      } else {
        this.activeJobs.delete(wsId);
      }
      this.pendingIndexActions.delete(wsId);
      this.syncIndexButton();
      await this.fetchStatus();
    } catch (err: any) {
      this.pendingIndexActions.delete(wsId);
      this.syncIndexButton();
      if (!this.destroyed && this.store.getState().workspaceId === wsId) {
        alert(`Indexing trigger failed: ${err.message}`);
      }
      await this.fetchStatus();
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
    if (this.destroyed) return;
    if (!wsId) {
      this.statusRequest?.abort();
      this.statusRequest = null;
      this.statusRequestWorkspaceId = null;
      return;
    }
    if (this.statusRequest) {
      if (
        !this.statusRequest.signal.aborted &&
        this.statusRequestWorkspaceId === wsId
      ) {
        return;
      }
      this.statusRequest.abort();
    }

    const controller = new AbortController();
    this.statusRequest = controller;
    this.statusRequestWorkspaceId = wsId;

    try {
      const statusDto: WorkspaceStatusDto = await api.getWorkspaceStatus(wsId, controller.signal);
      if (
        controller.signal.aborted ||
        this.destroyed ||
        this.store.getState().workspaceId !== wsId
      ) {
        return;
      }
      this.loadedStatusWorkspaces.add(wsId);
      this.callbacks.onStatus?.(statusDto);
      const job = statusDto.latestJob ?? null;
      const jobIsActive = !!job && ["queued", "running", "cancelling"].includes(job.status);
      const workspaceIsIndexing = statusDto.status === "indexing" || statusDto.status === "running";
      if (workspaceIsIndexing) {
        this.indexingWorkspaces.add(wsId);
      } else {
        this.indexingWorkspaces.delete(wsId);
      }
      if (job && (jobIsActive || workspaceIsIndexing)) {
        this.activeJobs.set(wsId, job);
      } else {
        this.activeJobs.delete(wsId);
      }
      if (this.pendingIndexActions.get(wsId) === "cancelling" && !jobIsActive && !workspaceIsIndexing) {
        this.pendingIndexActions.delete(wsId);
      }

      let statusKind: "idle" | "running" | "failed" = "idle";
      let text = "Idle";

      const pendingAction = this.pendingIndexActions.get(wsId);
      if (pendingAction === "starting") {
        statusKind = "running";
        text = "Starting index...";
      } else if (pendingAction === "cancelling" || job?.status === "cancelling" ||
                 (workspaceIsIndexing && (job?.status === "cancelled" || job?.status === "canceled"))) {
        statusKind = "running";
        text = "Cancelling index...";
      } else if (workspaceIsIndexing || jobIsActive) {
        statusKind = "running";
        if (job) {
          text = `Indexing (${job.filesProcessed}/${job.filesTotal || "?"})`;
        } else {
          text = "Indexing...";
        }
      } else if (statusDto.status === "error" || job?.status === "failed") {
        statusKind = "failed";
        text = "Index failed";
      } else if (job?.status === "cancelled" || job?.status === "canceled") {
        text = "Index cancelled";
      } else if (statusDto.status === "ready") {
        text = `Indexed (${statusDto.fileCount} files)`;
      } else {
        text = statusDto.fileCount > 0 ? `Indexed (${statusDto.fileCount} files)` : "Idle";
      }

      this.updateStatusDisplay(statusKind, text);
      this.store.setIndexStatus(statusKind);
      this.syncIndexButton();
    } catch {
      // Quietly ignore intermittent status poll failure
    } finally {
      if (this.statusRequest === controller) {
        this.statusRequest = null;
        this.statusRequestWorkspaceId = null;
      }
    }
  }

  private syncIndexButton(): void {
    if (this.destroyed) return;
    const wsId = this.store.getState().workspaceId;
    let label = "Index";
    let title = "Re-index workspace";
    let ariaLabel = "Index workspace";
    let disabled = false;
    let iconPath = "M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67";

    if (!wsId) {
      disabled = true;
      title = "Select a workspace to index";
      ariaLabel = title;
    } else {
      const pending = this.pendingIndexActions.get(wsId);
      const activeJob = this.activeJobs.get(wsId);
      if (pending === "starting") {
        label = "Starting...";
        disabled = true;
      } else if (pending === "cancelling" || activeJob?.status === "cancelling") {
        label = "Cancelling...";
        disabled = true;
        title = "Cancelling indexing";
        ariaLabel = title;
        iconPath = "M6 6h12v12H6z";
      } else if (activeJob && ["queued", "running"].includes(activeJob.status)) {
        label = "Cancel";
        title = "Cancel indexing";
        ariaLabel = title;
        iconPath = "M6 6h12v12H6z";
      } else if (this.indexingWorkspaces.has(wsId)) {
        label = "Finishing...";
        disabled = true;
      } else if (!this.loadedStatusWorkspaces.has(wsId)) {
        label = "Checking...";
        disabled = true;
      }
    }

    this.indexBtnLabel.textContent = label;
    this.indexBtnIcon.setAttribute("d", iconPath);
    this.indexBtn.disabled = disabled;
    this.indexBtn.title = title;
    this.indexBtn.setAttribute("aria-label", ariaLabel);
  }

  private syncWorkspaceSummaryButton(): void {
    if (!this.summaryBtn) return;
    this.summaryBtn.disabled = this.store.getState().workspaceId === null;
  }

  private updateStatusDisplay(status: "idle" | "running" | "failed", text: string): void {
    this.statusBadge.className = `index-status-badge status-${status}`;
    this.statusText.textContent = text;
  }

  destroy(): void {
    this.destroyed = true;
    this.statusRequest?.abort();
    this.statusRequest = null;
    this.statusRequestWorkspaceId = null;
    if (this.statusPollTimer) {
      clearInterval(this.statusPollTimer);
      this.statusPollTimer = null;
    }
  }
}
