import { IndexingSettingsForm } from "./indexing-settings";
import type { StateStore } from "../state";
import type { WorkspaceDto } from "../types";
import { api } from "../api";

export interface WorkspaceSettingsCallbacks {
  onSaved?: (workspaceId: number) => void;
  onDeleted?: (workspaceId: number) => void;
  onLinksChanged?: (workspaceId: number) => void;
}

export class WorkspaceSettingsModal {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: WorkspaceSettingsCallbacks;
  private indexingSettings = new IndexingSettingsForm();
  private currentWsId: number | null = null;
  private viewVersion = 0;

  private nameInput!: HTMLInputElement;
  private rootPathElem!: HTMLElement;
  private cdbInput!: HTMLInputElement;
  private cdbStatusElem!: HTMLElement;
  private defaultCmdInput!: HTMLInputElement;
  private defaultCmdPreviewElem!: HTMLElement;
  private linkedWorkspacesListElem!: HTMLElement;
  private linkWorkspaceBtn!: HTMLButtonElement;
  private deleteWsBtn!: HTMLButtonElement;
  private linkWorkspaceFormElem!: HTMLElement;
  private availableWorkspacesSelect!: HTMLSelectElement;
  private confirmAttachBtn!: HTMLButtonElement;
  private cancelAttachBtn!: HTMLButtonElement;
  private allWorkspaces: WorkspaceDto[] = [];
  private linkedWorkspaces: WorkspaceDto[] = [];
  private saveAndIndexBtn!: HTMLButtonElement;
  private saveBtn!: HTMLButtonElement;
  private cancelBtn!: HTMLButtonElement;
  private closeBtn!: HTMLButtonElement;

  constructor(store: StateStore, callbacks: WorkspaceSettingsCallbacks = {}) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "modal-backdrop workspace-settings-backdrop";
    this.element.style.display = "none";
    this.render();
    this.initEvents();
    document.body.appendChild(this.element);
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="modal workspace-settings-modal" role="dialog" aria-labelledby="ws-settings-title">
        <div class="modal-header">
          <div class="modal-title" id="ws-settings-title">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right: 6px;">
              <circle cx="12" cy="12" r="3"></circle>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path>
            </svg>
            <span>Workspace Settings & Build Context</span>
          </div>
          <button class="btn-icon close-settings-btn" title="Close (Esc)">✕</button>
        </div>

        <div class="modal-body">
          <div class="form-group">
            <label class="form-label">Root Directory</label>
            <div class="form-readonly-val ws-root-path">/workspace</div>
          </div>

          <div class="form-group">
            <label class="form-label" for="ws-name-input">Workspace Name</label>
            <input type="text" id="ws-name-input" class="form-input ws-name-input" placeholder="Workspace name" />
          </div>

          <div class="form-group">
            <label class="form-label" for="ws-cdb-input">Compilation Database Path (compile_commands.json)</label>
            <input type="text" id="ws-cdb-input" class="form-input ws-cdb-input" placeholder="e.g. compile_commands.json or build/compile_commands.json" />
            <div class="form-hint">Path can be relative to workspace root or absolute. Leave blank to auto-detect.</div>
            <div class="cdb-status-banner" style="margin-top: 8px;"></div>
          </div>

          <div class="form-group ws-default-cmd-group">
            <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 4px;">
              <label class="form-label" for="ws-default-cmd-input" style="margin-bottom: 0;">Workspace Default Compile Command</label>
              <span class="badge" style="font-size: 10px; background: rgba(59, 130, 246, 0.15); color: #60a5fa;">Default Fallback</span>
            </div>
            <input type="text" id="ws-default-cmd-input" class="form-input ws-default-cmd-input" placeholder="e.g. clang -Iinclude -DDEBUG=1 -std=c17" />
            <div class="form-hint">
              Applied to all files without an individual compile command in <code>compile_commands.json</code>.
              <br/>
              <strong>Precedence:</strong> Individual file compile commands in <code>compile_commands.json</code> take precedence.
            </div>
            <div class="ws-default-cmd-preview" style="margin-top: 8px; display: none; padding: 6px 10px; background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 4px; font-size: 11px;">
              <div class="preview-content" style="display: flex; gap: 8px; align-items: center; flex-wrap: wrap;"></div>
            </div>
          </div>

