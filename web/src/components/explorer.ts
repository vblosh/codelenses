import type { TreeNodeDto } from "../types";
import type { StateStore } from "../state";
import { api } from "../api";
import { formatByteSize } from "../rendering/ranges";

export class ExplorerComponent {
  private element: HTMLElement;
  private store: StateStore;
  private treeContainer!: HTMLElement;
  private refreshBtn!: HTMLButtonElement;
  private treeCache: Map<string, TreeNodeDto[]> = new Map();
  private loadingNodes: Set<string> = new Set();

  constructor(store: StateStore) {
    this.store = store;
    this.element = document.createElement("div");
    this.element.className = "pane";
    this.element.id = "pane-explorer";
    this.render();
    this.initEvents();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  private render(): void {
    this.element.innerHTML = `
      <div class="pane-header">
        <span>Explorer</span>
        <button class="btn-icon refresh-btn" title="Refresh file tree">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
        </button>
      </div>
      <div class="pane-content explorer-tree" role="tree">
        <div class="empty-state">
          <div class="empty-state-title">Loading tree...</div>
        </div>
      </div>
    `;

    this.treeContainer = this.element.querySelector(".explorer-tree")!;
    this.refreshBtn = this.element.querySelector(".refresh-btn")!;
  }

  private initEvents(): void {
    this.refreshBtn.addEventListener("click", () => {
      this.treeCache.clear();
      this.loadRootTree();
    });

    this.store.subscribe((_state, changedKeys) => {
      if (changedKeys.includes("workspaceId")) {
        this.treeCache.clear();
        this.loadRootTree();
      } else if (
        changedKeys.includes("selectedFileId") ||
        changedKeys.includes("expandedFolders")
      ) {
        this.renderTree();
      }
    });
  }

  async loadRootTree(): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId) {
      this.treeContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No workspace</div>
          <div class="empty-state-desc">Select a workspace to browse files.</div>
        </div>
      `;
      return;
    }

    try {
      this.treeContainer.innerHTML = `<div class="empty-state"><div class="empty-state-title">Loading files...</div></div>`;
      const rootTree = await api.getTree(wsId, "");
      this.treeCache.set("", rootTree.entries || []);
      this.renderTree();
    } catch (err: any) {
      this.treeContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title" style="color: var(--error);">Failed to load tree</div>
          <div class="empty-state-desc">${err.message}</div>
        </div>
      `;
    }
  }

  private async fetchSubtree(path: string): Promise<void> {
    const wsId = this.store.getState().workspaceId;
    if (!wsId || this.loadingNodes.has(path)) return;

    this.loadingNodes.add(path);
    try {
      const res = await api.getTree(wsId, path);
      this.treeCache.set(path, res.entries || []);
    } catch (err) {
      console.error(`Failed to fetch subtree for ${path}:`, err);
    } finally {
      this.loadingNodes.delete(path);
      this.renderTree();
    }
  }

  private renderTree(): void {
    const rootEntries = this.treeCache.get("");
    if (!rootEntries) return;

    if (rootEntries.length === 0) {
      this.treeContainer.innerHTML = `
        <div class="empty-state">
          <div class="empty-state-title">No files found</div>
          <div class="empty-state-desc">Workspace directory is empty.</div>
        </div>
      `;
      return;
    }

    this.treeContainer.innerHTML = "";
    const fragment = document.createDocumentFragment();
    this.renderNodes(rootEntries, 0, fragment);
    this.treeContainer.appendChild(fragment);
  }

  private renderNodes(nodes: TreeNodeDto[], level: number, container: DocumentFragment | HTMLElement): void {
    const state = this.store.getState();

    // Sort: directories first, then alphabetical
    const sorted = [...nodes].sort((a, b) => {
      if (a.type !== b.type) {
        return a.type === "directory" ? -1 : 1;
      }
      return a.name.localeCompare(b.name);
    });

    for (const node of sorted) {
      const isDir = node.type === "directory";
      const isExpanded = isDir && state.expandedFolders.has(node.path);
      const isSelected = !isDir && node.fileId === state.selectedFileId;

      const itemDiv = document.createElement("div");
      itemDiv.className = `tree-item${isSelected ? " selected" : ""}`;
      itemDiv.style.paddingLeft = `${level * 14 + 8}px`;
      itemDiv.dataset.path = node.path;
      if (node.fileId) itemDiv.dataset.fileId = String(node.fileId);

      // Icon & expand arrow
      if (isDir) {
        const arrow = document.createElement("span");
        arrow.className = `tree-arrow${isExpanded ? " expanded" : ""}`;
        arrow.textContent = "▶";
        itemDiv.appendChild(arrow);

        const folderIcon = document.createElement("span");
        folderIcon.className = "tree-icon";
        folderIcon.innerHTML = isExpanded
          ? `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#60a5fa" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path></svg>`
          : `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#93c5fd" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path></svg>`;
        itemDiv.appendChild(folderIcon);
      } else {
        const spacer = document.createElement("span");
        spacer.style.width = "12px";
        itemDiv.appendChild(spacer);

        const fileIcon = document.createElement("span");
        fileIcon.className = "tree-icon";
        fileIcon.innerHTML = `<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path><polyline points="14 2 14 8 20 8"></polyline></svg>`;
        itemDiv.appendChild(fileIcon);
      }

      // File/folder name
      const nameSpan = document.createElement("span");
      nameSpan.className = "tree-name";
      nameSpan.textContent = node.name;
      itemDiv.appendChild(nameSpan);

      // Size badge for files
      if (!isDir && node.sizeBytes > 0) {
        const sizeBadge = document.createElement("span");
        sizeBadge.className = "tree-badge";
        sizeBadge.textContent = formatByteSize(node.sizeBytes);
        itemDiv.appendChild(sizeBadge);
      }

      // Click handling
      itemDiv.addEventListener("click", () => {
        if (isDir) {
          this.store.toggleFolder(node.path);
          if (!this.treeCache.has(node.path)) {
            this.fetchSubtree(node.path);
          }
        } else if (node.fileId) {
          this.store.selectFile(node.fileId, null, {
            relativePath: node.path,
            name: node.name,
          });
        }
      });

      container.appendChild(itemDiv);

      // Render children if folder is expanded
      if (isDir && isExpanded) {
        const children = this.treeCache.get(node.path);
        if (children) {
          this.renderNodes(children, level + 1, container);
        } else if (this.loadingNodes.has(node.path)) {
          const loadingDiv = document.createElement("div");
          loadingDiv.className = "tree-item";
          loadingDiv.style.paddingLeft = `${(level + 1) * 14 + 8}px`;
          loadingDiv.style.color = "var(--text-muted)";
          loadingDiv.textContent = "Loading...";
          container.appendChild(loadingDiv);
        } else {
          // Trigger lazy fetch
          this.fetchSubtree(node.path);
        }
      }
    }
  }
}
