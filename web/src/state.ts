import type { AppState, IndexStatus, OpenTabDto } from "./types";

export type StateListener = (
  state: Readonly<AppState>,
  changedKeys: (keyof AppState)[]
) => void;

export class StateStore {
  private state: AppState;
  private listeners: Set<StateListener> = new Set();

  constructor(initialState?: Partial<AppState>) {
    this.state = {
      workspaceId: null,
      selectedFileId: null,
      selectedLine: null,
      selectedSymbolId: null,
      openTabs: [],
      expandedFolders: new Set<string>(),
      indexStatus: "idle",
      activeMobileTab: "explorer",
      activeInspectorTab: "outline",
      searchQuery: "",
      searchType: "source",
      isSearching: false,
      ...initialState,
    };
  }

  getState(): Readonly<AppState> {
    return this.state;
  }

  setState(partial: Partial<AppState>): void {
    const changedKeys: (keyof AppState)[] = [];
    for (const key of Object.keys(partial) as (keyof AppState)[]) {
      if (this.state[key] !== partial[key]) {
        changedKeys.push(key);
      }
    }

    if (changedKeys.length === 0) {
      return;
    }

    this.state = {
      ...this.state,
      ...partial,
    };

    this.notify(changedKeys);
  }

  subscribe(listener: StateListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notify(changedKeys: (keyof AppState)[]): void {
    for (const listener of this.listeners) {
      try {
        listener(this.state, changedKeys);
      } catch (err) {
        console.error("Error in state subscriber:", err);
      }
    }
  }

  // Convenient actions
  setWorkspace(workspaceId: number | null): void {
    if (this.state.workspaceId !== workspaceId) {
      this.setState({
        workspaceId,
        selectedFileId: null,
        selectedLine: null,
        selectedSymbolId: null,
        openTabs: [],
        expandedFolders: new Set<string>(),
      });
    }
  }

  selectFile(
    fileId: number | null,
    line: number | null = null,
    meta?: { relativePath?: string; name?: string }
  ): void {
    const update: Partial<AppState> = {
      selectedFileId: fileId,
      selectedLine: line,
      selectedSymbolId: null,
      activeMobileTab: "code",
    };

    if (fileId !== null) {
      const existingTab = this.state.openTabs.find((t) => t.fileId === fileId);
      if (!existingTab) {
        const newTab: OpenTabDto = {
          fileId,
          relativePath: meta?.relativePath || "",
          name: meta?.name || meta?.relativePath || `File #${fileId}`,
        };
        update.openTabs = [...this.state.openTabs, newTab];
      } else if (meta && (meta.relativePath || meta.name)) {
        update.openTabs = this.state.openTabs.map((t) =>
          t.fileId === fileId
            ? {
                ...t,
                relativePath: meta.relativePath || t.relativePath,
                name: meta.name || t.name,
              }
            : t
        );
      }
    }

    this.setState(update);
  }

  ensureTabOpen(tab: OpenTabDto): void {
    const existing = this.state.openTabs.find((t) => t.fileId === tab.fileId);
    if (!existing) {
      this.setState({ openTabs: [...this.state.openTabs, tab] });
    } else if (existing.relativePath !== tab.relativePath || existing.name !== tab.name) {
      this.setState({
        openTabs: this.state.openTabs.map((t) => (t.fileId === tab.fileId ? tab : t)),
      });
    }
  }

  closeTab(fileId: number): void {
    const index = this.state.openTabs.findIndex((t) => t.fileId === fileId);
    if (index === -1) return;

    const remainingTabs = this.state.openTabs.filter((t) => t.fileId !== fileId);
    const update: Partial<AppState> = { openTabs: remainingTabs };

    if (this.state.selectedFileId === fileId) {
      if (remainingTabs.length > 0) {
        const nextIndex = Math.min(index, remainingTabs.length - 1);
        update.selectedFileId = remainingTabs[nextIndex].fileId;
        update.selectedLine = null;
        update.selectedSymbolId = null;
      } else {
        update.selectedFileId = null;
        update.selectedLine = null;
        update.selectedSymbolId = null;
      }
    }

    this.setState(update);
  }

  selectLine(line: number | null): void {
    this.setState({ selectedLine: line });
  }

  selectSymbol(symbolId: number | null, line?: number | null): void {
    const update: Partial<AppState> = {
      selectedSymbolId: symbolId,
      activeInspectorTab: "references",
    };
    if (line !== undefined && line !== null) {
      update.selectedLine = line;
    }
    this.setState(update);
  }

  toggleFolder(path: string): void {
    const nextFolders = new Set(this.state.expandedFolders);
    if (nextFolders.has(path)) {
      nextFolders.delete(path);
    } else {
      nextFolders.add(path);
    }
    this.setState({ expandedFolders: nextFolders });
  }

  setIndexStatus(indexStatus: IndexStatus): void {
    this.setState({ indexStatus });
  }

  setActiveMobileTab(activeMobileTab: AppState["activeMobileTab"]): void {
    this.setState({ activeMobileTab });
  }

  setActiveInspectorTab(activeInspectorTab: AppState["activeInspectorTab"]): void {
    this.setState({ activeInspectorTab });
  }
}

export const store = new StateStore();
