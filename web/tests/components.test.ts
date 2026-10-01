import { describe, it, expect, vi, beforeEach } from "vitest";
import { StateStore } from "../src/state";
import { ToolbarComponent } from "../src/components/toolbar";
import { ExplorerComponent } from "../src/components/explorer";
import { CodeWindowComponent } from "../src/components/code-window";
import { OutlineComponent } from "../src/components/outline";
import { ReferencesComponent } from "../src/components/references";
import { DiagnosticsComponent } from "../src/components/diagnostics";
import { CompileCommandComponent } from "../src/components/compile-command";
import {
  WorkspaceSettingsModal,
  WorkspaceCompileCommandsModal,
  parseCommandPreview,
} from "../src/components/workspace-settings";
import { AddWorkspaceModal } from "../src/components/add-workspace";
import { IndexingSettingsForm } from "../src/components/indexing-settings";
import { AppComponent } from "../src/components/app";
import { api } from "../src/api";

describe("Frontend components", () => {
  let store: StateStore;

  beforeEach(() => {
    store = new StateStore({ workspaceId: 1 });
    vi.restoreAllMocks();
  });

  describe("ToolbarComponent", () => {
    it("renders brand, select, search and index controls", async () => {
      vi.spyOn(api, "getWorkspaces").mockResolvedValueOnce({
        workspaces: [{ id: 1, name: "ws1", rootPath: "/ws1", revision: 1, status: "idle", createdAt: "", updatedAt: "" }],
        total: 1,
      });
      vi.spyOn(api, "getWorkspaceStatus").mockResolvedValueOnce({
        workspaceId: 1,
        status: "idle",
        revision: 1,
        fileCount: 10,
        symbolCount: 50,
        diagnosticCounts: { total: 0, errors: 0, warnings: 0, info: 0 },
      });

      const onSearch = vi.fn();
      const toolbar = new ToolbarComponent(store, { onSearch });
      const elem = toolbar.getElement();

      expect(elem.querySelector(".brand")?.textContent).toContain("CodeLenses");
      expect(elem.querySelector(".workspace-select")).not.toBeNull();
      expect(elem.querySelector(".add-workspace-btn")).not.toBeNull();
      expect(elem.querySelector(".manage-links-btn")).not.toBeNull();
      expect(elem.querySelector(".search-input")).not.toBeNull();
      expect(elem.querySelector(".index-status-badge")).not.toBeNull();

      toolbar.destroy();
    });

    it("cancels status polls for old workspaces and on teardown", async () => {
      vi.spyOn(api, "getWorkspaces").mockResolvedValue({
        workspaces: [
          { id: 1, name: "ws1", rootPath: "/ws1", revision: 1, status: "idle", createdAt: "", updatedAt: "" },
          { id: 2, name: "ws2", rootPath: "/ws2", revision: 1, status: "idle", createdAt: "", updatedAt: "" },
        ],
        total: 2,
      });
      const statusRequest = vi.spyOn(api, "getWorkspaceStatus").mockImplementation(
        (workspaceId, signal) => {
          if (workspaceId === 2) {
            return Promise.resolve({
              workspaceId,
              status: "idle",
              revision: 1,
              latestJob: null,
              fileCount: 2,
              symbolCount: 0,
              diagnosticCounts: { total: 0, errors: 0, warnings: 0, info: 0 },
            });
          }
          return new Promise((_, reject) => {
            signal?.addEventListener(
              "abort",
              () => reject(new DOMException("Aborted", "AbortError")),
              { once: true }
            );
          });
        }
      );

      const toolbar = new ToolbarComponent(store, { onSearch: vi.fn() });
      await vi.waitFor(() => expect(statusRequest).toHaveBeenCalledTimes(1));
      const firstSignal = statusRequest.mock.calls[0][1]!;

      await toolbar.fetchStatus();
      expect(statusRequest).toHaveBeenCalledTimes(1);

      store.setWorkspace(2);
      await vi.waitFor(() => expect(statusRequest).toHaveBeenCalledTimes(2));
      expect(firstSignal.aborted).toBe(true);
      await vi.waitFor(() => {
        expect(toolbar.getElement().querySelector(".status-text")?.textContent).toBe(
          "Indexed (2 files)"
        );
      });

      store.setWorkspace(1);
      expect(statusRequest).toHaveBeenCalledTimes(3);
      const lastSignal = statusRequest.mock.calls[2][1]!;
      toolbar.destroy();
      expect(lastSignal.aborted).toBe(true);
    });

    it("triggers onAddWorkspace and onManageLinks callbacks when buttons are clicked", async () => {
      vi.spyOn(api, "getWorkspaces").mockResolvedValueOnce({
        workspaces: [{ id: 1, name: "ws1", rootPath: "/ws1", revision: 1, status: "idle", createdAt: "", updatedAt: "" }],
        total: 1,
      });

      const onSearch = vi.fn();
      const onAddWorkspace = vi.fn();
      const onManageLinks = vi.fn();
      const toolbar = new ToolbarComponent(store, { onSearch, onAddWorkspace, onManageLinks });
      const elem = toolbar.getElement();

      const addBtn = elem.querySelector(".add-workspace-btn") as HTMLButtonElement;
      expect(addBtn).not.toBeNull();
      addBtn.click();
      expect(onAddWorkspace).toHaveBeenCalledTimes(1);

      const libsBtn = elem.querySelector(".manage-links-btn") as HTMLButtonElement;
      expect(libsBtn).not.toBeNull();
      libsBtn.click();
      expect(onManageLinks).toHaveBeenCalledTimes(1);

      toolbar.destroy();
    });
  });

  describe("ExplorerComponent", () => {
    it("loads and displays tree items", async () => {
      vi.spyOn(api, "getTree").mockResolvedValueOnce({
        workspaceId: 1,
        path: "",
        entries: [
          { name: "src", path: "src", type: "directory", sizeBytes: 0, isBinary: false, language: "" },
          { name: "main.cpp", path: "main.cpp", type: "file", fileId: 10, sizeBytes: 120, isBinary: false, language: "cpp" },
        ],
      });

      const explorer = new ExplorerComponent(store);
      await explorer.loadRootTree();

      const elem = explorer.getElement();
      const items = elem.querySelectorAll(".tree-item");
      expect(items.length).toBe(2);

      // Click on file selects it in store
      const fileItem = elem.querySelector('.tree-item[data-file-id="10"]') as HTMLElement;
      expect(fileItem).not.toBeNull();
      fileItem.click();
      expect(store.getState().selectedFileId).toBe(10);
    });
  });

  describe("CodeWindowComponent", () => {
    it("renders code rows for loaded file", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 10,
        workspaceId: 1,
        path: "/src/main.cpp",
        relativePath: "src/main.cpp",
        name: "main.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 30,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 10,
        path: "src/main.cpp",
        content: "int main() {\n  return 0;\n}",
        totalSizeBytes: 30,
        totalLines: 3,
        startLine: 0,
        endLine: 2,
        startByte: 0,
        endByte: 30,
        isBinary: false,
        contentHash: "hash123",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 10,
        legend: { tokenTypes: ["keyword", "function"] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 10,
        occurrences: [],
        total: 0,
      });

      const codeWindow = new CodeWindowComponent(store);
      await codeWindow.loadFile(10);

      const elem = codeWindow.getElement();
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("src/main.cpp");
      expect(elem.querySelector(".lang-badge")?.textContent).toBe("cpp");

      const lines = elem.querySelectorAll<HTMLElement>(".code-line");
      expect(lines.length).toBe(3);
      expect(lines[0].dataset.line).toBe("1");
      expect(lines[1].dataset.line).toBe("2");
      expect(lines[2].dataset.line).toBe("3");
    });

    it("labels navigated library source with its target framework", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 23,
        workspaceId: 2,
        path: "/sdk/root-4/Widget.cs",
        relativePath: "root-4/Widget.cs",
        name: "Widget.cs",
        language: "csharp",
        encoding: "utf-8",
        sizeBytes: 18,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
        origin: "library",
        ownerWorkspaceId: 2,
        ownerWorkspaceName: "SDK",
        targetFramework: "net8.0",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 23,
        path: "root-4/Widget.cs",
        content: "class Widget {}",
        totalSizeBytes: 15,
        totalLines: 1,
        startLine: 0,
        endLine: 0,
        startByte: 0,
        endByte: 15,
        isBinary: false,
        contentHash: "widget-hash",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 23,
        legend: { tokenTypes: ["class"] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 23,
        occurrences: [],
        total: 0,
      });
      vi.spyOn(api, "getFileOutline").mockResolvedValueOnce({ fileId: 23, outline: [] });
      vi.spyOn(api, "getFileCompileCommand").mockResolvedValueOnce({
        fileId: 23,
        hasCompileCommand: false,
        isAutoDetected: false,
      });

      const codeWindow = new CodeWindowComponent(store);
      await codeWindow.loadFile(23);

      const badge = codeWindow.getElement().querySelector(".workspace-origin-badge");
      expect(badge?.textContent).toBe("SDK");
      expect(badge?.getAttribute("title")).toContain("SDK");
    });

    it("displays notice for binary files", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 20,
        workspaceId: 1,
        path: "/bin/app",
        relativePath: "bin/app",
        name: "app",
        language: "",
        encoding: "binary",
        sizeBytes: 1024,
        modifiedNs: 0,
        isBinary: true,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });

      const codeWindow = new CodeWindowComponent(store);
      await codeWindow.loadFile(20);

      const elem = codeWindow.getElement();
      expect(elem.textContent).toContain("Binary files cannot be displayed");
    });

    it("supports multiple open file tabs, tab switching, and closing tabs", async () => {
      vi.spyOn(api, "getFileMetadata").mockImplementation(async (_wsId, fileId) => ({
        id: fileId,
        workspaceId: 1,
        path: fileId === 10 ? "/src/main.cpp" : "/src/util.cpp",
        relativePath: fileId === 10 ? "src/main.cpp" : "src/util.cpp",
        name: fileId === 10 ? "main.cpp" : "util.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 40,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      }));

      vi.spyOn(api, "getFileContent").mockImplementation(async (_wsId, fileId) => ({
        fileId,
        path: fileId === 10 ? "src/main.cpp" : "src/util.cpp",
        content: fileId === 10 ? "// main content" : "// util content",
        totalSizeBytes: 40,
        totalLines: 1,
        startLine: 0,
        endLine: 0,
        startByte: 0,
        endByte: 40,
        isBinary: false,
        contentHash: "hash" + fileId,
      }));

      vi.spyOn(api, "getFileHighlights").mockResolvedValue({
        fileId: 10,
        legend: { tokenTypes: [] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValue({
        fileId: 10,
        occurrences: [],
        total: 0,
      });

      const codeWindow = new CodeWindowComponent(store);

      // Open file 10
      store.selectFile(10, null, { relativePath: "src/main.cpp", name: "main.cpp" });
      await codeWindow.loadFile(10);

      // Open file 20
      store.selectFile(20, null, { relativePath: "src/util.cpp", name: "util.cpp" });
      await codeWindow.loadFile(20);

      const elem = codeWindow.getElement();
      const tabsBar = elem.querySelector(".code-tabs-bar") as HTMLElement;
      expect(tabsBar).not.toBeNull();
      expect(tabsBar.style.display).not.toBe("none");

      // Verify both tabs are rendered
      const tabs = tabsBar.querySelectorAll<HTMLElement>(".code-tab");
      expect(tabs.length).toBe(2);
      expect(tabs[0].textContent).toContain("main.cpp");
      expect(tabs[1].textContent).toContain("util.cpp");
      expect(tabs[1].classList.contains("active")).toBe(true);
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("src/util.cpp");

      // Switch to first tab by clicking it
      tabs[0].click();
      expect(store.getState().selectedFileId).toBe(10);
      await codeWindow.loadFile(10);
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("src/main.cpp");

      // Close the second tab (util.cpp)
      const closeBtnUtil = tabsBar.querySelector('.code-tab[data-file-id="20"] .code-tab-close') as HTMLElement;
      closeBtnUtil.click();
      expect(store.getState().openTabs.length).toBe(1);
      expect(store.getState().selectedFileId).toBe(10);

      // Close the remaining tab
      const closeBtnMain = tabsBar.querySelector('.code-tab[data-file-id="10"] .code-tab-close') as HTMLElement;
      closeBtnMain.click();
      expect(store.getState().openTabs.length).toBe(0);
      expect(store.getState().selectedFileId).toBeNull();
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("No file open");
    });

    it("instantly locates code a couple of lines above selected symbol without smooth scroll", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 10,
        workspaceId: 1,
        path: "/src/main.cpp",
        relativePath: "src/main.cpp",
        name: "main.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 100,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 10,
        path: "src/main.cpp",
        content: "line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10",
        totalSizeBytes: 100,
        totalLines: 10,
        startLine: 0,
        endLine: 9,
        startByte: 0,
        endByte: 100,
        isBinary: false,
        contentHash: "hash123",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 10,
        legend: { tokenTypes: [] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 10,
        occurrences: [
          {
            id: 1,
            workspaceId: 1,
            fileId: 10,
            symbolId: 99,
            occurrenceKind: "definition",
            name: "testSymbol",
            range: { start: { line: 5, column: 0, byte: 0 }, end: { line: 5, column: 10, byte: 10 } },
            confidence: 1,
            resolution: "resolved",
          },
        ],
        total: 1,
      });

      const codeWindow = new CodeWindowComponent(store);
      await codeWindow.loadFile(10);

      const elem = codeWindow.getElement();
      const line5 = elem.querySelector('.code-line[data-line="5"]') as HTMLElement;
      const line3 = elem.querySelector('.code-line[data-line="3"]') as HTMLElement;
      expect(line5).not.toBeNull();
      expect(line3).not.toBeNull();

      // Mock scrollIntoView on lines
      const scrollIntoViewSpies = new Map<HTMLElement, any>();
      elem.querySelectorAll<HTMLElement>(".code-line").forEach((el) => {
        const spy = vi.fn();
        el.scrollIntoView = spy;
        scrollIntoViewSpies.set(el, spy);
      });

      // Mock offsetTop
      Object.defineProperty(line3, "offsetTop", { value: 60, configurable: true });
      Object.defineProperty(line5, "offsetTop", { value: 100, configurable: true });

      // Scroll to line 5
      codeWindow.scrollToLine(5);

      // Verify line 5 is selected
      expect(line5.classList.contains("selected")).toBe(true);

      // Verify target element (line 3, couple lines above line 5) was scrolled into view with instant/auto behavior and block: "start"
      const line3Spy = scrollIntoViewSpies.get(line3);
      expect(line3Spy).toHaveBeenCalledWith(
        expect.objectContaining({ behavior: "auto", block: "start" })
      );
      // Verify line 5 did NOT use smooth scroll or center
      const line5Spy = scrollIntoViewSpies.get(line5);
      expect(line5Spy).not.toHaveBeenCalledWith(
        expect.objectContaining({ behavior: "smooth" })
      );

      // Verify selecting line 1 scrolls to top (0)
      const line1 = elem.querySelector('.code-line[data-line="1"]') as HTMLElement;
      const viewerContainer = elem.querySelector(".code-viewer-container") as HTMLElement;
      viewerContainer.scrollTop = 999;
      codeWindow.scrollToLine(1);
      expect(line1.classList.contains("selected")).toBe(true);
      expect(line5.classList.contains("selected")).toBe(false);
      expect(viewerContainer.scrollTop).toBe(0);

      // Selecting symbol without explicit line navigates via occurrences
      store.selectSymbol(99);
      expect(line5.classList.contains("selected")).toBe(true);
    });

    it("does not scroll code window when user selects a visible symbol or clicks inside code window", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 11,
        workspaceId: 1,
        path: "/src/main.cpp",
        relativePath: "src/main.cpp",
        name: "main.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 100,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 11,
        path: "src/main.cpp",
        content: "int foo = 1;\nint bar = 2;\nint baz = 3;",
        totalSizeBytes: 100,
        totalLines: 3,
        startLine: 0,
        endLine: 2,
        startByte: 0,
        endByte: 100,
        isBinary: false,
        contentHash: "hash123",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 11,
        legend: { tokenTypes: ["variable"] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 11,
        occurrences: [
          {
            id: 2,
            workspaceId: 1,
            fileId: 11,
            symbolId: 201,
            occurrenceKind: "definition",
            name: "bar",
            range: { start: { line: 2, column: 4, byte: 17 }, end: { line: 2, column: 7, byte: 20 } },
            confidence: 1,
            resolution: "resolved",
          },
        ],
        total: 1,
      });

      const onSymbolClick = vi.fn();
      const codeWindow = new CodeWindowComponent(store, { onSymbolClick });
      await codeWindow.loadFile(11);

      const elem = codeWindow.getElement();
      const viewerContainer = elem.querySelector(".code-viewer-container") as HTMLElement;
      viewerContainer.scrollTop = 50;

      const line2 = elem.querySelector('.code-line[data-line="2"]') as HTMLElement;
      expect(line2).not.toBeNull();

      // Spy on scrollIntoView for all lines
      const scrollSpies: any[] = [];
      elem.querySelectorAll<HTMLElement>(".code-line").forEach((el) => {
        const spy = vi.fn();
        el.scrollIntoView = spy;
        scrollSpies.push(spy);
      });

      // Simulate clicking on the token span for "bar" inside line 2
      // Wrap text in a span as highlightSource would
      const tokenSpan = document.createElement("span");
      tokenSpan.className = "tok";
      tokenSpan.textContent = "bar";
      line2.querySelector(".line-content")?.appendChild(tokenSpan);

      tokenSpan.click();

      // Symbol and line are selected
      expect(store.getState().selectedSymbolId).toBe(201);
      expect(store.getState().selectedLine).toBe(2);
      expect(line2.classList.contains("selected")).toBe(true);

      // Verify NO scroll occurred: scrollTop is unchanged and scrollIntoView was NOT called
      expect(viewerContainer.scrollTop).toBe(50);
      for (const spy of scrollSpies) {
        expect(spy).not.toHaveBeenCalled();
      }

      // Also test isLineVisible: mock getBoundingClientRect
      vi.spyOn(viewerContainer, "getBoundingClientRect").mockReturnValue({
        top: 100,
        bottom: 500,
        left: 0,
        right: 800,
        width: 800,
        height: 400,
        x: 0,
        y: 100,
        toJSON: () => {},
      });
      vi.spyOn(line2, "getBoundingClientRect").mockReturnValue({
        top: 150,
        bottom: 170,
        left: 0,
        right: 800,
        width: 800,
        height: 20,
        x: 0,
        y: 150,
        toJSON: () => {},
      });

      expect(codeWindow.isLineVisible(line2)).toBe(true);

      // Calling scrollToLine on an already visible line does NOT scroll
      codeWindow.scrollToLine(2);
      expect(viewerContainer.scrollTop).toBe(50);
      for (const spy of scrollSpies) {
        expect(spy).not.toHaveBeenCalled();
      }
    });

    it("highlights symbols and occurrences when hovered in code window", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 12,
        workspaceId: 1,
        path: "/src/calc.cpp",
        relativePath: "src/calc.cpp",
        name: "calc.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 60,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 12,
        path: "src/calc.cpp",
        content: "int foo = 1;\nint bar = 2;\nreturn foo;",
        totalSizeBytes: 60,
        totalLines: 3,
        startLine: 0,
        endLine: 2,
        startByte: 0,
        endByte: 60,
        isBinary: false,
        contentHash: "hashcalc",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 12,
        legend: { tokenTypes: ["keyword", "variable"] },
        tokens: [
          { line: 0, startColumn: 0, length: 3, tokenType: 0 },
          { line: 0, startColumn: 4, length: 3, tokenType: 1 },
          { line: 1, startColumn: 0, length: 3, tokenType: 0 },
          { line: 1, startColumn: 4, length: 3, tokenType: 1 },
          { line: 2, startColumn: 0, length: 6, tokenType: 0 },
          { line: 2, startColumn: 7, length: 3, tokenType: 1 },
        ],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 12,
        occurrences: [
          {
            id: 1,
            workspaceId: 1,
            fileId: 12,
            symbolId: 50,
            occurrenceKind: "definition",
            name: "foo",
            range: { start: { line: 1, column: 4, byte: 4 }, end: { line: 1, column: 7, byte: 7 } },
            confidence: 1,
            resolution: "resolved",
          },
          {
            id: 2,
            workspaceId: 1,
            fileId: 12,
            symbolId: 60,
            occurrenceKind: "definition",
            name: "bar",
            range: { start: { line: 2, column: 4, byte: 17 }, end: { line: 2, column: 7, byte: 20 } },
            confidence: 1,
            resolution: "resolved",
          },
          {
            id: 3,
            workspaceId: 1,
            fileId: 12,
            symbolId: 50,
            occurrenceKind: "reference",
            name: "foo",
            range: { start: { line: 3, column: 7, byte: 33 }, end: { line: 3, column: 10, byte: 36 } },
            confidence: 1,
            resolution: "resolved",
          },
        ],
        total: 3,
      });

      const codeWindow = new CodeWindowComponent(store);
      await codeWindow.loadFile(12);

      const elem = codeWindow.getElement();
      const fooSpans = elem.querySelectorAll<HTMLElement>('.code-line span[data-symbol-name="foo"]');
      const barSpan = elem.querySelector<HTMLElement>('.code-line span[data-symbol-name="bar"]');

      expect(fooSpans.length).toBe(2);
      expect(barSpan).not.toBeNull();
      expect(fooSpans[0].dataset.symbolId).toBe("50");
      expect(fooSpans[1].dataset.symbolId).toBe("50");
      expect(fooSpans[0].classList.contains("symbol-token")).toBe(true);

      // Hover over the first "foo" symbol
      fooSpans[0].dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));

      // First foo gets symbol-hovered
      expect(fooSpans[0].classList.contains("symbol-hovered")).toBe(true);
      // Other foo occurrence gets symbol-highlighted
      expect(fooSpans[1].classList.contains("symbol-highlighted")).toBe(true);
      // bar is unaffected
      expect(barSpan?.classList.contains("symbol-hovered")).toBe(false);
      expect(barSpan?.classList.contains("symbol-highlighted")).toBe(false);

      // Move mouse away
      fooSpans[0].dispatchEvent(new MouseEvent("mouseout", { bubbles: true }));

      expect(fooSpans[0].classList.contains("symbol-hovered")).toBe(false);
      expect(fooSpans[1].classList.contains("symbol-highlighted")).toBe(false);

      // Hover over "bar"
      barSpan!.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));
      expect(barSpan!.classList.contains("symbol-hovered")).toBe(true);
      expect(fooSpans[0].classList.contains("symbol-highlighted")).toBe(false);
      expect(fooSpans[1].classList.contains("symbol-highlighted")).toBe(false);

      // Mouse leave container clears all
      elem.querySelector(".code-viewer-container")!.dispatchEvent(new MouseEvent("mouseleave"));
      expect(barSpan!.classList.contains("symbol-hovered")).toBe(false);
    });

    it("selects symbol and marks occurrences when clicked in code window", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValueOnce({
        id: 15,
        workspaceId: 1,
        path: "/src/calc.cpp",
        relativePath: "src/calc.cpp",
        name: "calc.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 60,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValueOnce({
        fileId: 15,
        path: "src/calc.cpp",
        content: "int foo = 1;\nint bar = 2;\nreturn foo;",
        totalSizeBytes: 60,
        totalLines: 3,
        startLine: 0,
        endLine: 2,
        startByte: 0,
        endByte: 60,
        isBinary: false,
        contentHash: "hashcalc",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValueOnce({
        fileId: 15,
        legend: { tokenTypes: ["keyword", "variable"] },
        tokens: [
          { line: 0, startColumn: 0, length: 3, tokenType: 0 },
          { line: 0, startColumn: 4, length: 3, tokenType: 1 },
          { line: 1, startColumn: 0, length: 3, tokenType: 0 },
          { line: 1, startColumn: 4, length: 3, tokenType: 1 },
          { line: 2, startColumn: 0, length: 6, tokenType: 0 },
          { line: 2, startColumn: 7, length: 3, tokenType: 1 },
        ],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValueOnce({
        fileId: 15,
        occurrences: [
          {
            id: 1,
            workspaceId: 1,
            fileId: 15,
            symbolId: 50,
            occurrenceKind: "definition",
            name: "foo",
            range: { start: { line: 1, column: 4, byte: 4 }, end: { line: 1, column: 7, byte: 7 } },
            confidence: 1,
            resolution: "resolved",
          },
          {
            id: 2,
            workspaceId: 1,
            fileId: 15,
            symbolId: 60,
            occurrenceKind: "definition",
            name: "bar",
            range: { start: { line: 2, column: 4, byte: 17 }, end: { line: 2, column: 7, byte: 20 } },
            confidence: 1,
            resolution: "resolved",
          },
          {
            id: 3,
            workspaceId: 1,
            fileId: 15,
            symbolId: 50,
            occurrenceKind: "reference",
            name: "foo",
            range: { start: { line: 3, column: 7, byte: 33 }, end: { line: 3, column: 10, byte: 36 } },
            confidence: 1,
            resolution: "resolved",
          },
        ],
        total: 3,
      });

      const onSymbolClick = vi.fn();
      const codeWindow = new CodeWindowComponent(store, { onSymbolClick });
      await codeWindow.loadFile(15);

      const elem = codeWindow.getElement();
      const fooSpans = elem.querySelectorAll<HTMLElement>('.code-line span[data-symbol-name="foo"]');
      const barSpan = elem.querySelector<HTMLElement>('.code-line span[data-symbol-name="bar"]');

      expect(fooSpans.length).toBe(2);
      expect(barSpan).not.toBeNull();

      // Click on the first "foo" symbol
      fooSpans[0].click();

      // Symbol is selected in store
      expect(store.getState().selectedSymbolId).toBe(50);
      expect(store.getState().selectedLine).toBe(1);
      expect(onSymbolClick).toHaveBeenCalledWith(50, 1);

      // In the DOM, fooSpans[0] is marked as symbol-selected, fooSpans[1] is marked as symbol-occurrence-selected
      expect(fooSpans[0].classList.contains("symbol-selected")).toBe(true);
      expect(fooSpans[1].classList.contains("symbol-occurrence-selected")).toBe(true);
      expect(barSpan?.classList.contains("symbol-selected")).toBe(false);

      // Mouseleave should not clear selected symbol
      elem.querySelector(".code-viewer-container")!.dispatchEvent(new MouseEvent("mouseleave"));
      expect(fooSpans[0].classList.contains("symbol-selected")).toBe(true);
      expect(fooSpans[1].classList.contains("symbol-occurrence-selected")).toBe(true);

      // Now click on "bar"
      barSpan!.click();
      expect(store.getState().selectedSymbolId).toBe(60);
      expect(store.getState().selectedLine).toBe(2);
      expect(onSymbolClick).toHaveBeenCalledWith(60, 2);

      // foo selection is cleared, bar is selected
      expect(barSpan!.classList.contains("symbol-selected")).toBe(true);
      expect(fooSpans[0].classList.contains("symbol-selected")).toBe(false);
      expect(fooSpans[1].classList.contains("symbol-occurrence-selected")).toBe(false);

      // External store update also selects the symbol in code window
      store.selectSymbol(50, 3);
      expect(fooSpans[1].classList.contains("symbol-selected")).toBe(true);
      expect(fooSpans[0].classList.contains("symbol-occurrence-selected")).toBe(true);
      expect(barSpan!.classList.contains("symbol-selected")).toBe(false);
    });

    it("highlights referenced symbol in code window when navigating from references panel", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValue({
        id: 2,
        workspaceId: 1,
        path: "/main.cpp",
        relativePath: "main.cpp",
        name: "main.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 80,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValue({
        fileId: 2,
        path: "main.cpp",
        content: "void run() {\n    calculateTotal();\n}",
        totalSizeBytes: 33,
        totalLines: 3,
        startLine: 0,
        endLine: 2,
        startByte: 0,
        endByte: 33,
        isBinary: false,
        contentHash: "hashrun",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValue({
        fileId: 2,
        legend: { tokenTypes: ["keyword", "function"] },
        tokens: [
          { line: 0, startColumn: 0, length: 4, tokenType: 0 },
          { line: 0, startColumn: 5, length: 3, tokenType: 1 },
          { line: 1, startColumn: 4, length: 14, tokenType: 1 },
        ],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValue({
        fileId: 2,
        occurrences: [
          {
            id: 201,
            workspaceId: 1,
            fileId: 2,
            symbolId: 5,
            occurrenceKind: "reference",
            name: "calculateTotal",
            range: { start: { line: 2, column: 4, byte: 17 }, end: { line: 2, column: 18, byte: 31 } },
            confidence: 1,
            resolution: "resolved",
          },
        ],
        total: 1,
      });
      vi.spyOn(api, "getFileOutline").mockResolvedValue({ fileId: 2, outline: [] });
      vi.spyOn(api, "getFileCompileCommand").mockResolvedValue({
        fileId: 2,
        hasCompileCommand: false,
        isAutoDetected: false,
      });

      const codeWindow = new CodeWindowComponent(store);
      // Simulating what ReferencesComponent does when clicking a reference:
      store.selectFile(2, 2, { relativePath: "main.cpp" }, 5, "calculateTotal");
      await codeWindow.loadFile(2);

      const elem = codeWindow.getElement();
      const symbolElem = elem.querySelector('.code-line[data-line="2"] span[data-symbol-name="calculateTotal"]');
      expect(symbolElem).not.toBeNull();
      expect(symbolElem?.classList.contains("symbol-selected")).toBe(true);
    });

    it("wraps and highlights referenced symbol even when syntax highlighter does not generate token spans", async () => {
      vi.spyOn(api, "getFileMetadata").mockResolvedValue({
        id: 3,
        workspaceId: 1,
        path: "/util.cpp",
        relativePath: "util.cpp",
        name: "util.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 50,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValue({
        fileId: 3,
        path: "util.cpp",
        content: "auto res = compute(x);",
        totalSizeBytes: 22,
        totalLines: 1,
        startLine: 0,
        endLine: 0,
        startByte: 0,
        endByte: 22,
        isBinary: false,
        contentHash: "hashcompute",
      });
      // No tree-sitter tokens available
      vi.spyOn(api, "getFileHighlights").mockResolvedValue({ fileId: 3, legend: {}, tokens: [] });
      // Unresolved occurrence with symbolId null
      vi.spyOn(api, "getFileOccurrences").mockResolvedValue({
        fileId: 3,
        occurrences: [
          {
            id: 301,
            workspaceId: 1,
            fileId: 3,
            symbolId: null,
            occurrenceKind: "reference",
            name: "compute",
            range: { start: { line: 1, column: 11, byte: 11 }, end: { line: 1, column: 18, byte: 18 } },
            confidence: 0.8,
            resolution: "unresolved",
          },
        ],
        total: 1,
      });
      vi.spyOn(api, "getFileOutline").mockResolvedValue({ fileId: 3, outline: [] });
      vi.spyOn(api, "getFileCompileCommand").mockResolvedValue({
        fileId: 3,
        hasCompileCommand: false,
        isAutoDetected: false,
      });

      const codeWindow = new CodeWindowComponent(store);
      store.selectFile(3, 1, { relativePath: "util.cpp" }, 99, "compute");
      await codeWindow.loadFile(3);

      const elem = codeWindow.getElement();
      const symbolElem = elem.querySelector('.code-line[data-line="1"] span[data-symbol-name="compute"]');
      expect(symbolElem).not.toBeNull();
      expect(symbolElem?.classList.contains("symbol-selected")).toBe(true);
    });
  });

  describe("OutlineComponent", () => {
    it("renders symbol outline and handles filter", async () => {
      vi.spyOn(api, "getFileOutline").mockResolvedValueOnce({
        fileId: 10,
        outline: [
          {
            id: 1,
            name: "MyClass",
            kind: "class",
            range: { start: { line: 1, column: 0, byte: 0 }, end: { line: 20, column: 1, byte: 100 } },
            children: [
              {
                id: 2,
                name: "doWork",
                kind: "method",
                range: { start: { line: 5, column: 2, byte: 20 }, end: { line: 8, column: 3, byte: 50 } },
                children: [],
              },
            ],
          },
        ],
      });

      const outline = new OutlineComponent(store);
      await outline.loadOutline(10);

      const elem = outline.getElement();
      const items = elem.querySelectorAll(".outline-item");
      expect(items.length).toBe(2);

      // Clicking an item selects symbol and navigates to line
      const methodItem = elem.querySelector('.outline-item[data-symbol-id="2"]') as HTMLElement;
      methodItem.click();
      expect(store.getState().selectedSymbolId).toBe(2);
      expect(store.getState().selectedLine).toBe(5); // 5 (1-based from outline range)
    });

    it("sets tooltip to signature without prepending symbol name", async () => {
      vi.spyOn(api, "getFileOutline").mockResolvedValueOnce({
        fileId: 10,
        outline: [
          {
            id: 1,
            name: "asynclog",
            kind: "namespace",
            signature: "namespace asynclog",
            range: { start: { line: 1, column: 0, byte: 0 }, end: { line: 20, column: 1, byte: 100 } },
            children: [],
          },
        ],
      });

      const outline = new OutlineComponent(store);
      await outline.loadOutline(10);

      const elem = outline.getElement();
      const nameSpan = elem.querySelector(".outline-name") as HTMLElement;
      expect(nameSpan.textContent).toBe("asynclog");
      expect(nameSpan.title).toBe("namespace asynclog");
    });
  });

  describe("ReferencesComponent", () => {
    it("renders symbol detail and references list", async () => {
      vi.spyOn(api, "getSymbolDetail").mockResolvedValueOnce({
        symbol: {
          id: 5,
          workspaceId: 1,
          fileId: 1,
          symbolKey: "sym5",
          name: "calculateTotal",
          kind: "function",
          language: "cpp",
          range: { start: { line: 10, column: 0, byte: 0 }, end: { line: 15, column: 1, byte: 50 } },
          isDefinition: true,
          isDeclaration: false,
          createdAt: "",
        },
        file: {
          id: 1,
          workspaceId: 1,
          path: "/math.cpp",
          relativePath: "math.cpp",
          name: "math.cpp",
          language: "cpp",
          encoding: "utf-8",
          sizeBytes: 100,
          modifiedNs: 0,
          isBinary: false,
          isGenerated: false,
          isDeleted: false,
          createdAt: "",
          updatedAt: "",
        },
        declarations: [],
        callersCount: 2,
        calleesCount: 0,
        referencersCount: 3,
      });

      vi.spyOn(api, "getSymbolReferences").mockResolvedValueOnce({
        items: [
          {
            id: 101,
            referenceKind: "call",
            name: "calculateTotal",
            resolution: "resolved",
            confidence: 1.0,
            range: { start: { line: 42, column: 5, byte: 0 }, end: { line: 42, column: 19, byte: 0 } },
            fileId: 2,
            relativePath: "main.cpp",
          },
        ],
        total: 1,
        limit: 50,
        offset: 0,
        hasMore: false,
      });

      const refs = new ReferencesComponent(store);
      await refs.loadReferences(5);

      const elem = refs.getElement();
      expect(elem.textContent).toContain("calculateTotal");
      expect(elem.textContent).toContain("3 references");
      expect(elem.textContent).toContain("main.cpp:42"); // 42 is 1-based line from range
      // Click on reference navigates to target file and line while preserving symbol
      const refItem = elem.querySelector(".reference-item") as HTMLElement;
      refItem.click();
      expect(store.getState().selectedFileId).toBe(2);
      expect(store.getState().selectedLine).toBe(42);
      expect(store.getState().selectedSymbolId).toBe(5);
      expect(store.getState().selectedSymbolName).toBe("calculateTotal");
    });

    it("loads additional reference pages when hasMore is true", async () => {
      vi.spyOn(api, "getSymbolDetail").mockResolvedValue({
        symbol: {
          id: 5,
          workspaceId: 1,
          fileId: 1,
          symbolKey: "sym5",
          name: "calculateTotal",
          kind: "function",
          language: "cpp",
          range: { start: { line: 10, column: 0, byte: 0 }, end: { line: 15, column: 1, byte: 50 } },
          isDefinition: true,
          isDeclaration: false,
          createdAt: "",
        },
        file: {
          id: 1,
          workspaceId: 1,
          path: "/math.cpp",
          relativePath: "math.cpp",
          name: "math.cpp",
          language: "cpp",
          encoding: "utf-8",
          sizeBytes: 100,
          modifiedNs: 0,
          isBinary: false,
          isGenerated: false,
          isDeleted: false,
          createdAt: "",
          updatedAt: "",
        },
        declarations: [],
        callersCount: 2,
        calleesCount: 0,
        referencersCount: 52,
      });

      // Page 1: 50 items, hasMore: true
      const firstPageItems = Array.from({ length: 50 }, (_, i) => ({
        id: 1000 + i,
        referenceKind: "call",
        name: "calculateTotal",
        resolution: "resolved",
        confidence: 1.0,
        range: { start: { line: i, column: 0, byte: 0 }, end: { line: i, column: 10, byte: 0 } },
        fileId: 2,
        relativePath: "main.cpp",
      }));

      // Page 2: 2 items, hasMore: false
      const secondPageItems = [
        {
          id: 1050,
          referenceKind: "call",
          name: "calculateTotal",
          resolution: "resolved",
          confidence: 1.0,
          range: { start: { line: 50, column: 0, byte: 0 }, end: { line: 50, column: 10, byte: 0 } },
          fileId: 2,
          relativePath: "main.cpp",
        },
        {
          id: 1051,
          referenceKind: "call",
          name: "calculateTotal",
          resolution: "resolved",
          confidence: 1.0,
          range: { start: { line: 51, column: 0, byte: 0 }, end: { line: 51, column: 10, byte: 0 } },
          fileId: 2,
          relativePath: "main.cpp",
        },
      ];

      vi.spyOn(api, "getSymbolReferences").mockImplementation(async (_wsId, _symId, _limit, offset = 0) => {
        if (offset === 0) {
          return {
            items: firstPageItems,
            total: 52,
            limit: 50,
            offset: 0,
            hasMore: true,
          };
        }
        return {
          items: secondPageItems,
          total: 52,
          limit: 50,
          offset: 50,
          hasMore: false,
        };
      });

      const refs = new ReferencesComponent(store);
      await refs.loadReferences(5);

      const elem = refs.getElement();
      expect(elem.querySelectorAll(".reference-item").length).toBe(50);

      // Load more button should be present
      const loadMoreBtn = elem.querySelector(".load-more-refs-btn") as HTMLButtonElement;
      expect(loadMoreBtn).not.toBeNull();
      expect(loadMoreBtn.textContent).toContain("50 of 52");

      // Click to load next page
      await refs.loadMoreReferences();

      // Now all 52 items are present and button is removed
      expect(elem.querySelectorAll(".reference-item").length).toBe(52);
      expect(elem.querySelector(".load-more-refs-btn")).toBeNull();
    });

    it("ignores a pending reference page after the linked context is invalidated", async () => {
      let resolvePage!: (value: any) => void;
      vi.spyOn(api, "getSymbolDetail").mockResolvedValue(null as any);
      vi.spyOn(api, "getSymbolReferences").mockImplementation((_wsId, _symId, _limit, offset = 0) => {
        if (offset === 0) {
          return Promise.resolve({
            items: [{ id: 1, referenceKind: "call", name: "Widget", resolution: "resolved", confidence: 1,
              range: { start: { line: 1, column: 0, byte: 0 }, end: { line: 1, column: 1, byte: 0 } },
              fileId: 2, relativePath: "use.cpp" }],
            total: 2, limit: 1, offset: 0, hasMore: true,
          });
        }
        return new Promise((resolve) => { resolvePage = resolve; });
      });

      const panel = new ReferencesComponent(store);
      await panel.loadReferences(9);
      const pendingPage = panel.loadMoreReferences();
      await panel.loadReferences(null);
      resolvePage({
        items: [{ id: 2, referenceKind: "call", name: "Widget", resolution: "resolved", confidence: 1,
          range: { start: { line: 2, column: 0, byte: 0 }, end: { line: 2, column: 1, byte: 0 } },
          fileId: 3, relativePath: "stale.cpp" }],
        total: 2, limit: 1, offset: 1, hasMore: false,
      });
      await pendingPage;

      expect(panel.getElement().querySelectorAll(".reference-item")).toHaveLength(0);
      expect(panel.getElement().textContent).toContain("No symbol selected");
    });

    it("renders link to declaration and navigates to explicit declaration when present", async () => {
      vi.spyOn(api, "getSymbolDetail").mockResolvedValueOnce({
        symbol: {
          id: 5,
          workspaceId: 1,
          fileId: 1,
          symbolKey: "sym5",
          name: "calculateTotal",
          kind: "function",
          language: "cpp",
          range: { start: { line: 10, column: 0, byte: 0 }, end: { line: 15, column: 1, byte: 50 } },
          isDefinition: true,
          isDeclaration: false,
          createdAt: "",
        },
        file: {
          id: 1,
          workspaceId: 1,
          path: "/math.cpp",
          relativePath: "math.cpp",
          name: "math.cpp",
          language: "cpp",
          encoding: "utf-8",
          sizeBytes: 100,
          modifiedNs: 0,
          isBinary: false,
          isGenerated: false,
          isDeleted: false,
          createdAt: "",
          updatedAt: "",
        },
        declarations: [
          {
            id: 20,
            workspaceId: 1,
            fileId: 3,
            symbolKey: "sym5",
            name: "calculateTotal",
            kind: "function",
            language: "cpp",
            range: { start: { line: 5, column: 0, byte: 0 }, end: { line: 5, column: 30, byte: 30 } },
            isDefinition: false,
            isDeclaration: true,
            createdAt: "",
            relativePath: "math.h",
          },
        ],
        callersCount: 0,
        calleesCount: 0,
        referencersCount: 0,
      });

      vi.spyOn(api, "getSymbolReferences").mockResolvedValueOnce({
        items: [],
        total: 0,
        limit: 50,
        offset: 0,
        hasMore: false,
      });

      const refs = new ReferencesComponent(store);
      await refs.loadReferences(5);

      const elem = refs.getElement();
      const declDiv = elem.querySelector(".symbol-declaration") as HTMLElement;
      expect(declDiv).not.toBeNull();
      expect(declDiv.textContent).toContain("Declaration:");

      const declLink = elem.querySelector(".declaration-link") as HTMLElement;
      expect(declLink).not.toBeNull();
      expect(declLink.textContent).toBe("math.h:5");

      declLink.click();
      expect(store.getState().selectedFileId).toBe(3);
      expect(store.getState().selectedLine).toBe(5);
      expect(store.getState().selectedSymbolId).toBe(20);
      expect(store.getState().selectedSymbolName).toBe("calculateTotal");
    });

    it("falls back to symbol own file and line when no explicit declaration in declarations", async () => {
      vi.spyOn(api, "getSymbolDetail").mockResolvedValueOnce({
        symbol: {
          id: 8,
          workspaceId: 1,
          fileId: 4,
          symbolKey: "sym8",
          name: "helperFunc",
          kind: "function",
          language: "cpp",
          range: { start: { line: 25, column: 0, byte: 0 }, end: { line: 30, column: 1, byte: 50 } },
          isDefinition: true,
          isDeclaration: false,
          createdAt: "",
        },
        file: {
          id: 4,
          workspaceId: 1,
          path: "/helper.cpp",
          relativePath: "helper.cpp",
          name: "helper.cpp",
          language: "cpp",
          encoding: "utf-8",
          sizeBytes: 100,
          modifiedNs: 0,
          isBinary: false,
          isGenerated: false,
          isDeleted: false,
          createdAt: "",
          updatedAt: "",
        },
        declarations: [],
        callersCount: 0,
        calleesCount: 0,
        referencersCount: 0,
      });

      vi.spyOn(api, "getSymbolReferences").mockResolvedValueOnce({
        items: [],
        total: 0,
        limit: 50,
        offset: 0,
        hasMore: false,
      });

      const refs = new ReferencesComponent(store);
      await refs.loadReferences(8);

      const elem = refs.getElement();
      const declLink = elem.querySelector(".declaration-link") as HTMLElement;
      expect(declLink).not.toBeNull();
      expect(declLink.textContent).toBe("helper.cpp:25");

      declLink.click();
      expect(store.getState().selectedFileId).toBe(4);
      expect(store.getState().selectedLine).toBe(25);
      expect(store.getState().selectedSymbolId).toBe(8);
      expect(store.getState().selectedSymbolName).toBe("helperFunc");
    });
  });

  describe("DiagnosticsComponent", () => {
    it("renders diagnostics and handles click", () => {
      const diagsComp = new DiagnosticsComponent(store);
      diagsComp.setDiagnostics([
        {
          fileId: 1,
          filePath: "main.cpp",
          line: 14,
          column: 5,
          severity: "error",
          message: "Undefined variable 'x'",
        },
      ]);

      const elem = diagsComp.getElement();
      expect(elem.textContent).toContain("[ERROR]");
      expect(elem.textContent).toContain("Undefined variable 'x'");
      expect(elem.textContent).toContain("main.cpp:14:5");

      const item = elem.querySelector(".diag-item") as HTMLElement;
      item.click();
      expect(store.getState().selectedFileId).toBe(1);
      expect(store.getState().selectedLine).toBe(14);
    });
  });

  describe("CodeWindowComponent stale response handling", () => {
    it("discards stale async response if newer load started", async () => {
      let resolveFirst!: (value: any) => void;
      const firstPromise = new Promise((resolve) => {
        resolveFirst = resolve;
      });

      vi.spyOn(api, "getFileMetadata").mockImplementation(async (_wsId, fileId) => ({
        id: fileId,
        workspaceId: 1,
        path: fileId === 1 ? "/first.cpp" : "/second.cpp",
        relativePath: fileId === 1 ? "first.cpp" : "second.cpp",
        name: fileId === 1 ? "first.cpp" : "second.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 20,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      }));

      vi.spyOn(api, "getFileContent").mockImplementation(async (_wsId, fileId) => {
        if (fileId === 1) {
          await firstPromise;
          return {
            fileId: 1,
            path: "first.cpp",
            content: "first content",
            totalSizeBytes: 20,
            totalLines: 1,
            startLine: 0,
            endLine: 0,
            startByte: 0,
            endByte: 20,
            isBinary: false,
            contentHash: "h1",
          };
        }
        return {
          fileId: 2,
          path: "second.cpp",
          content: "second content",
          totalSizeBytes: 20,
          totalLines: 1,
          startLine: 0,
          endLine: 0,
          startByte: 0,
          endByte: 20,
          isBinary: false,
          contentHash: "h2",
        };
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValue({
        fileId: 1,
        legend: { tokenTypes: [] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValue({
        fileId: 1,
        occurrences: [],
        total: 0,
      });

      const codeWindow = new CodeWindowComponent(store);

      // Start loading file 1 (slow)
      const p1 = codeWindow.loadFile(1);

      // Immediately start loading file 2 (fast)
      const p2 = codeWindow.loadFile(2);
      await p2;

      const elem = codeWindow.getElement();
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("second.cpp");

      // Now slow file 1 resolves
      resolveFirst(null);
      await p1;

      // File 2 must STILL be shown, NOT overwritten by file 1!
      expect(elem.querySelector(".file-path-text")?.textContent).toBe("second.cpp");
    });
  });

  describe("AppComponent security & route reconciliation", () => {
    let container: HTMLElement;

    beforeEach(() => {
      container = document.createElement("div");
      document.body.appendChild(container);
    });

    it("escapes search results content to prevent XSS markup injection", async () => {
      const maliciousSnippet = `<img src="x" onerror="alert('xss')" />`;
      const maliciousName = `<svg onload="alert('hack')">`;

      vi.spyOn(api, "getWorkspaces").mockResolvedValue({ workspaces: [], total: 0 });
      vi.spyOn(api, "getWorkspaceStatus").mockResolvedValue({
        workspaceId: 1,
        status: "idle",
        revision: 1,
        fileCount: 0,
        symbolCount: 0,
        diagnosticCounts: { total: 0, errors: 0, warnings: 0, info: 0 },
      });
      vi.spyOn(api, "searchSource").mockResolvedValue({
        items: [
          {
            fileId: 1,
            relativePath: "malicious.cpp",
            snippet: maliciousSnippet,
            rank: 1.0,
            origin: "library",
            ownerWorkspaceId: 20,
            ownerWorkspaceName: "SDK",
            targetFramework: "net8.0",
          },
        ],
        total: 1,
        limit: 50,
        offset: 0,
        hasMore: false,
      });

      const app = new AppComponent(container, store);
      app.openSearch();
      const input = document.querySelector(".search-modal-input") as HTMLInputElement;
      input.value = "test";
      await (app as any).performSearch();

      const modal = document.querySelector(".search-modal-backdrop") as HTMLElement;
      expect(modal).not.toBeNull();
      // Ensure no <img> tag was parsed/inserted
      expect(modal.querySelector("img")).toBeNull();
      expect(modal.textContent).toContain(maliciousSnippet);
      expect(modal.textContent).toContain("SDK");

      // Now test symbol search escaping
      vi.spyOn(api, "searchSymbols").mockResolvedValue({
        items: [
          {
            id: 1,
            fileId: 1,
            relativePath: "malicious.cpp",
            name: maliciousName,
            kind: "function",
            rank: 1.0,
            origin: "library",
            ownerWorkspaceId: 20,
            ownerWorkspaceName: "SDK",
            targetFramework: "net8.0",
          },
        ],
        total: 1,
        limit: 50,
        offset: 0,
        hasMore: false,
      });

      const modeSelect = document.querySelector(".search-modal-mode") as HTMLSelectElement;
      modeSelect.value = "symbol";
      await (app as any).performSearch();

      // Ensure no <svg> tag was parsed/inserted
      expect(modal.querySelector("svg.hack-tag")).toBeNull();
      expect(modal.querySelectorAll("svg").length).toBe(0);
      expect(modal.textContent).toContain(maliciousName);
      expect(modal.textContent).toContain("SDK");

      app.closeSearch();
    });

    it("reconciles route and loads tree, status, file, outline, and diagnostics", async () => {
      store.setState({ workspaceId: 1, selectedFileId: 5, selectedLine: 2 });

      vi.spyOn(api, "getWorkspaces").mockResolvedValue({
        workspaces: [{ id: 1, name: "ws1", rootPath: "/ws1", revision: 1, status: "idle", createdAt: "", updatedAt: "" }],
        total: 1,
      });
      vi.spyOn(api, "getTree").mockResolvedValue({
        workspaceId: 1,
        path: "",
        entries: [{ name: "main.cpp", path: "main.cpp", type: "file", fileId: 5, sizeBytes: 50, isBinary: false, language: "cpp" }],
      });
      vi.spyOn(api, "getWorkspaceStatus").mockResolvedValue({
        workspaceId: 1,
        status: "idle",
        revision: 1,
        fileCount: 1,
        symbolCount: 2,
        diagnosticCounts: { total: 1, errors: 1, warnings: 0, info: 0 },
      });
      vi.spyOn(api, "getFileMetadata").mockResolvedValue({
        id: 5,
        workspaceId: 1,
        path: "/main.cpp",
        relativePath: "main.cpp",
        name: "main.cpp",
        language: "cpp",
        encoding: "utf-8",
        sizeBytes: 50,
        modifiedNs: 0,
        isBinary: false,
        isGenerated: false,
        isDeleted: false,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getFileContent").mockResolvedValue({
        fileId: 5,
        path: "main.cpp",
        content: "line 1\nline 2",
        totalSizeBytes: 50,
        totalLines: 2,
        startLine: 0,
        endLine: 1,
        startByte: 0,
        endByte: 50,
        isBinary: false,
        contentHash: "h5",
      });
      vi.spyOn(api, "getFileHighlights").mockResolvedValue({
        fileId: 5,
        legend: { tokenTypes: [] },
        tokens: [],
      });
      vi.spyOn(api, "getFileOccurrences").mockResolvedValue({
        fileId: 5,
        occurrences: [],
        total: 0,
      });
      vi.spyOn(api, "getFileOutline").mockResolvedValue({
        fileId: 5,
        outline: [],
      });
      vi.spyOn(api, "getWorkspaceDiagnostics").mockResolvedValue({
        workspaceId: 1,
        diagnostics: [
          {
            id: 10,
            workspaceId: 1,
            fileId: 5,
            relativePath: "main.cpp",
            severity: "error",
            message: "syntax error",
            line: 1, // 0-based from API (represents line 2 in UI)
          },
        ],
        total: 1,
      });
      vi.spyOn(api, "getFileDiagnostics").mockResolvedValue({
        fileId: 5,
        diagnostics: [
          {
            id: 10,
            workspaceId: 1,
            fileId: 5,
            relativePath: "main.cpp",
            severity: "error",
            message: "syntax error",
            line: 1,
          },
        ],
        total: 1,
      });

      const app = new AppComponent(container, store);
      await app.reconcile();

      // Diagnostics loaded into inspector pill
      const pill = container.querySelector(".diag-count-pill") as HTMLElement;
      expect(pill.textContent).toBe("1");
      expect(pill.style.display).toBe("inline-block");

      // Diagnostics item loaded in inspector
      const diagItem = container.querySelector(".diag-item") as HTMLElement;
      expect(diagItem).not.toBeNull();
      expect(diagItem.textContent).toContain("syntax error");

      // Code window gutter has marker on line 2 (converted from API line 1)
      const line2 = container.querySelector('.code-line[data-line="2"]') as HTMLElement;
      expect(line2).not.toBeNull();
      expect(line2.classList.contains("has-error")).toBe(true);
      expect(line2.querySelector(".line-diag-marker")).not.toBeNull();
    });
  });

  describe("parseCommandPreview", () => {
    it("extracts compiler, standard, defines, and includes", () => {
      const parsed = parseCommandPreview("clang++ -std=c++17 -DFOO=1 -DBAR -Iinclude -I /usr/local/include -O2");
      expect(parsed.compiler).toBe("clang++");
      expect(parsed.standard).toBe("c++17");
      expect(parsed.defines).toEqual(["FOO=1", "BAR"]);
      expect(parsed.includes).toEqual(["include", "/usr/local/include"]);
    });

    it("handles GNU style --std=", () => {
      const parsed = parseCommandPreview("gcc --std=gnu11 -D_GNU_SOURCE");
      expect(parsed.compiler).toBe("gcc");
      expect(parsed.standard).toBe("gnu11");
      expect(parsed.defines).toEqual(["_GNU_SOURCE"]);
    });

    it("handles command without compiler or standard", () => {
      const parsed = parseCommandPreview("-Isrc -DVERSION=2");
      expect(parsed.compiler).toBeUndefined();
      expect(parsed.standard).toBeUndefined();
      expect(parsed.includes).toEqual(["src"]);
      expect(parsed.defines).toEqual(["VERSION=2"]);
    });
  });

  describe("WorkspaceSettingsModal", () => {
    let modal: WorkspaceSettingsModal;

    beforeEach(() => {
      modal = new WorkspaceSettingsModal(store);
    });

    it("loads workspace settings and populates defaultCompileCommand", async () => {
      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        compileCommandsPath: "build/compile_commands.json",
        defaultCompileCommand: "clang -Iinclude -DDEBUG=1 -std=c11",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: "build/compile_commands.json",
        effectivePath: "/path/to/project/build/compile_commands.json",
        exists: true,
        isAutoDetected: false,
        totalCommands: 15,
        defaultCompileCommand: "clang -Iinclude -DDEBUG=1 -std=c11",
      });

      await modal.open(1);

      const elem = modal.getElement();
      const defaultCmdInput = elem.querySelector(".ws-default-cmd-input") as HTMLInputElement;
      expect(defaultCmdInput.value).toBe("clang -Iinclude -DDEBUG=1 -std=c11");

      const previewBox = elem.querySelector(".ws-default-cmd-preview") as HTMLElement;
      expect(previewBox.style.display).not.toBe("none");
      expect(previewBox.textContent).toContain("C11");
      expect(previewBox.textContent).toContain("1 define");
      expect(previewBox.textContent).toContain("1 include dir");

      modal.close();
    });

    it("updates live preview when typing into defaultCompileCommand input", async () => {
      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });

      await modal.open(1);

      const elem = modal.getElement();
      const defaultCmdInput = elem.querySelector(".ws-default-cmd-input") as HTMLInputElement;
      const previewBox = elem.querySelector(".ws-default-cmd-preview") as HTMLElement;

      expect(defaultCmdInput.value).toBe("");
      expect(previewBox.style.display).toBe("none");

      defaultCmdInput.value = "gcc -std=c99 -DFOO -DBAR -Iinc1 -Iinc2";
      defaultCmdInput.dispatchEvent(new Event("input"));

      expect(previewBox.style.display).toBe("block");
      expect(previewBox.textContent).toContain("C99");
      expect(previewBox.textContent).toContain("2 defines");
      expect(previewBox.textContent).toContain("2 include dirs");

      modal.close();
    });

    it("saves workspace with defaultCompileCommand via Save button", async () => {
      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });
      const updateSpy = vi.spyOn(api, "updateWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 2,
        createdAt: "",
        updatedAt: "",
      });

      await modal.open(1);

      const elem = modal.getElement();
      const defaultCmdInput = elem.querySelector(".ws-default-cmd-input") as HTMLInputElement;
      defaultCmdInput.value = "clang -Isrc -DTEST=1";

      const saveBtn = elem.querySelector(".save-settings-btn") as HTMLButtonElement;
      saveBtn.click();

      expect(updateSpy).toHaveBeenCalledWith(1, {
        name: "My Project",
        compileCommandsPath: null,
        defaultCompileCommand: "clang -Isrc -DTEST=1",
      });
    });

    it("focuses defaultCompileCommand input when focusDefaultCmd is true", async () => {
      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });

      const elem = modal.getElement();
      const defaultCmdInput = elem.querySelector(".ws-default-cmd-input") as HTMLInputElement;
      const focusSpy = vi.spyOn(defaultCmdInput, "focus");

      await modal.open(1, true);

      await new Promise((resolve) => setTimeout(resolve, 80));
      expect(focusSpy).toHaveBeenCalled();
      modal.close();
    });

    it("deletes workspace via api.deleteWorkspace when confirmed", async () => {
      const onDeleted = vi.fn();
      const modalWithDelete = new WorkspaceSettingsModal(store, { onDeleted });

      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });
      const deleteSpy = vi.spyOn(api, "deleteWorkspace").mockResolvedValueOnce({
        status: "deleted",
        id: 1,
      });
      const confirmSpy = vi.spyOn(window, "confirm").mockReturnValue(true);

      await modalWithDelete.open(1);

      const elem = modalWithDelete.getElement();
      const deleteBtn = elem.querySelector(".delete-ws-btn") as HTMLButtonElement;
      expect(deleteBtn).not.toBeNull();
      deleteBtn.click();

      await new Promise((resolve) => setTimeout(resolve, 10));
      expect(confirmSpy).toHaveBeenCalled();
      expect(deleteSpy).toHaveBeenCalledWith(1);
      expect(onDeleted).toHaveBeenCalledWith(1);
      expect(elem.style.display).toBe("none");
    });

    it("cancels workspace deletion if confirmation is rejected", async () => {
      const onDeleted = vi.fn();
      const modalWithDelete = new WorkspaceSettingsModal(store, { onDeleted });

      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });
      const deleteSpy = vi.spyOn(api, "deleteWorkspace");
      vi.spyOn(window, "confirm").mockReturnValue(false);

      await modalWithDelete.open(1);

      const elem = modalWithDelete.getElement();
      const deleteBtn = elem.querySelector(".delete-ws-btn") as HTMLButtonElement;
      deleteBtn.click();

      expect(deleteSpy).not.toHaveBeenCalled();
      expect(onDeleted).not.toHaveBeenCalled();
      modalWithDelete.close();
    });

  });

  describe("WorkspaceCompileCommandsModal", () => {
    it("calls open with focusDefaultCmd=true", async () => {
      const modal = new WorkspaceCompileCommandsModal(store);
      vi.spyOn(api, "getWorkspace").mockResolvedValueOnce({
        id: 1,
        name: "My Project",
        rootPath: "/path/to/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      });
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValueOnce({
        configuredPath: null,
        effectivePath: null,
        exists: false,
        isAutoDetected: false,
        totalCommands: 0,
      });

      const defaultCmdInput = modal.getElement().querySelector(".ws-default-cmd-input") as HTMLInputElement;
      const focusSpy = vi.spyOn(defaultCmdInput, "focus");

      await modal.open(1);
      await new Promise((resolve) => setTimeout(resolve, 80));
      expect(focusSpy).toHaveBeenCalled();
      modal.close();
    });
  });

  describe("CompileCommandComponent", () => {
    it("renders per-file compile command with precedence banner and badge", () => {
      const onConfigureWorkspaceDefault = vi.fn();
      const comp = new CompileCommandComponent(store, {
        onConfigureWorkspaceDefault,
      });

      comp.setCompileCommand({
        fileId: 10,
        hasCompileCommand: true,
        isAutoDetected: false,
        isWorkspaceDefault: false,
        databasePath: "compile_commands.json",
        compileCommand: {
          directory: "/project",
          file: "src/main.c",
          arguments: ["clang", "-Iinclude", "-DDEBUG=1", "-std=c11", "-c", "src/main.c"],
          output: "main.o",
          languageStandard: "c11",
          defines: ["DEBUG=1"],
          includeDirs: ["/project/include"],
        },
      });

      const elem = comp.getElement();
      // Badge should have file-entry class and 'configured' text
      const badge = elem.querySelector(".cc-source-badge");
      expect(badge?.classList.contains("file-entry")).toBe(true);
      expect(badge?.textContent).toBe("configured");

      // Precedence banner should explain individual override
      const banner = elem.querySelector(".cc-precedence-banner");
      expect(banner?.textContent).toContain("individual file command");
      expect(banner?.textContent).toContain("overrides workspace default");

      // Clicking Change Default button invokes callback
      const editBtn = banner?.querySelector(".edit-ws-default-btn") as HTMLButtonElement;
      expect(editBtn).not.toBeNull();
      editBtn.click();
      expect(onConfigureWorkspaceDefault).toHaveBeenCalledTimes(1);

      // Defines and includes rendered
      expect(elem.textContent).toContain("DEBUG=1");
      expect(elem.textContent).toContain("/project/include");
    });

    it("renders workspace default compile command with precedence banner and badge", () => {
      const onConfigureWorkspaceDefault = vi.fn();
      const comp = new CompileCommandComponent(store, {
        onConfigureWorkspaceDefault,
      });

      comp.setCompileCommand({
        fileId: 20,
        hasCompileCommand: true,
        isAutoDetected: false,
        isWorkspaceDefault: true,
        databasePath: null,
        compileCommand: {
          directory: "/project",
          file: "src/other.c",
          arguments: ["clang", "-Icommon", "-DAPP_VERSION=2", "-std=c17"],
          output: null,
          languageStandard: "c17",
          defines: ["APP_VERSION=2"],
          includeDirs: ["/project/common"],
        },
      });

      const elem = comp.getElement();
      // Badge should have ws-default class and 'workspace default' text
      const badge = elem.querySelector(".cc-source-badge");
      expect(badge?.classList.contains("ws-default")).toBe(true);
      expect(badge?.textContent).toBe("workspace default");

      // Precedence banner should explain fallback
      const banner = elem.querySelector(".cc-precedence-banner");
      expect(banner?.textContent).toContain("workspace default");
      expect(banner?.textContent).toContain("no entry in compile_commands.json");

      // Clicking Change Default button invokes callback
      const editBtn = banner?.querySelector(".edit-ws-default-btn") as HTMLButtonElement;
      expect(editBtn).not.toBeNull();
      editBtn.click();
      expect(onConfigureWorkspaceDefault).toHaveBeenCalledTimes(1);
    });

    it("renders empty state with Change Workspace Default Command button and triggers callback", () => {
      const onConfigureWorkspaceDefault = vi.fn();
      const comp = new CompileCommandComponent(store, {
        onConfigureWorkspaceDefault,
      });

      comp.setCompileCommand({
        fileId: 30,
        hasCompileCommand: false,
        isAutoDetected: false,
      });

      const elem = comp.getElement();
      expect(elem.querySelector(".empty-state-title")?.textContent).toBe("No compile command");

      const defaultBtn = elem.querySelector(".configure-default-cmd-btn") as HTMLButtonElement;
      expect(defaultBtn).not.toBeNull();
      expect(defaultBtn.textContent).toContain("Change Workspace Default Command");

      defaultBtn.click();
      expect(onConfigureWorkspaceDefault).toHaveBeenCalledTimes(1);
    });
  });

  describe("AddWorkspaceModal", () => {
    it("opens and validates empty root path on submit", () => {
      const modal = new AddWorkspaceModal();
      modal.open();

      const elem = modal.getElement();
      expect(elem.style.display).toBe("flex");

      const submitBtn = elem.querySelector(".submit-add-btn") as HTMLButtonElement;
      submitBtn.click();

      const banner = elem.querySelector(".add-ws-banner") as HTMLElement;
      expect(banner.style.display).toBe("flex");
      expect(banner.textContent).toContain("specify a root directory");

      modal.close();
      expect(elem.style.display).toBe("none");
    });

    it("submits CreateWorkspaceRequest and calls onCreated callback", async () => {
      const onCreated = vi.fn();
      const modal = new AddWorkspaceModal({ onCreated });
      modal.open();

      const elem = modal.getElement();
      const rootInput = elem.querySelector(".ws-add-root") as HTMLInputElement;
      const nameInput = elem.querySelector(".ws-add-name") as HTMLInputElement;
      const cdbInput = elem.querySelector(".ws-add-cdb") as HTMLInputElement;
      const defaultCmdInput = elem.querySelector(".ws-add-default-cmd") as HTMLInputElement;
      const includesInput = elem.querySelector(".ws-add-includes") as HTMLInputElement;
      const excludesInput = elem.querySelector(".ws-add-excludes") as HTMLInputElement;

      rootInput.value = "/path/to/my-repo";
      nameInput.value = "My Repo";
      cdbInput.value = "build/compile_commands.json";
      defaultCmdInput.value = "clang++ -std=c++20";
      includesInput.value = "src/**, include/**";
      excludesInput.value = "build/**";

      const mockCreated = {
        id: 10,
        name: "My Repo",
        rootPath: "/path/to/my-repo",
        compileCommandsPath: "build/compile_commands.json",
        defaultCompileCommand: "clang++ -std=c++20",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      };

      const createSpy = vi.spyOn(api, "createWorkspace").mockResolvedValueOnce(mockCreated);

      const submitBtn = elem.querySelector(".submit-add-btn") as HTMLButtonElement;
      submitBtn.click();

      await new Promise((resolve) => setTimeout(resolve, 10));

      expect(createSpy).toHaveBeenCalledWith({
        rootPath: "/path/to/my-repo",
        name: "My Repo",
        compileCommandsPath: "build/compile_commands.json",
        defaultCompileCommand: "clang++ -std=c++20",
        includePatterns: ["src/**", "include/**"],
        excludePatterns: ["build/**"],
      });

      expect(onCreated).toHaveBeenCalledWith(mockCreated);
      expect(elem.style.display).toBe("none");
    });

    it("triggers indexing when Add & Index Now is clicked", async () => {
      const onCreated = vi.fn();
      const modal = new AddWorkspaceModal({ onCreated });
      modal.open();

      const elem = modal.getElement();
      const rootInput = elem.querySelector(".ws-add-root") as HTMLInputElement;
      rootInput.value = "/my/project";

      const mockCreated = {
        id: 20,
        name: "project",
        rootPath: "/my/project",
        status: "idle",
        revision: 1,
        createdAt: "",
        updatedAt: "",
      };

      vi.spyOn(api, "createWorkspace").mockResolvedValueOnce(mockCreated);
      const indexSpy = vi.spyOn(api, "triggerIndexing").mockResolvedValueOnce({
        id: 1,
        workspaceId: 20,
        jobType: "incremental",
        status: "queued",
        queuedAt: "",
        filesTotal: 0,
        filesProcessed: 0,
        filesSkipped: 0,
        errorCount: 0,
        warningCount: 0,
      });

      const submitIndexBtn = elem.querySelector(".submit-add-index-btn") as HTMLButtonElement;
      submitIndexBtn.click();

      await new Promise((resolve) => setTimeout(resolve, 10));

      expect(indexSpy).toHaveBeenCalledWith(20, "incremental", false);
      expect(onCreated).toHaveBeenCalledWith(mockCreated);
    });

    it("shows error banner when createWorkspace fails", async () => {
      const modal = new AddWorkspaceModal();
      modal.open();

      const elem = modal.getElement();
      const rootInput = elem.querySelector(".ws-add-root") as HTMLInputElement;
      rootInput.value = "/nonexistent/path";

      vi.spyOn(api, "createWorkspace").mockRejectedValueOnce(new Error("Path does not exist"));

      const submitBtn = elem.querySelector(".submit-add-btn") as HTMLButtonElement;
      submitBtn.click();

      await new Promise((resolve) => setTimeout(resolve, 10));

      const banner = elem.querySelector(".add-ws-banner") as HTMLElement;
      expect(banner.style.display).toBe("flex");
      expect(banner.textContent).toContain("Path does not exist");
      modal.close();
    });
  });

  describe("Linked workspaces", () => {
    it("links an existing workspace and unlinks it without indexing files", async () => {
      const a = { id: 1, name: "A", rootPath: "/a", revision: 0, status: "idle", createdAt: "", updatedAt: "" };
      const b = { ...a, id: 2, name: "B", rootPath: "/b" };
      vi.spyOn(api, "getWorkspace").mockResolvedValue(a);
      vi.spyOn(api, "getWorkspaceCompileCommands").mockRejectedValue(new Error("no commands"));
      vi.spyOn(api, "getWorkspaces").mockResolvedValue({ workspaces: [a, b], total: 2 });
      const links = vi.spyOn(api, "getWorkspaceLinks").mockResolvedValue({ workspaces: [], total: 0 });
      const link = vi.spyOn(api, "linkWorkspace").mockResolvedValue(b);
      const unlink = vi.spyOn(api, "unlinkWorkspace").mockResolvedValue({ status: "unlinked" });
      const index = vi.spyOn(api, "triggerIndexing");
      const changed = vi.fn();
      const modal = new WorkspaceSettingsModal(store, { onLinksChanged: changed });
      await modal.open(1);
      const el = modal.getElement();
      (el.querySelector(".link-workspace-btn") as HTMLElement).click();
      const select = el.querySelector(".available-workspaces-select") as HTMLSelectElement;
      expect(select.options.length).toBe(1);
      expect(select.value).toBe("2");
      links.mockResolvedValue({ workspaces: [b], total: 1 });
      (el.querySelector(".confirm-attach-btn") as HTMLElement).click();
      await vi.waitFor(() => expect(changed).toHaveBeenCalledTimes(1));
      expect(link).toHaveBeenCalledWith(1, 2);
      expect(el.textContent).toContain("B");
      links.mockResolvedValue({ workspaces: [], total: 0 });
      (el.querySelector(".unlink-workspace-btn") as HTMLElement).click();
      await vi.waitFor(() => expect(changed).toHaveBeenCalledTimes(2));
      expect(unlink).toHaveBeenCalledWith(1, 2);
      expect(index).not.toHaveBeenCalled();
      modal.close();
    });

    it("keeps a pending link action bound to the workspace view where it started", async () => {
      const a = { id: 1, name: "A", rootPath: "/a", revision: 0, status: "idle", createdAt: "", updatedAt: "" };
      const b = { ...a, id: 2, name: "B", rootPath: "/b" };
      let resolveLink!: (value: typeof b) => void;
      vi.spyOn(api, "getWorkspace").mockImplementation(async (id) => id === 1 ? a : b);
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValue(null as any);
      vi.spyOn(api, "getWorkspaces").mockResolvedValue({ workspaces: [a, b], total: 2 });
      const getLinks = vi.spyOn(api, "getWorkspaceLinks").mockResolvedValue({ workspaces: [], total: 0 });
      vi.spyOn(api, "linkWorkspace").mockReturnValue(new Promise((resolve) => { resolveLink = resolve; }));
      const changed = vi.fn();
      const modal = new WorkspaceSettingsModal(store, { onLinksChanged: changed });

      await modal.open(1);
      const element = modal.getElement();
      (element.querySelector(".link-workspace-btn") as HTMLButtonElement).click();
      (element.querySelector(".confirm-attach-btn") as HTMLButtonElement).click();
      await vi.waitFor(() => expect(api.linkWorkspace).toHaveBeenCalledWith(1, 2));

      await modal.open(2);
      const callsAfterOpeningB = getLinks.mock.calls.length;
      resolveLink(b);
      await vi.waitFor(() => expect(changed).toHaveBeenCalledWith(1));
      await new Promise((resolve) => setTimeout(resolve, 0));

      expect(getLinks).toHaveBeenCalledTimes(callsAfterOpeningB);
      expect(element.querySelector(".ws-root-path")?.textContent).toBe("/b");
      expect(element.style.display).toBe("flex");
      modal.close();
    });

    it("keeps save and reindex bound to the workspace where the action started", async () => {
      const a = { id: 1, name: "A", rootPath: "/a", revision: 0, status: "idle", createdAt: "", updatedAt: "" };
      const b = { ...a, id: 2, name: "B", rootPath: "/b" };
      let resolveSave!: (value: typeof a) => void;
      vi.spyOn(api, "getWorkspace").mockImplementation(async (id) => id === 1 ? a : b);
      vi.spyOn(api, "getWorkspaceCompileCommands").mockResolvedValue(null as any);
      vi.spyOn(api, "getWorkspaceLinks").mockResolvedValue({ workspaces: [], total: 0 });
      vi.spyOn(api, "getWorkspaces").mockResolvedValue({ workspaces: [a, b], total: 2 });
      const update = vi.spyOn(api, "updateWorkspace").mockReturnValue(new Promise((resolve) => { resolveSave = resolve; }));
      const index = vi.spyOn(api, "triggerIndexing").mockResolvedValue({ status: "queued" } as any);
      const saved = vi.fn();
      const modal = new WorkspaceSettingsModal(store, { onSaved: saved });

      await modal.open(1);
      (modal.getElement().querySelector(".save-index-settings-btn") as HTMLButtonElement).click();
      await vi.waitFor(() => expect(update).toHaveBeenCalledWith(1, expect.any(Object)));
      await modal.open(2);
      resolveSave(a);
      await vi.waitFor(() => expect(index).toHaveBeenCalledWith(1, "incremental", false));

      expect(index).not.toHaveBeenCalledWith(2, "incremental", false);
      expect(saved).toHaveBeenCalledWith(1);
      expect(modal.getElement().querySelector(".ws-root-path")?.textContent).toBe("/b");
      expect(modal.getElement().style.display).toBe("flex");
      modal.close();
    });

    it("invalidates stale search results and clears files and symbols no longer linked to the active workspace", async () => {
      const ownTab = { fileId: 10, relativePath: "own.cpp", name: "own.cpp" };
      const linkedTab = { fileId: 20, relativePath: "lib.cpp", name: "lib.cpp" };
      store.setState({
        workspaceId: 1,
        selectedFileId: 20,
        selectedLine: 7,
        selectedSymbolId: 90,
        selectedSymbolName: "LinkedSymbol",
        openTabs: [ownTab, linkedTab],
      });
      vi.spyOn(api, "getWorkspaces").mockResolvedValue({ workspaces: [], total: 0 });
      vi.spyOn(api, "getWorkspaceStatus").mockResolvedValue({
        workspaceId: 1, status: "idle", revision: 1, fileCount: 0, symbolCount: 0,
        diagnosticCounts: { total: 0, errors: 0, warnings: 0, info: 0 },
      });
      vi.spyOn(api, "getFileMetadata").mockImplementation(async (_workspaceId, fileId) => {
        if (fileId === 20) throw new Error("No longer linked");
        return { id: 10, workspaceId: 1, path: "/own.cpp", relativePath: "own.cpp", name: "own.cpp",
          language: "cpp", encoding: "utf-8", sizeBytes: 1, modifiedNs: 0, isBinary: false,
          isGenerated: false, isDeleted: false, createdAt: "", updatedAt: "" };
      });
      vi.spyOn(api, "getSymbolDetail").mockRejectedValue(new Error("No longer linked"));
      let resolveStale!: (value: any) => void;
      const search = vi.spyOn(api, "searchSymbols")
        .mockImplementationOnce(() => new Promise((resolve) => { resolveStale = resolve; }))
        .mockResolvedValueOnce({
          items: [{ id: 100, fileId: 10, relativePath: "own.cpp", name: "FreshResult", kind: "class", rank: 1 }],
          total: 1, limit: 50, offset: 0, hasMore: false,
        });

      const container = document.createElement("div");
      document.body.appendChild(container);
      const app = new AppComponent(container, store);
      app.openSearch();
      (app as any).searchModalInput.value = "Widget";
      (app as any).searchModeSelect.value = "symbol";
      const staleSearch = (app as any).performSearch() as Promise<void>;
      expect(search).toHaveBeenCalledTimes(1);
      const refresh = (app as any).refreshLinkedContext(1) as Promise<void>;
      await refresh;
      resolveStale({
        items: [{ id: 99, fileId: 20, relativePath: "lib.cpp", name: "StaleResult", kind: "class", rank: 1 }],
        total: 1, limit: 50, offset: 0, hasMore: false,
      });
      await staleSearch;

      expect(search).toHaveBeenCalledTimes(2);
      expect(store.getState().workspaceId).toBe(1);
      expect(store.getState().selectedFileId).toBeNull();
      expect(store.getState().selectedSymbolId).toBeNull();
      expect(store.getState().openTabs).toEqual([ownTab]);
      expect((app as any).searchResultsList.textContent).toContain("FreshResult");
      expect((app as any).searchResultsList.textContent).not.toContain("StaleResult");

      app.closeSearch();
      (app as any).toolbar.destroy();
      (app as any).searchModalBackdrop.remove();
      container.remove();
    });

    it("shows a linked declaration with zero usages while retaining the consumer context", async () => {
      vi.spyOn(api, "getSymbolDetail").mockResolvedValue({
        symbol: { id: 9, fileId: 20, name: "Widget", kind: "class", ownerWorkspaceId: 2, ownerWorkspaceName: "B", range: { start: { line: 4 } } },
        file: { id: 20, relativePath: "Widget.cs" }, declarations: [], referencersCount: 0, callersCount: 0, calleesCount: 0,
      } as any);
      const references = vi.spyOn(api, "getSymbolReferences").mockResolvedValue({ items: [], total: 0, limit: 50, offset: 0, hasMore: false });
      const panel = new ReferencesComponent(store);
      await panel.loadReferences(9);
      const declaration = panel.getElement().querySelector(".declaration-link") as HTMLElement;
      expect(declaration.textContent).toContain("B · Widget.cs:4");
      declaration.click();
      expect(store.getState().workspaceId).toBe(1);
      expect(store.getState().selectedFileId).toBe(20);
      expect(references).toHaveBeenCalledWith(1, 9, 50, 0);
    });

    it("ignores an outdated reference response after switching workspace", async () => {
      let complete!: (v: any) => void;
      vi.spyOn(api, "getSymbolDetail").mockResolvedValue(null as any);
      vi.spyOn(api, "getSymbolReferences").mockReturnValue(new Promise(resolve => { complete = resolve; }));
      const panel = new ReferencesComponent(store);
      const loading = panel.loadReferences(9);
      store.setWorkspace(2);
      complete({ items: [], total: 0, hasMore: false });
      await loading;
      expect(panel.getElement().textContent).toContain("No symbol selected");
    });

    it("preserves advanced indexing fields and ordered roots", () => {
      const form = new IndexingSettingsForm();
      form.setValue({ language: "cpp", sourceRoots: ["/headers", "/runtime"], defaultIncludeRoots: ["/runtime", "/headers"], defines: ["FOO=1"], sdkVersion: "1" });
      expect(form.getValue()).toMatchObject({ sourceRoots: ["/headers", "/runtime"], defaultIncludeRoots: ["/runtime", "/headers"], defines: ["FOO=1"] });
    });
  });
});
