import type { WorkspaceDto, CreateWorkspaceRequest } from "../types";
import { api } from "../api";

export interface AddWorkspaceCallbacks {
  onCreated?: (workspace: WorkspaceDto) => void;
}

export class AddWorkspaceModal {
  private element: HTMLElement;
  private callbacks: AddWorkspaceCallbacks;

  private rootInput!: HTMLInputElement;
  private nameInput!: HTMLInputElement;
  private cdbInput!: HTMLInputElement;
  private defaultCmdInput!: HTMLInputElement;
  private includePatternsInput!: HTMLInputElement;
  private excludePatternsInput!: HTMLInputElement;
  private bannerElem!: HTMLElement;
  private submitBtn!: HTMLButtonElement;
  private submitIndexBtn!: HTMLButtonElement;
  private cancelBtn!: HTMLButtonElement;
  private closeBtn!: HTMLButtonElement;

  constructor(callbacks: AddWorkspaceCallbacks = {}) {
    this.callbacks = callbacks;
    this.element = document.createElement("div");
    this.element.className = "modal-backdrop add-workspace-backdrop";
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
      <div class="modal add-workspace-modal" role="dialog" aria-labelledby="add-ws-title">
        <div class="modal-header">
          <div class="modal-title" id="add-ws-title">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right: 6px;">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path>
              <line x1="12" y1="11" x2="12" y2="17"></line>
              <line x1="9" y1="14" x2="15" y2="14"></line>
            </svg>
            <span>Add Workspace</span>
          </div>
          <button class="btn-icon close-add-ws-btn" title="Close (Esc)">✕</button>
        </div>

        <div class="modal-body">
          <div class="alert-banner error add-ws-banner" style="display: none;"></div>

          <div class="form-group">
            <label class="form-label" for="add-ws-root">Root Directory Path <span style="color: var(--error);">*</span></label>
            <input type="text" id="add-ws-root" class="form-input ws-add-root" placeholder="e.g. /home/user/projects/my-repo" required />
            <div class="form-hint">Absolute or canonical path to the directory on the host machine.</div>
          </div>

          <div class="form-group">
            <label class="form-label" for="add-ws-name">Workspace Name</label>
            <input type="text" id="add-ws-name" class="form-input ws-add-name" placeholder="Optional display name (defaults to folder name)" />
          </div>

          <div class="form-group">
            <label class="form-label" for="add-ws-cdb">Compilation Database Path (optional)</label>
            <input type="text" id="add-ws-cdb" class="form-input ws-add-cdb" placeholder="e.g. compile_commands.json or build/compile_commands.json" />
            <div class="form-hint">Relative to root directory or absolute path. Leave blank to auto-detect.</div>
          </div>

          <div class="form-group">
            <label class="form-label" for="add-ws-default-cmd">Default Compile Command (optional)</label>
            <input type="text" id="add-ws-default-cmd" class="form-input ws-add-default-cmd" placeholder="e.g. clang++ -std=c++20 -Iinclude" />
            <div class="form-hint">Fallback compilation command for C/C++ files without an individual compile command.</div>
          </div>

          <div class="form-row">
            <div class="form-group">
              <label class="form-label" for="add-ws-includes">Include Patterns (optional)</label>
              <input type="text" id="add-ws-includes" class="form-input ws-add-includes" placeholder="e.g. src/**, include/**" />
              <div class="form-hint">Comma-separated glob filters.</div>
            </div>
            <div class="form-group">
              <label class="form-label" for="add-ws-excludes">Exclude Patterns (optional)</label>
              <input type="text" id="add-ws-excludes" class="form-input ws-add-excludes" placeholder="e.g. tests/fixtures/**, vendor/**" />
              <div class="form-hint">Comma-separated glob filters.</div>
            </div>
          </div>
        </div>

        <div class="modal-footer">
          <button type="button" class="btn cancel-add-btn">Cancel</button>
          <button type="button" class="btn submit-add-btn">Add Workspace</button>
          <button type="button" class="btn btn-primary submit-add-index-btn">Add & Index Now</button>
        </div>
      </div>
    `;

    this.rootInput = this.element.querySelector(".ws-add-root")!;
    this.nameInput = this.element.querySelector(".ws-add-name")!;
    this.cdbInput = this.element.querySelector(".ws-add-cdb")!;
    this.defaultCmdInput = this.element.querySelector(".ws-add-default-cmd")!;
    this.includePatternsInput = this.element.querySelector(".ws-add-includes")!;
    this.excludePatternsInput = this.element.querySelector(".ws-add-excludes")!;
    this.bannerElem = this.element.querySelector(".add-ws-banner")!;
    this.submitBtn = this.element.querySelector(".submit-add-btn")!;
    this.submitIndexBtn = this.element.querySelector(".submit-add-index-btn")!;
    this.cancelBtn = this.element.querySelector(".cancel-add-btn")!;
    this.closeBtn = this.element.querySelector(".close-add-ws-btn")!;
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

    this.submitBtn.addEventListener("click", () => this.handleSubmit(false));
    this.submitIndexBtn.addEventListener("click", () => this.handleSubmit(true));

    this.rootInput.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        this.handleSubmit(false);
      }
    });
  }

  open(): void {
    this.resetForm();
    this.element.style.display = "flex";
    setTimeout(() => {
      this.rootInput.focus();
    }, 50);
  }

  close(): void {
    this.element.style.display = "none";
    this.resetForm();
  }

  private resetForm(): void {
    this.rootInput.value = "";
    this.nameInput.value = "";
    this.cdbInput.value = "";
    this.defaultCmdInput.value = "";
    this.includePatternsInput.value = "";
    this.excludePatternsInput.value = "";
    this.hideError();
  }

  private showError(msg: string): void {
    this.bannerElem.textContent = msg;
    this.bannerElem.style.display = "flex";
  }

  private hideError(): void {
    this.bannerElem.style.display = "none";
    this.bannerElem.textContent = "";
  }

  private parsePatternList(str: string): string[] {
    return str
      .split(/[,;\n]+/)
      .map((s) => s.trim())
      .filter((s) => s.length > 0);
  }

  private async handleSubmit(andIndex: boolean): Promise<void> {
    const rootPath = this.rootInput.value.trim();
    if (!rootPath) {
      this.showError("Please specify a root directory path for the workspace.");
      this.rootInput.focus();
      return;
    }

    const name = this.nameInput.value.trim();
    const cdbPath = this.cdbInput.value.trim();
    const defaultCmd = this.defaultCmdInput.value.trim();
    const includePatterns = this.parsePatternList(this.includePatternsInput.value);
    const excludePatterns = this.parsePatternList(this.excludePatternsInput.value);

    const req: CreateWorkspaceRequest = {
      rootPath,
      name: name || undefined,
      compileCommandsPath: cdbPath || null,
      defaultCompileCommand: defaultCmd || null,
      includePatterns: includePatterns.length > 0 ? includePatterns : undefined,
      excludePatterns: excludePatterns.length > 0 ? excludePatterns : undefined,
    };

    try {
      this.submitBtn.disabled = true;
      this.submitIndexBtn.disabled = true;
      this.hideError();

      const created = await api.createWorkspace(req);

      if (andIndex && created.id) {
        api.triggerIndexing(created.id, "incremental", false).catch((err) => {
          console.warn("Auto-index trigger failed:", err);
        });
      }

      this.close();
      if (this.callbacks.onCreated) {
        this.callbacks.onCreated(created);
      }
    } catch (err: any) {
      this.showError(err.message || String(err));
    } finally {
      this.submitBtn.disabled = false;
      this.submitIndexBtn.disabled = false;
    }
  }
}