          <div class="form-group ws-links-group" style="margin-top: 16px; border-top: 1px solid var(--border-color); padding-top: 16px;">
            <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px;">
              <label class="form-label" style="margin-bottom: 0;">Linked workspaces</label>
              <div style="display: flex; gap: 6px;">
                <button type="button" class="btn btn-sm link-workspace-btn">+ Link workspace</button>
              </div>
            </div>
            <div class="form-hint" style="margin-bottom: 8px;">
              Search symbols and open declarations from directly linked workspaces. References show usages in the active workspace.
            </div>
            <div class="linked-workspaces-list" style="display: flex; flex-direction: column; gap: 6px;"></div>
            <div class="link-workspace-form" style="display: none; margin-top: 8px; padding: 10px; background: var(--bg-secondary); border-radius: 4px; border: 1px solid var(--border-color);">
              <div style="font-weight: 600; margin-bottom: 6px; font-size: 12px;">Select workspace to link</div>
              <div style="display: flex; gap: 8px; align-items: center;">
                <select class="form-select available-workspaces-select" style="flex: 1; padding: 4px 8px; font-size: 12px;"></select>
                <button type="button" class="btn btn-primary btn-sm confirm-attach-btn">Link</button>
                <button type="button" class="btn btn-sm cancel-attach-btn">Cancel</button>
              </div>
            </div>
          </div>

          <!-- Danger Zone: Delete Workspace -->
          <div class="danger-zone" style="margin-top: 20px; border-top: 1px solid rgba(239, 68, 68, 0.3); padding-top: 14px;">
            <div style="display: flex; align-items: center; justify-content: space-between;">
              <div>
                <div class="danger-zone-title" style="color: #ef4444; font-weight: 600; font-size: 12px; margin-bottom: 2px;">Delete Workspace</div>
                <div class="form-hint" style="color: var(--text-muted); font-size: 11px;">Permanently remove this workspace and its index from CodeLenses.</div>
              </div>
              <button type="button" class="btn btn-sm btn-danger delete-ws-btn">Delete Workspace</button>
            </div>
          </div>
        </div>

