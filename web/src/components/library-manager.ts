import type { StateStore } from "../state";
import type { LibraryDto, CreateLibraryRequest } from "../types";
import { api } from "../api";

export interface LibraryManagerCallbacks {
  onLibrariesChanged?: () => void;
}

export class LibraryManagerModal {
  private element: HTMLElement;
  private store: StateStore;
  private callbacks: LibraryManagerCallbacks;
  private activeWsId: number | null = null;

  private allLibraries: LibraryDto[] = [];
  private attachedLibIds: Set<number> = new Set();

  private bannerElem!: HTMLElement;
  private listContainerElem!: HTMLElement;
  private toggleCreateBtn!: HTMLButtonElement;
  private createFormElem!: HTMLElement;

  // Form inputs
  private libNameInput!: HTMLInputElement;
  private libLangSelect!: HTMLSelectElement;
  private libRootsInput!: HTMLTextAreaElement;
  private libStdInput!: HTMLInputElement;
  private libProviderInput!: HTMLInputElement;
  private libVersionInput!: HTMLInputElement;
  private libDefinesInput!: HTMLInputElement;
  private libTargetFrameworkInput!: HTMLInputElement;
  private submitCreateBtn!: HTMLButtonElement;
  private submitCreateIndexBtn!: HTMLButtonElement;
  private cancelCreateBtn!: HTMLButtonElement;
  private closeBtn!: HTMLButtonElement;

