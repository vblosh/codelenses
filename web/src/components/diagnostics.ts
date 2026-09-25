import type { DiagnosticItem } from "../types";
import type { StateStore } from "../state";

export class DiagnosticsComponent {
  private element: HTMLElement;
  private store: StateStore;
  private listContainer!: HTMLElement;
  private diagnostics: DiagnosticItem[] = [];

  constructor(store: StateStore) {
    this.store = store;
    this.element = document.createElement("div");
    this.element.className = "inspector-pane-view";
    this.element.id = "inspector-diagnostics";
    this.render();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="diagnostics-list" style="flex: 1; overflow-y: auto;">
        <div class="empty-state">
          <div class="empty-state-title">No diagnostics</div>
          <div class="empty-state-desc">No errors or warnings found in this file or workspace.</div>
        </div>
      </div>
    `;

    this.listContainer = this.element.querySelector(".diagnostics-list")!;
  }

  setDiagnostics(diagnostics: DiagnosticItem[]): void {
    this.diagnostics = diagnostics;
    this.renderList();
  }

  getDiagnostics(): DiagnosticItem[] {
    return this.diagnostics;
  }

  private renderList(): void {
    if (!this.diagnostics || this.diagnostics.length === 0) {
      this.listContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No diagnostics</div>
          <div class="empty-state-desc">No errors or warnings found in this file.</div>
        </div>
      `;
      return;
    }

    this.listContainer.innerHTML = "";
    const fragment = document.createDocumentFragment();

    for (const diag of this.diagnostics) {
      const item = document.createElement("div");
      item.className = "diag-item";

      const header = document.createElement("div");
      header.className = `diag-header ${diag.severity}`;
      const sevSpan = document.createElement("span");
      sevSpan.textContent = `[${diag.severity.toUpperCase()}]`;
      header.appendChild(sevSpan);
      if (diag.code) {
        const codeSpan = document.createElement("span");
        codeSpan.textContent = ` ${diag.code}`;
        header.appendChild(codeSpan);
      }

      const msgDiv = document.createElement("div");
      msgDiv.className = "diag-message";
      msgDiv.textContent = diag.message;

      const locDiv = document.createElement("div");
      locDiv.className = "diag-location";
      const pathStr = diag.relativePath || diag.filePath;
      locDiv.textContent = pathStr
        ? `${pathStr}:${diag.line}${diag.column ? `:${diag.column}` : ""}`
        : `Line ${diag.line}`;

      item.appendChild(header);
      item.appendChild(msgDiv);
      item.appendChild(locDiv);

      item.addEventListener("click", () => {
        if (diag.fileId) {
          this.store.selectFile(diag.fileId, diag.line);
        } else {
          this.store.selectLine(diag.line);
        }
      });

      fragment.appendChild(item);
    }

    this.listContainer.appendChild(fragment);
  }
}
