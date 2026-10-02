import { describe, it, expect, vi } from "vitest";
import { StateStore } from "../src/state";

describe("StateStore", () => {
  it("initializes with default values", () => {
    const store = new StateStore();
    const state = store.getState();
    expect(state.workspaceId).toBeNull();
    expect(state.selectedFileId).toBeNull();
    expect(state.selectedLine).toBeNull();
    expect(state.selectedSymbolId).toBeNull();
    expect(state.expandedFolders.size).toBe(0);
    expect(state.indexStatus).toBe("idle");
    expect(state.searchType).toBe("symbol");
  });

  it("updates state and notifies subscribers", () => {
    const store = new StateStore();
    const listener = vi.fn();
    const unsubscribe = store.subscribe(listener);

    store.setState({ selectedFileId: 42 });
    expect(store.getState().selectedFileId).toBe(42);
    expect(listener).toHaveBeenCalledWith(
      expect.objectContaining({ selectedFileId: 42 }),
      ["selectedFileId"]
    );

    unsubscribe();
    store.setState({ selectedFileId: 100 });
    expect(listener).toHaveBeenCalledTimes(1);
  });

  it("toggles folders in expandedFolders set", () => {
    const store = new StateStore();
    store.toggleFolder("src/components");
    expect(store.getState().expandedFolders.has("src/components")).toBe(true);

    store.toggleFolder("src/components");
    expect(store.getState().expandedFolders.has("src/components")).toBe(false);
  });

  it("selectFile resets line if not provided and switches mobile tab", () => {
    const store = new StateStore();
    store.selectFile(12, 45);
    expect(store.getState().selectedFileId).toBe(12);
    expect(store.getState().selectedLine).toBe(45);
    expect(store.getState().activeMobileTab).toBe("code");

    store.selectFile(15);
    expect(store.getState().selectedFileId).toBe(15);
    expect(store.getState().selectedLine).toBeNull();
  });

  it("selectSymbol activates references inspector tab", () => {
    const store = new StateStore();
    store.selectSymbol(99, 120);
    expect(store.getState().selectedSymbolId).toBe(99);
    expect(store.getState().selectedLine).toBe(120);
    expect(store.getState().activeInspectorTab).toBe("references");
  });

  it("setWorkspace clears file and symbol selections and open tabs", () => {
    const store = new StateStore({
      workspaceId: 1,
      selectedFileId: 5,
      selectedLine: 10,
      selectedSymbolId: 2,
      openTabs: [{ fileId: 5, relativePath: "src/main.cpp", name: "main.cpp" }],
    });

    store.setWorkspace(2);
    expect(store.getState().workspaceId).toBe(2);
    expect(store.getState().selectedFileId).toBeNull();
    expect(store.getState().selectedLine).toBeNull();
    expect(store.getState().selectedSymbolId).toBeNull();
    expect(store.getState().openTabs).toEqual([]);
  });

  it("manages openTabs with selectFile, ensureTabOpen, and closeTab", () => {
    const store = new StateStore();

    // Opening files adds them to openTabs
    store.selectFile(10, null, { relativePath: "src/a.cpp", name: "a.cpp" });
    store.selectFile(20, null, { relativePath: "src/b.cpp", name: "b.cpp" });
    store.selectFile(30, null, { relativePath: "src/c.cpp", name: "c.cpp" });

    expect(store.getState().openTabs).toHaveLength(3);
    expect(store.getState().selectedFileId).toBe(30);

    // Closing the active tab selects an adjacent tab
    store.closeTab(30);
    expect(store.getState().openTabs).toHaveLength(2);
    expect(store.getState().selectedFileId).toBe(20);

    // Closing middle tab
    store.selectFile(10);
    store.closeTab(10);
    expect(store.getState().openTabs).toHaveLength(1);
    expect(store.getState().selectedFileId).toBe(20);

    // Closing last tab clears selection
    store.closeTab(20);
    expect(store.getState().openTabs).toHaveLength(0);
    expect(store.getState().selectedFileId).toBeNull();
  });
});