        <div class="modal-footer">
          <button class="btn cancel-settings-btn">Cancel</button>
          <button class="btn save-settings-btn">Save</button>
          <button class="btn btn-primary save-index-settings-btn">Save & Re-index</button>
        </div>
      </div>
    `;

    this.element.querySelector(".ws-links-group")!.before(this.indexingSettings.element);
    this.rootPathElem = this.element.querySelector(".ws-root-path")!;
    this.nameInput = this.element.querySelector(".ws-name-input")!;
    this.cdbInput = this.element.querySelector(".ws-cdb-input")!;
    this.cdbStatusElem = this.element.querySelector(".cdb-status-banner")!;
    this.defaultCmdInput = this.element.querySelector(".ws-default-cmd-input")!;
    this.defaultCmdPreviewElem = this.element.querySelector(".ws-default-cmd-preview")!;
    this.linkedWorkspacesListElem = this.element.querySelector(".linked-workspaces-list")!;
    this.linkWorkspaceBtn = this.element.querySelector(".link-workspace-btn")!;
    this.deleteWsBtn = this.element.querySelector(".delete-ws-btn")!;
    this.linkWorkspaceFormElem = this.element.querySelector(".link-workspace-form")!;
    this.availableWorkspacesSelect = this.element.querySelector(".available-workspaces-select")!;
    this.confirmAttachBtn = this.element.querySelector(".confirm-attach-btn")!;
    this.cancelAttachBtn = this.element.querySelector(".cancel-attach-btn")!;
    this.saveAndIndexBtn = this.element.querySelector(".save-index-settings-btn")!;
    this.saveBtn = this.element.querySelector(".save-settings-btn")!;
    this.cancelBtn = this.element.querySelector(".cancel-settings-btn")!;
    this.closeBtn = this.element.querySelector(".close-settings-btn")!;
  }

  private initEvents(): void {
    this.closeBtn.addEventListener("click", () => this.close());
    this.cancelBtn.addEventListener("click", () => this.close());

    this.element.addEventListener("click", (e) => {
      if (e.target === this.element) {
        this.close();
      }
    });

    window.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && this.element.style.display !== "none") {
        this.close();
      }
    });

    this.defaultCmdInput.addEventListener("input", () => {
      this.updateDefaultCmdPreview(this.defaultCmdInput.value);
    });

    this.linkWorkspaceBtn.addEventListener("click", () => {
      this.showAttachForm();
    });

    this.deleteWsBtn.addEventListener("click", () => {
      this.handleDeleteWorkspace();
    });

    this.cancelAttachBtn.addEventListener("click", () => {
      this.linkWorkspaceFormElem.style.display = "none";
    });

    this.confirmAttachBtn.addEventListener("click", () => {
      this.handleLinkWorkspace();
    });

    this.saveBtn.addEventListener("click", () => this.handleSave(false));
    this.saveAndIndexBtn.addEventListener("click", () => this.handleSave(true));
  }

  async open(workspaceId?: number, focusDefaultCmd?: boolean): Promise<void> {
    const wsId = workspaceId ?? this.store.getState().workspaceId;
    if (!wsId) return;

    const viewVersion = ++this.viewVersion;
    this.currentWsId = wsId;
    this.element.style.display = "flex";
    this.setActionButtonsDisabled(true);

    try {
      this.cdbStatusElem.innerHTML = `<span class="badge" style="opacity: 0.7;">Checking compilation database...</span>`;
      const [ws, cdbInfo] = await Promise.all([
        api.getWorkspace(wsId),
        api.getWorkspaceCompileCommands(wsId).catch(() => null),
      ]);

      if (!this.isCurrentView(wsId, viewVersion)) return;
      this.indexingSettings.setValue(ws.indexingSettings);
      this.rootPathElem.textContent = ws.rootPath;
      this.nameInput.value = ws.name || "";
      this.cdbInput.value = ws.compileCommandsPath || "";
      this.defaultCmdInput.value = ws.defaultCompileCommand || "";

      this.renderCdbStatus(cdbInfo);
      this.updateDefaultCmdPreview(this.defaultCmdInput.value);
      await this.loadLinks(wsId, viewVersion);
      if (!this.isCurrentView(wsId, viewVersion)) return;
      this.setActionButtonsDisabled(false);

      if (focusDefaultCmd) {
        setTimeout(() => {
          if (!this.isCurrentView(wsId, viewVersion)) return;
          this.defaultCmdInput.focus();
          this.defaultCmdInput.select();
        }, 50);
      }
    } catch (err: any) {
      if (this.isCurrentView(wsId, viewVersion)) {
        this.cdbStatusElem.innerHTML = `<span class="badge badge-error">Error loading settings: ${escapeHtml(err.message || String(err))}</span>`;
      }
    }
  }

  close(): void {
    this.viewVersion++;
    this.currentWsId = null;
    this.element.style.display = "none";
    this.linkWorkspaceFormElem.style.display = "none";
  }

  private isCurrentView(workspaceId: number, viewVersion: number): boolean {
    return this.currentWsId === workspaceId &&
      this.viewVersion === viewVersion &&
      this.element.style.display !== "none";
  }

  private setActionButtonsDisabled(disabled: boolean): void {
    this.saveBtn.disabled = disabled;
    this.saveAndIndexBtn.disabled = disabled;
    this.deleteWsBtn.disabled = disabled;
    this.linkWorkspaceBtn.disabled = disabled;
    this.confirmAttachBtn.disabled = disabled;
  }

  private async loadLinks(wsId: number, viewVersion: number): Promise<void> {
    if (!this.isCurrentView(wsId, viewVersion)) return;
    try {
      this.linkedWorkspacesListElem.innerHTML = `<span style="font-size: 11px; opacity: 0.7;">Loading linked workspaces...</span>`;
      const [attachedRes, allRes] = await Promise.all([
        api.getWorkspaceLinks(wsId),
        api.getWorkspaces(),
      ]);
      if (!this.isCurrentView(wsId, viewVersion)) return;
      this.linkedWorkspaces = attachedRes.workspaces || [];
      this.allWorkspaces = allRes.workspaces || [];
      this.renderLinkedWorkspaces();
    } catch (err: any) {
      if (this.isCurrentView(wsId, viewVersion)) {
        this.linkedWorkspacesListElem.innerHTML = `<span class="badge badge-error">Failed to load links: ${escapeHtml(err.message || String(err))}</span>`;
      }
    }
  }

  private renderLinkedWorkspaces(): void {
    if (!this.linkedWorkspaces.length) {
      this.linkedWorkspacesListElem.innerHTML = `
        <div style="font-size: 12px; color: var(--text-secondary); font-style: italic;">
          No workspaces linked.
        </div>
      `;
      return;
    }

    this.linkedWorkspacesListElem.innerHTML = this.linkedWorkspaces
      .map((lib) => {
        return `
          <div class="linked-workspace-item" style="display: flex; align-items: center; justify-content: space-between; padding: 6px 10px; background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 4px;">
            <div style="display: flex; align-items: center; gap: 8px;">
              <strong style="font-size: 12px;">${escapeHtml(lib.name)}</strong>
              <span>${escapeHtml(lib.rootPath)}</span>
            </div>
            <button type="button" class="btn btn-sm unlink-workspace-btn" data-workspace-id="${lib.id}" style="color: #ef4444; border-color: rgba(239, 68, 68, 0.3);" title="Unlink workspace">Unlink</button>
          </div>
        `;
      })
      .join("");

    this.linkedWorkspacesListElem.querySelectorAll(".unlink-workspace-btn").forEach((btn) => {
      btn.addEventListener("click", async (e) => {
        const libId = Number((e.currentTarget as HTMLElement).getAttribute("data-workspace-id"));
        const workspaceId = this.currentWsId;
        const viewVersion = this.viewVersion;
        if (!workspaceId || !libId || !this.isCurrentView(workspaceId, viewVersion)) return;
        const button = btn as HTMLButtonElement;
        try {
          button.disabled = true;
          await api.unlinkWorkspace(workspaceId, libId);
          this.callbacks.onLinksChanged?.(workspaceId);
          await this.loadLinks(workspaceId, viewVersion);
        } catch (err: any) {
          if (this.isCurrentView(workspaceId, viewVersion)) {
            alert(`Failed to unlink workspace: ${err.message || String(err)}`);
          }
        } finally {
          button.disabled = false;
        }
      });
    });
  }

  private showAttachForm(): void {
    const attachedIds = new Set(this.linkedWorkspaces.map((l) => l.id));
    const available = this.allWorkspaces.filter((l) => l.id !== this.currentWsId && !attachedIds.has(l.id));

    if (!available.length) {
      alert("No additional workspaces available. Create a workspace first.");
      return;
    }
    this.availableWorkspacesSelect.innerHTML = available.map((w) =>
      `<option value="${w.id}">${escapeHtml(w.name)} — ${escapeHtml(w.rootPath)}</option>`).join("");

    this.linkWorkspaceFormElem.style.display = "block";
  }

  private async handleLinkWorkspace(): Promise<void> {
    const workspaceId = this.currentWsId;
    const viewVersion = this.viewVersion;
    if (!workspaceId || !this.isCurrentView(workspaceId, viewVersion)) return;
    const profileId = Number(this.availableWorkspacesSelect.value);
    if (!profileId) return;

    try {
      this.confirmAttachBtn.disabled = true;
      await api.linkWorkspace(workspaceId, profileId);
      this.callbacks.onLinksChanged?.(workspaceId);
      if (this.isCurrentView(workspaceId, viewVersion)) {
        this.linkWorkspaceFormElem.style.display = "none";
      }
      await this.loadLinks(workspaceId, viewVersion);
    } catch (err: any) {
      if (this.isCurrentView(workspaceId, viewVersion)) {
        alert(`Failed to link workspace: ${err.message || String(err)}`);
      }
    } finally {
      if (this.isCurrentView(workspaceId, viewVersion)) this.confirmAttachBtn.disabled = false;
    }
  }

  private updateDefaultCmdPreview(cmdStr: string): void {
    const previewBox = this.defaultCmdPreviewElem;
    if (!previewBox) return;
    const content = previewBox.querySelector(".preview-content") as HTMLElement;
    if (!content) return;

    const trimmed = cmdStr.trim();
    if (!trimmed) {
      previewBox.style.display = "none";
      content.innerHTML = "";
      return;
    }

    const parsed = parseCommandPreview(trimmed);
    previewBox.style.display = "block";

    const stdTag = parsed.standard
      ? `<span class="badge" style="background: rgba(16, 185, 129, 0.2); color: #34d399; font-weight: 600;">${escapeHtml(parsed.standard.toUpperCase())}</span>`
      : `<span class="badge" style="background: rgba(148, 163, 184, 0.2); color: var(--text-secondary);">DEFAULT STD</span>`;

    const compTag = parsed.compiler
      ? `<span style="font-weight: 500; color: var(--text-primary);">${escapeHtml(parsed.compiler)}</span>`
      : "";

    const defsCount = `${parsed.defines.length} define${parsed.defines.length === 1 ? "" : "s"}`;
    const incsCount = `${parsed.includes.length} include dir${parsed.includes.length === 1 ? "" : "s"}`;

    content.innerHTML = `
      ${compTag}
      ${stdTag}
      <span style="color: var(--text-secondary);">(${defsCount}, ${incsCount})</span>
    `;
  }

  private renderCdbStatus(cdbInfo: any): void {
    if (!cdbInfo) {
      this.cdbStatusElem.innerHTML = `<span class="badge" style="background: var(--bg-tertiary);">Status unavailable</span>`;
      return;
    }

    if (cdbInfo.exists) {
      const mode = cdbInfo.isAutoDetected ? "Auto-detected" : "Configured";
      const count = `${cdbInfo.totalCommands} command${cdbInfo.totalCommands === 1 ? "" : "s"}`;
      this.cdbStatusElem.innerHTML = `
        <div class="status-pill status-found">
          <span class="status-dot green"></span>
          <span>${mode}: <strong>${escapeHtml(cdbInfo.effectivePath || "")}</strong> (${count})</span>
        </div>
      `;
    } else if (cdbInfo.configuredPath) {
      this.cdbStatusElem.innerHTML = `
        <div class="status-pill status-missing">
          <span class="status-dot red"></span>
          <span>File not found at configured path: <em>${escapeHtml(cdbInfo.configuredPath)}</em></span>
        </div>
      `;
    } else {
      this.cdbStatusElem.innerHTML = `
        <div class="status-pill status-none">
          <span class="status-dot gray"></span>
          <span>No compile_commands.json found in default locations.</span>
        </div>
      `;
    }
  }

  private async handleSave(reindex: boolean): Promise<void> {
    const workspaceId = this.currentWsId;
    const viewVersion = this.viewVersion;
    if (!workspaceId || !this.isCurrentView(workspaceId, viewVersion)) return;

    const name = this.nameInput.value.trim();
    const cdbPath = this.cdbInput.value.trim() || null;
    const defaultCmd = this.defaultCmdInput.value.trim() || null;

    try {
      this.saveBtn.disabled = true;
      this.saveAndIndexBtn.disabled = true;

      await api.updateWorkspace(workspaceId, {
        name: name || undefined,
        indexingSettings: this.indexingSettings.getValue(),
        compileCommandsPath: cdbPath,
        defaultCompileCommand: defaultCmd,
      });

      if (reindex) {
        await api.triggerIndexing(workspaceId, "incremental", false);
      }

      if (this.isCurrentView(workspaceId, viewVersion)) this.close();
      this.callbacks.onSaved?.(workspaceId);
    } catch (err: any) {
      if (this.isCurrentView(workspaceId, viewVersion)) alert(`Failed to save settings: ${err.message}`);
    } finally {
      if (this.isCurrentView(workspaceId, viewVersion)) this.setActionButtonsDisabled(false);
    }
  }

  async refreshLinks(): Promise<void> {
    const workspaceId = this.currentWsId;
    if (workspaceId) {
      await this.loadLinks(workspaceId, this.viewVersion);
    }
  }

  private async handleDeleteWorkspace(): Promise<void> {
    const workspaceId = this.currentWsId;
    const viewVersion = this.viewVersion;
    if (!workspaceId || !this.isCurrentView(workspaceId, viewVersion)) return;
    const wsName = this.nameInput.value || this.rootPathElem.textContent || `Workspace #${workspaceId}`;
    if (
      !confirm(
        `Are you sure you want to delete workspace "${wsName}"? All indexed files, symbols, and references will be permanently removed.`
      )
    ) {
      return;
    }
    if (!this.isCurrentView(workspaceId, viewVersion)) return;

