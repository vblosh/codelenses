import type { StateStore } from "./state";

export interface RouteParams {
  workspaceId: number | null;
  fileId: number | null;
  line: number | null;
  symbolId: number | null;
}

export function parseHash(hash: string): RouteParams {
  const cleanHash = hash.replace(/^#\/?/, "");
  const params = new URLSearchParams(cleanHash);

  const parseNum = (val: string | null): number | null => {
    if (!val) return null;
    const n = parseInt(val, 10);
    return isNaN(n) ? null : n;
  };

  return {
    workspaceId: parseNum(params.get("workspace")),
    fileId: parseNum(params.get("file")),
    line: parseNum(params.get("line")),
    symbolId: parseNum(params.get("symbol")),
  };
}

export function buildHash(params: Partial<RouteParams>): string {
  const searchParams = new URLSearchParams();
  if (params.workspaceId !== null && params.workspaceId !== undefined) {
    searchParams.set("workspace", String(params.workspaceId));
  }
  if (params.fileId !== null && params.fileId !== undefined) {
    searchParams.set("file", String(params.fileId));
  }
  if (params.line !== null && params.line !== undefined) {
    searchParams.set("line", String(params.line));
  }
  if (params.symbolId !== null && params.symbolId !== undefined) {
    searchParams.set("symbol", String(params.symbolId));
  }
  const str = searchParams.toString();
  return str ? `#${str}` : "";
}

export class Router {
  private store: StateStore;
  private isUpdatingFromHash = false;
  private isUpdatingFromStore = false;

  constructor(store: StateStore) {
    this.store = store;
  }

  init(): void {
    // Read initial hash on load
    this.handleHashChange();

    // Listen to hash changes in browser
    window.addEventListener("hashchange", () => this.handleHashChange());
    window.addEventListener("popstate", () => this.handleHashChange());

    // Listen to store updates to keep URL hash in sync
    this.store.subscribe((state, changedKeys) => {
      const routingKeys = [
        "workspaceId",
        "selectedFileId",
        "selectedLine",
        "selectedSymbolId",
      ] as const;

      const hasRoutingChange = changedKeys.some((k) =>
        (routingKeys as readonly string[]).includes(k)
      );

      if (hasRoutingChange && !this.isUpdatingFromHash) {
        this.updateHashFromState(state);
      }
    });
  }

  private handleHashChange(): void {
    if (this.isUpdatingFromStore) return;

    this.isUpdatingFromHash = true;
    try {
      const route = parseHash(window.location.hash);
      const state = this.store.getState();

      const updates: Record<string, any> = {};
      if (route.workspaceId !== state.workspaceId) {
        updates.workspaceId = route.workspaceId;
      }
      if (route.fileId !== state.selectedFileId) {
        updates.selectedFileId = route.fileId;
      }
      if (route.line !== state.selectedLine) {
        updates.selectedLine = route.line;
      }
      if (route.symbolId !== state.selectedSymbolId) {
        updates.selectedSymbolId = route.symbolId;
      }

      if (Object.keys(updates).length > 0) {
        this.store.setState(updates);
      }
    } finally {
      this.isUpdatingFromHash = false;
    }
  }

  private updateHashFromState(state: ReturnType<StateStore["getState"]>): void {
    this.isUpdatingFromStore = true;
    try {
      const newHash = buildHash({
        workspaceId: state.workspaceId,
        fileId: state.selectedFileId,
        line: state.selectedLine,
        symbolId: state.selectedSymbolId,
      });

      if (window.location.hash !== newHash) {
        if (!newHash) {
          // Keep base url clean
          history.pushState(null, "", window.location.pathname + window.location.search);
        } else {
          history.pushState(null, "", newHash);
        }
      }
    } finally {
      this.isUpdatingFromStore = false;
    }
  }

  navigate(params: Partial<RouteParams>): void {
    const nextState = {
      workspaceId: params.workspaceId !== undefined ? params.workspaceId : this.store.getState().workspaceId,
      selectedFileId: params.fileId !== undefined ? params.fileId : this.store.getState().selectedFileId,
      selectedLine: params.line !== undefined ? params.line : this.store.getState().selectedLine,
      selectedSymbolId: params.symbolId !== undefined ? params.symbolId : this.store.getState().selectedSymbolId,
    };
    this.store.setState(nextState);
  }
}
