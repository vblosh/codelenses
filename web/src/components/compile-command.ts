import type { FileCompileCommandDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";

export interface CompileCommandCallbacks {
  onConfigureWorkspace?: () => void;
  onConfigureWorkspaceDefault?: () => void;
}

export class CompileCommandComponent {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: CompileCommandCallbacks;
  private container!: HTMLElement;
  private currentData: FileCompileCommandDto | null = null;
  private defineFilterQuery: string = "";

  constructor(store: StateStore, callbacks: CompileCommandCallbacks = {}) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "inspector-pane-view";
    this.element.id = "inspector-compile-command";
    this.render();
    this.initEvents();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="compile-command-container" style="flex: 1; overflow-y: auto; padding: 12px;">
        <div class="empty-state">
          <div class="empty-state-title">No compile command</div>
          <div class="empty-state-desc">Select a file to inspect its compilation flags and context.</div>
        </div>
      </div>
    `;

    this.container = this.element.querySelector(".compile-command-container")!;
  }

  private initEvents(): void {
    this.store.subscribe((state, changedKeys) => {
      if (changedKeys.includes("selectedFileId")) {
        this.loadCompileCommand(state.selectedFileId);
      }
    });
  }

  async loadCompileCommand(fileId: number | null): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || !fileId) {
      this.currentData = null;
      this.renderEmpty("No file selected", "Select a file from the explorer to view its build context.");
      return;
    }

    try {
      this.container.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">Loading compile command...</div>
        </div>
      `;

      const data = await api.getFileCompileCommand(wsId, fileId);
      this.setCompileCommand(data);
    } catch (err: any) {
      this.currentData = null;
      this.renderEmpty(
        "Compile command unavailable",
        err.message || "Failed to load compile command context."
      );
    }
  }

  setCompileCommand(data: FileCompileCommandDto | null): void {
    this.currentData = data;
    this.defineFilterQuery = "";
    if (!data || !data.hasCompileCommand || !data.compileCommand) {
      const dbPath = data?.databasePath;
      const desc = dbPath
        ? "This file is not specified in compile_commands.json. Default language heuristics are used."
        : "No compilation database found in this workspace. Default language heuristics are used.";
      this.renderEmpty("No compile command", desc, true);
      return;
    }

    this.renderContext(data);
  }

  getCompileCommand(): FileCompileCommandDto | null {
    return this.currentData;
  }

  private renderEmpty(title: string, desc: string, showConfigureBtn: boolean = false): void {
    this.container.innerHTML = `
      <div class="empty-state">
        <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" style="margin-bottom: 8px; opacity: 0.7;">
          <polyline points="4 17 10 11 4 5"></polyline>
          <line x1="12" y1="19" x2="20" y2="19"></line>
        </svg>
        <div class="empty-state-title">${title}</div>
        <div class="empty-state-desc">${desc}</div>
        ${
          showConfigureBtn
            ? `
            <div class="empty-actions" style="margin-top: 12px; display: flex; gap: 8px; justify-content: center; flex-wrap: wrap;">
              <button class="btn configure-cdb-btn">Configure compile_commands.json</button>
              <button class="btn configure-default-cmd-btn" style="background: var(--bg-tertiary);">Change Workspace Default Command</button>
            </div>
            `
            : ""
        }
      </div>
    `;

    if (showConfigureBtn) {
      const btnCdb = this.container.querySelector(".configure-cdb-btn");
      btnCdb?.addEventListener("click", () => {
        if (this.callbacks.onConfigureWorkspace) {
          this.callbacks.onConfigureWorkspace();
        }
      });
      const btnDefault = this.container.querySelector(".configure-default-cmd-btn");
      btnDefault?.addEventListener("click", () => {
        if (this.callbacks.onConfigureWorkspaceDefault) {
          this.callbacks.onConfigureWorkspaceDefault();
        } else if (this.callbacks.onConfigureWorkspace) {
          this.callbacks.onConfigureWorkspace();
        }
      });
    }
  }

  private renderContext(data: FileCompileCommandDto): void {
    const cmd = data.compileCommand!;
    const standard = cmd.languageStandard || "default";
    const compiler = cmd.arguments.length > 0 ? cmd.arguments[0] : "compiler";
    const dbSource = data.databasePath || "compile_commands.json";
    const isAuto = data.isAutoDetected ? "auto-detected" : "configured";
    const isWsDefault = !!data.isWorkspaceDefault;
    const sourceLabel = isWsDefault ? "workspace default" : isAuto;
    const sourceClass = isWsDefault ? "ws-default" : "file-entry";

    this.container.innerHTML = `
      <!-- Summary Card -->
      <div class="cc-card">
        <div class="cc-header">
          <div class="cc-title-group">
            <span class="badge cc-standard-badge">${standard.toUpperCase()}</span>
            <span class="cc-compiler-name">${escapeHtml(compiler)}</span>
          </div>
          <span class="badge cc-source-badge ${sourceClass}">${escapeHtml(sourceLabel)}</span>
        </div>

        <div class="cc-precedence-banner" style="margin-top: 8px; padding: 6px 10px; border-radius: 4px; font-size: 11px; display: flex; align-items: center; justify-content: space-between; gap: 8px; background: ${isWsDefault ? "rgba(59, 130, 246, 0.1)" : "rgba(16, 185, 129, 0.1)"}; border-left: 3px solid ${isWsDefault ? "#3b82f6" : "#10b981"};">
          <span>
            ${isWsDefault
              ? "Applied from <strong>workspace default</strong> (no entry in compile_commands.json)."
              : "Applied from <strong>individual file command</strong> in compile_commands.json (overrides workspace default)."}
          </span>
          <button class="btn btn-sm edit-ws-default-btn" style="padding: 2px 8px; font-size: 10px; white-space: nowrap;">Change Default</button>
        </div>

        <div class="cc-meta-row" style="margin-top: 8px;">
          <span class="cc-meta-label">Working Directory:</span>
          <span class="cc-meta-val" title="${escapeHtml(cmd.directory)}">${escapeHtml(cmd.directory)}</span>
        </div>
        ${
          cmd.output
            ? `<div class="cc-meta-row">
                <span class="cc-meta-label">Target Output:</span>
                <span class="cc-meta-val">${escapeHtml(cmd.output)}</span>
              </div>`
            : ""
        }
        ${
          !isWsDefault
            ? `<div class="cc-meta-row">
                <span class="cc-meta-label">Database:</span>
                <span class="cc-meta-val" title="${escapeHtml(dbSource)}">${escapeHtml(dbSource)}</span>
              </div>`
            : ""
        }
      </div>

      <!-- Macro Defines Section -->
      <div class="cc-section">
        <div class="cc-section-header">
          <span>Preprocessor Defines</span>
          <span class="count-pill cc-defines-count">${cmd.defines.length}</span>
        </div>

        ${
          cmd.defines.length > 0
            ? `
          <div class="cc-search-bar" style="margin-bottom: 8px;">
            <input type="text" class="search-input cc-define-filter" placeholder="Filter defines (-D / -U)..." style="width: 100%; font-size: 11px; padding: 4px 8px; border-radius: 4px; border: 1px solid var(--border-color); background: var(--bg-primary); color: var(--text-primary);" />
          </div>
          <div class="cc-defines-list"></div>
          `
            : `<div class="cc-empty-msg">No macro definitions specified in compile command.</div>`
        }
      </div>

      <!-- Include Directories Section -->
      <div class="cc-section">
        <div class="cc-section-header">
          <span>Include Directories</span>
          <span class="count-pill">${cmd.includeDirs.length}</span>
        </div>

        ${
          cmd.includeDirs.length > 0
            ? `
          <div class="cc-includes-list">
            ${cmd.includeDirs
              .map(
                (inc, idx) => `
              <div class="cc-include-item">
                <span class="cc-index">${idx + 1}.</span>
                <span class="cc-path" title="${escapeHtml(inc)}">${escapeHtml(inc)}</span>
              </div>
            `
              )
              .join("")}
          </div>
          `
            : `<div class="cc-empty-msg">No include directories (-I) specified in compile command.</div>`
        }
      </div>

      <!-- Raw Arguments Section -->
      <div class="cc-section">
        <div class="cc-section-header">
          <span>Command Arguments</span>
          <button class="btn-icon copy-cmd-btn" title="Copy raw command to clipboard">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect>
              <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path>
            </svg>
            <span class="copy-text">Copy</span>
          </button>
        </div>
        <pre class="cc-command-pre"><code>${escapeHtml(cmd.arguments.join(" "))}</code></pre>
      </div>
    `;

    // Render filtered defines
    if (cmd.defines.length > 0) {
      const filterInput = this.container.querySelector(".cc-define-filter") as HTMLInputElement | null;
      filterInput?.addEventListener("input", (e) => {
        this.defineFilterQuery = (e.target as HTMLInputElement).value.trim().toLowerCase();
        this.renderDefinesList(cmd.defines);
      });
      this.renderDefinesList(cmd.defines);
    }

    // Attach copy command event
    const copyBtn = this.container.querySelector(".copy-cmd-btn") as HTMLButtonElement | null;
    const copyText = copyBtn?.querySelector(".copy-text") as HTMLElement | null;
    copyBtn?.addEventListener("click", () => {
      const fullCmd = cmd.arguments.join(" ");
      navigator.clipboard?.writeText(fullCmd).then(() => {
        if (copyText) {
          const original = copyText.textContent;
          copyText.textContent = "Copied!";
          setTimeout(() => {
            copyText.textContent = original;
          }, 1500);
        }
      });
    });

    const editDefaultBtn = this.container.querySelector(".edit-ws-default-btn") as HTMLButtonElement | null;
    editDefaultBtn?.addEventListener("click", () => {
      if (this.callbacks.onConfigureWorkspaceDefault) {
        this.callbacks.onConfigureWorkspaceDefault();
      } else if (this.callbacks.onConfigureWorkspace) {
        this.callbacks.onConfigureWorkspace();
      }
    });
  }

  private renderDefinesList(defines: string[]): void {
    const listElem = this.container.querySelector(".cc-defines-list");
    if (!listElem) return;

    const filtered = defines.filter((d) =>
      this.defineFilterQuery ? d.toLowerCase().includes(this.defineFilterQuery) : true
    );

    if (filtered.length === 0) {
      listElem.innerHTML = `<div class="cc-empty-msg">No defines matching filter.</div>`;
      return;
    }

    listElem.innerHTML = filtered
      .map((def) => {
        if (def.startsWith("-U")) {
          const undefName = def.substring(2);
          return `
            <div class="cc-define-item undef">
              <span class="badge undef-tag">-U</span>
              <span class="cc-def-name">${escapeHtml(undefName)}</span>
              <span class="cc-def-hint">(undefined)</span>
            </div>
          `;
        }

        const eq = def.indexOf("=");
        const name = eq !== -1 ? def.substring(0, eq) : def;
        const val = eq !== -1 ? def.substring(eq + 1) : "1";

        return `
          <div class="cc-define-item">
            <span class="badge def-tag">-D</span>
            <span class="cc-def-name">${escapeHtml(name)}</span>
            <span class="cc-def-eq">=</span>
            <span class="cc-def-val">${escapeHtml(val)}</span>
          </div>
        `;
      })
      .join("");
  }
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