    try {
      this.deleteWsBtn.disabled = true;
      await api.deleteWorkspace(workspaceId);
      if (this.isCurrentView(workspaceId, viewVersion)) this.close();
      this.callbacks.onDeleted?.(workspaceId);
    } catch (err: any) {
      if (this.isCurrentView(workspaceId, viewVersion)) {
        alert(`Failed to delete workspace: ${err.message || String(err)}`);
      }
    } finally {
      if (this.isCurrentView(workspaceId, viewVersion)) this.deleteWsBtn.disabled = false;
    }
  }
}

export class WorkspaceCompileCommandsModal extends WorkspaceSettingsModal {
  constructor(store: StateStore, callbacks: WorkspaceSettingsCallbacks = {}) {
    super(store, callbacks);
  }

  async open(workspaceId?: number): Promise<void> {
    await super.open(workspaceId, true);
  }
}

export function parseCommandPreview(cmdStr: string): {
  standard?: string;
  defines: string[];
  includes: string[];
  compiler?: string;
} {
  const tokens = cmdStr.trim().split(/\s+/).filter(Boolean);
  let standard: string | undefined;
  const defines: string[] = [];
  const includes: string[] = [];
  let compiler: string | undefined = tokens.length > 0 && !tokens[0].startsWith("-") ? tokens[0] : undefined;

  for (let i = 0; i < tokens.length; ++i) {
    const t = tokens[i];
    if (t.startsWith("-std=")) {
      standard = t.substring(5);
    } else if (t.startsWith("--std=")) {
      standard = t.substring(6);
    } else if (t === "-D" && i + 1 < tokens.length) {
      defines.push(tokens[++i]);
    } else if (t.startsWith("-D") && t.length > 2) {
      defines.push(t.substring(2));
    } else if (t === "-I" && i + 1 < tokens.length) {
      includes.push(tokens[++i]);
    } else if (t.startsWith("-I") && t.length > 2) {
      includes.push(t.substring(2));
    }
  }

  return { standard, defines, includes, compiler };
}

function escapeHtml(text: string): string {
  const map: Record<string, string> = {
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#039;",
  };
  return text.replace(/[&<>"']/g, (m) => map[m]);
}