  constructor(store: StateStore, callbacks: LibraryManagerCallbacks = {}) {
    this.store = store;
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "modal-backdrop library-manager-backdrop";
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
      <div class="modal library-manager-modal" style="width: 680px; max-width: 95vw; max-height: 90vh; display: flex; flex-direction: column;" role="dialog" aria-labelledby="lib-manager-title">
        <div class="modal-header">
          <div class="modal-title" id="lib-manager-title">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right: 6px;">
              <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
              <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
            </svg>
            <span>Libraries & Toolchain SDKs</span>
          </div>
          <button class="btn-icon close-lib-manager-btn" title="Close (Esc)">✕</button>
        </div>

        <div class="modal-body" style="overflow-y: auto; flex: 1;">
          <div class="alert-banner lib-manager-banner" style="display: none;"></div>

          <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px;">
            <div class="form-hint" style="margin: 0; font-size: 12px;">
              Configure local C/C++ headers or C#/.NET source and declaration trees. C# profiles do not restore NuGet packages or evaluate MSBuild projects.
            </div>
            <button type="button" class="btn btn-sm btn-primary toggle-create-lib-btn">+ Add Library Profile</button>
          </div>

          <!-- Create Library Collapsible Form -->
          <div class="create-lib-form" style="display: none; margin-bottom: 16px; padding: 14px; background: var(--bg-secondary); border-radius: 6px; border: 1px solid var(--border-color);">
            <div style="font-weight: 600; font-size: 13px; margin-bottom: 12px; display: flex; align-items: center; justify-content: space-between;">
              <span>Register New Standard Library Profile</span>
              <span class="badge" style="background: rgba(59, 130, 246, 0.15); color: #60a5fa;">C / C++ / C#</span>
            </div>

            <div class="form-row" style="margin-bottom: 8px;">
              <div class="form-group" style="flex: 2;">
                <label class="form-label" for="new-lib-name">Profile Name <span style="color: var(--error);">*</span></label>
                <input type="text" id="new-lib-name" class="form-input new-lib-name" placeholder="e.g. GCC 12 libstdc++ or glibc 2.35" required />
              </div>
              <div class="form-group" style="flex: 1;">
                <label class="form-label" for="new-lib-lang">Language <span style="color: var(--error);">*</span></label>
                <select id="new-lib-lang" class="form-select new-lib-lang">
                  <option value="cpp">C++ (cpp)</option>
                  <option value="c">C (c)</option>
                  <option value="csharp">C# (csharp)</option>
                </select>
              </div>
            </div>

            <div class="form-row" style="margin-bottom: 8px;">
              <div class="form-group" style="flex: 2;">
                <label class="form-label" for="new-lib-roots">Local Source Root Directories <span style="color: var(--error);">*</span></label>
                <textarea id="new-lib-roots" class="form-input new-lib-roots" rows="2" placeholder="One or more paths separated by commas or newlines" required></textarea>
                <div class="form-hint">C# profiles index local .cs source and declaration files from every root.</div>
              </div>
              <div class="form-group language-standard-group" style="flex: 1;">
                <label class="form-label" for="new-lib-std">Language Standard</label>
                <input type="text" id="new-lib-std" class="form-input new-lib-std" placeholder="e.g. c++20, c17" />
              </div>
            </div>

            <div class="form-group target-framework-group" style="display: none; margin-bottom: 8px;">
              <label class="form-label" for="new-lib-target-framework">Target Framework Moniker (TFM) <span style="color: var(--error);">*</span></label>
              <input type="text" id="new-lib-target-framework" class="form-input new-lib-target-framework" placeholder="e.g. net8.0" />
              <div class="form-hint">Identifies the profile; CodeLenses does not apply a .NET compatibility matrix.</div>
            </div>

            <div class="form-row" style="margin-bottom: 8px;">
              <div class="form-group">
                <label class="form-label" for="new-lib-provider">Provider</label>
                <input type="text" id="new-lib-provider" class="form-input new-lib-provider" placeholder="e.g. gcc, clang, glibc, custom" />
              </div>
              <div class="form-group">
                <label class="form-label" for="new-lib-version">SDK / Version</label>
                <input type="text" id="new-lib-version" class="form-input new-lib-version" placeholder="e.g. 12.2.0" />
              </div>
            </div>

            <div class="form-group" style="margin-bottom: 12px;">
              <label class="form-label" for="new-lib-defines">Preprocessor Defines (optional)</label>
              <input type="text" id="new-lib-defines" class="form-input new-lib-defines" placeholder="e.g. _GNU_SOURCE=1, __linux__=1" />
              <div class="form-hint">Comma-separated macros passed during parsing.</div>
            </div>

            <div style="display: flex; justify-content: flex-end; gap: 8px;">
              <button type="button" class="btn cancel-create-lib-btn">Cancel</button>
              <button type="button" class="btn submit-create-lib-btn">Create Profile</button>
              <button type="button" class="btn btn-primary submit-create-index-lib-btn">Create & Index Now</button>
            </div>
          </div>

          <!-- Installed Profiles List -->
          <div>
            <div style="font-weight: 600; font-size: 12px; margin-bottom: 8px; color: var(--text-secondary); text-transform: uppercase; letter-spacing: 0.05em;">
              Registered Profiles
            </div>
            <div class="lib-profiles-list" style="display: flex; flex-direction: column; gap: 8px;"></div>
          </div>
        </div>

        <div class="modal-footer">
          <button type="button" class="btn close-modal-footer-btn">Close</button>
        </div>
      </div>
    `;

    this.bannerElem = this.element.querySelector(".lib-manager-banner")!;
    this.listContainerElem = this.element.querySelector(".lib-profiles-list")!;
    this.toggleCreateBtn = this.element.querySelector(".toggle-create-lib-btn")!;
    this.createFormElem = this.element.querySelector(".create-lib-form")!;

    this.libNameInput = this.element.querySelector(".new-lib-name")!;
    this.libLangSelect = this.element.querySelector(".new-lib-lang")!;
    this.libRootsInput = this.element.querySelector(".new-lib-roots")!;
    this.libStdInput = this.element.querySelector(".new-lib-std")!;
    this.libProviderInput = this.element.querySelector(".new-lib-provider")!;
    this.libVersionInput = this.element.querySelector(".new-lib-version")!;
    this.libDefinesInput = this.element.querySelector(".new-lib-defines")!;
    this.libTargetFrameworkInput = this.element.querySelector(".new-lib-target-framework")!;
    this.submitCreateBtn = this.element.querySelector(".submit-create-lib-btn")!;
    this.submitCreateIndexBtn = this.element.querySelector(".submit-create-index-lib-btn")!;
    this.cancelCreateBtn = this.element.querySelector(".cancel-create-lib-btn")!;
    this.closeBtn = this.element.querySelector(".close-lib-manager-btn")!;
  }

  private initEvents(): void {
    this.closeBtn.addEventListener("click", () => this.close());
    const footerClose = this.element.querySelector(".close-modal-footer-btn");
    footerClose?.addEventListener("click", () => this.close());

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

    this.toggleCreateBtn.addEventListener("click", () => {
      const isHidden = this.createFormElem.style.display === "none";
      this.createFormElem.style.display = isHidden ? "block" : "none";
      if (isHidden) {
        this.libNameInput.focus();
      }
    });

    this.cancelCreateBtn.addEventListener("click", () => {
      this.createFormElem.style.display = "none";
      this.resetForm();
    });

    this.submitCreateBtn.addEventListener("click", () => this.handleCreate(false));
    this.submitCreateIndexBtn.addEventListener("click", () => this.handleCreate(true));
    this.libLangSelect.addEventListener("change", () => this.updateLanguageFields());
  }

  async open(workspaceId?: number): Promise<void> {
    this.activeWsId = workspaceId ?? this.store.getState().workspaceId;
    this.element.style.display = "flex";
    this.createFormElem.style.display = "none";
    this.hideBanner();
    await this.loadLibraries();
  }

  close(): void {
    this.element.style.display = "none";
    this.createFormElem.style.display = "none";
    this.resetForm();
  }

  private resetForm(): void {
    this.libNameInput.value = "";
    this.libLangSelect.value = "cpp";
    this.libRootsInput.value = "";
    this.libStdInput.value = "";
    this.libProviderInput.value = "";
    this.libVersionInput.value = "";
    this.libDefinesInput.value = "";
    this.libTargetFrameworkInput.value = "";
    this.updateLanguageFields();
    this.hideBanner();
  }

  private showBanner(msg: string, type: "error" | "success" = "error"): void {
    this.bannerElem.className = `alert-banner ${type} lib-manager-banner`;
    this.bannerElem.textContent = msg;
    this.bannerElem.style.display = "flex";
  }

  private hideBanner(): void {
    this.bannerElem.style.display = "none";
    this.bannerElem.textContent = "";
  }

  async loadLibraries(): Promise<void> {
    try {
      this.listContainerElem.innerHTML = `<span style="font-size: 11px; opacity: 0.7;">Loading library profiles...</span>`;

      const [allRes, attachedRes] = await Promise.all([
        api.getLibraries(),
        this.activeWsId ? api.getWorkspaceLibraries(this.activeWsId).catch(() => null) : null,
      ]);

      this.allLibraries = allRes.libraries || [];
      this.attachedLibIds.clear();
      if (attachedRes?.libraries) {
        for (const l of attachedRes.libraries) {
          this.attachedLibIds.add(l.id);
        }
      }

      this.renderList();
    } catch (err: any) {
      this.showBanner(`Failed to load libraries: ${err.message || String(err)}`, "error");
    }
  }

  private renderList(): void {
    if (!this.allLibraries.length) {
      this.listContainerElem.innerHTML = `
        <div style="font-size: 12px; color: var(--text-secondary); font-style: italic; padding: 12px 0;">
          No library profiles registered yet. Click "+ Add Library Profile" above to create one.
        </div>
      `;
      return;
    }

    this.listContainerElem.innerHTML = this.allLibraries
      .map((lib) => {
        const langBadge = `<span class="badge" style="background: rgba(59, 130, 246, 0.15); color: #60a5fa; font-size: 10px;">${escapeHtml(lib.language.toUpperCase())}</span>`;
        const stdBadge = lib.languageStandard
          ? `<span class="badge" style="background: rgba(16, 185, 129, 0.15); color: #34d399; font-size: 10px;">${escapeHtml(lib.languageStandard)}</span>`
          : "";
        const frameworkBadge = lib.targetFramework
          ? `<span class="badge" style="background: rgba(16, 185, 129, 0.15); color: #34d399; font-size: 10px;">${escapeHtml(lib.targetFramework)}</span>`
          : "";
        const providerBadge = lib.provider
          ? `<span class="badge" style="background: rgba(148, 163, 184, 0.15); color: var(--text-secondary); font-size: 10px;">${escapeHtml(lib.provider)}</span>`
          : "";
        const isAttached = this.attachedLibIds.has(lib.id);

        let wsActionBtn = "";
        if (this.activeWsId) {
          if (isAttached) {
            wsActionBtn = `<button type="button" class="btn btn-sm detach-from-ws-btn" data-lib-id="${lib.id}" style="color: #f87171; border-color: rgba(239, 68, 68, 0.3);" title="Detach from active workspace">Detach</button>`;
          } else {
            wsActionBtn = `<button type="button" class="btn btn-sm btn-primary attach-to-ws-btn" data-lib-id="${lib.id}" title="Attach to active workspace">+ Attach</button>`;
          }
        }

        const rootsStr = lib.sourceRoots?.join(", ") || "No roots configured";

        return `
          <div class="lib-card" style="padding: 10px 12px; background: var(--bg-secondary); border: 1px solid var(--border-color); border-radius: 6px; display: flex; flex-direction: column; gap: 6px;">
            <div style="display: flex; align-items: center; justify-content: space-between;">
              <div style="display: flex; align-items: center; gap: 8px;">
                <strong style="font-size: 13px;">${escapeHtml(lib.name)}</strong>
                ${langBadge}
                ${stdBadge}
                ${frameworkBadge}
                ${providerBadge}
                ${isAttached ? `<span class="badge" style="background: rgba(59, 130, 246, 0.2); color: #93c5fd; font-size: 10px;">Attached to Workspace</span>` : ""}
              </div>
              <div style="display: flex; align-items: center; gap: 6px;">
                ${wsActionBtn}
                <button type="button" class="btn btn-sm index-lib-btn" data-lib-id="${lib.id}" title="Index this library">Index</button>
                <button type="button" class="btn btn-sm btn-danger delete-lib-profile-btn" data-lib-id="${lib.id}" title="Delete library profile permanently">Delete</button>
              </div>
            </div>
            <div style="font-size: 11px; color: var(--text-muted); font-family: var(--font-mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">
              Roots: ${escapeHtml(rootsStr)}
            </div>
          </div>
        `;
      })
      .join("");

    // Wire action buttons
    this.listContainerElem.querySelectorAll(".delete-lib-profile-btn").forEach((btn) => {
      btn.addEventListener("click", async (e) => {
        const libId = Number((e.currentTarget as HTMLElement).getAttribute("data-lib-id"));
        if (!libId) return;
        const lib = this.allLibraries.find((l) => l.id === libId);
        const name = lib ? lib.name : `Profile #${libId}`;
        if (!confirm(`Are you sure you want to permanently delete library profile "${name}"? This will remove all its indexed symbols and workspace associations.`)) {
          return;
        }

        const button = btn as HTMLButtonElement;
        try {
          button.disabled = true;
          await api.deleteLibrary(libId);
          this.showBanner(`Library profile "${name}" deleted.`, "success");
          await this.loadLibraries();
          this.callbacks.onLibrariesChanged?.();
        } catch (err: any) {
          this.showBanner(`Failed to delete library profile: ${err.message || String(err)}`, "error");
        } finally {
          button.disabled = false;
        }
      });
    });

    this.listContainerElem.querySelectorAll(".attach-to-ws-btn").forEach((btn) => {
      btn.addEventListener("click", async (e) => {
        const libId = Number((e.currentTarget as HTMLElement).getAttribute("data-lib-id"));
        if (!this.activeWsId || !libId) return;
        const button = btn as HTMLButtonElement;
        try {
          button.disabled = true;
          await api.attachLibrary(this.activeWsId, libId);
          await this.loadLibraries();
          this.callbacks.onLibrariesChanged?.();
          api.triggerIndexing(this.activeWsId, "incremental", false).catch(() => {});
        } catch (err: any) {
          this.showBanner(`Failed to attach library: ${err.message || String(err)}`, "error");
        } finally {
          button.disabled = false;
        }
      });
    });

    this.listContainerElem.querySelectorAll(".detach-from-ws-btn").forEach((btn) => {
      btn.addEventListener("click", async (e) => {
        const libId = Number((e.currentTarget as HTMLElement).getAttribute("data-lib-id"));
        if (!this.activeWsId || !libId) return;
        const button = btn as HTMLButtonElement;
        try {
          button.disabled = true;
          await api.detachLibrary(this.activeWsId, libId);
          await this.loadLibraries();
          this.callbacks.onLibrariesChanged?.();
          api.triggerIndexing(this.activeWsId, "incremental", false).catch(() => {});
        } catch (err: any) {
          this.showBanner(`Failed to detach library: ${err.message || String(err)}`, "error");
        } finally {
          button.disabled = false;
        }
      });
    });

    this.listContainerElem.querySelectorAll(".index-lib-btn").forEach((btn) => {
      btn.addEventListener("click", async (e) => {
        const libId = Number((e.currentTarget as HTMLElement).getAttribute("data-lib-id"));
        if (!libId) return;
        const button = btn as HTMLButtonElement;
        try {
          button.disabled = true;
          button.textContent = "Indexing...";
          await api.indexLibrary(libId);
          this.showBanner(`Indexing triggered for library #${libId}.`, "success");
        } catch (err: any) {
          this.showBanner(`Failed to index library: ${err.message || String(err)}`, "error");
        } finally {
          button.disabled = false;
          button.textContent = "Index";
        }
      });
    });
  }

  private parseList(str: string): string[] {
    return str
      .split(/[,;\n]+/)
      .map((s) => s.trim())
      .filter((s) => s.length > 0);
  }

  private updateLanguageFields(): void {
    const isCsharp = this.libLangSelect.value === "csharp";
    (this.element.querySelector(".language-standard-group") as HTMLElement).style.display =
      isCsharp ? "none" : "block";
    (this.element.querySelector(".target-framework-group") as HTMLElement).style.display =
      isCsharp ? "block" : "none";
  }

  private async handleCreate(andIndex: boolean): Promise<void> {
    const name = this.libNameInput.value.trim();
    if (!name) {
      this.showBanner("Please provide a name for the library profile.", "error");
      this.libNameInput.focus();
      return;
    }

    const language = this.libLangSelect.value;
    const sourceRoots = this.parseList(this.libRootsInput.value);
    if (sourceRoots.length === 0) {
      this.showBanner("Please provide at least one local source root.", "error");
      this.libRootsInput.focus();
      return;
    }

    const standard = this.libStdInput.value.trim() || undefined;
    const targetFramework = this.libTargetFrameworkInput.value.trim() || undefined;
    if (language === "csharp" && !targetFramework) {
      this.showBanner("Please provide a target framework moniker, such as net8.0.", "error");
      this.libTargetFrameworkInput.focus();
      return;
    }
    const provider = this.libProviderInput.value.trim() || "custom";
    const sdkVersion = this.libVersionInput.value.trim() || undefined;
    const defines = this.parseList(this.libDefinesInput.value);

    const req: CreateLibraryRequest = {
      name,
      language,
      sourceRoots,
      rootPath: sourceRoots[0],
      languageStandard: language === "csharp" ? undefined : standard,
      targetFramework: language === "csharp" ? targetFramework : undefined,
      provider,
      sdkVersion,
      defines: defines.length > 0 ? defines : undefined,
    };

    try {
      this.submitCreateBtn.disabled = true;
      this.submitCreateIndexBtn.disabled = true;
      this.hideBanner();

      const created = await api.createLibrary(req);

      if (andIndex && created.id) {
        api.indexLibrary(created.id).catch((err) => {
          console.warn("Library indexing trigger failed:", err);
        });
      }

      this.createFormElem.style.display = "none";
      this.resetForm();
      this.showBanner(`Library profile "${created.name}" created successfully.`, "success");
      await this.loadLibraries();
      this.callbacks.onLibrariesChanged?.();
    } catch (err: any) {
      this.showBanner(err.message || String(err), "error");
    } finally {
      this.submitCreateBtn.disabled = false;
      this.submitCreateIndexBtn.disabled = false;
    }
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
