import { describe, it, expect, vi, beforeEach } from "vitest";
import { StateStore } from "../src/state";
import { ToolbarComponent } from "../src/components/toolbar";
import { ExplorerComponent } from "../src/components/explorer";
import { CodeWindowComponent } from "../src/components/code-window";
import { OutlineComponent } from "../src/components/outline";
import { ReferencesComponent } from "../src/components/references";
import { DiagnosticsComponent } from "../src/components/diagnostics";
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
      expect(elem.querySelector(".search-input")).not.toBeNull();
      expect(elem.querySelector(".index-status-badge")).not.toBeNull();

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
      expect(store.getState().selectedLine).toBe(6); // 5 (0-based) + 1 = 6 (1-based)
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
      expect(elem.textContent).toContain("main.cpp:43"); // 42 (0-based) + 1 = 43 (1-based)

      // Click on reference navigates to target file and line
      const refItem = elem.querySelector(".reference-item") as HTMLElement;
      refItem.click();
      expect(store.getState().selectedFileId).toBe(2);
      expect(store.getState().selectedLine).toBe(43); // 42 + 1 = 43
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
          { fileId: 1, relativePath: "malicious.cpp", snippet: maliciousSnippet, rank: 1.0 },
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

      // Now test symbol search escaping
      vi.spyOn(api, "searchSymbols").mockResolvedValue({
        items: [
          { id: 1, fileId: 1, relativePath: "malicious.cpp", name: maliciousName, kind: "function", rank: 1.0 },
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
});
