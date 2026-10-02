import { beforeEach, describe, expect, it, vi } from "vitest";
import { WorkspaceSummaryComponent } from "../src/components/workspace-summary";
import { CodeWindowComponent } from "../src/components/code-window";
import { StateStore } from "../src/state";
import type { WorkspaceSummaryDto } from "../src/types";
import { api } from "../src/api";

function makeSummary(workspaceId = 1, name = "Sample workspace"): WorkspaceSummaryDto {
  return {
    workspace: {
      id: workspaceId,
      rootPath: `/projects/${workspaceId}`,
      name,
      revision: 3,
      status: "ready",
      createdAt: "2026-01-01T00:00:00Z",
      updatedAt: "2026-01-02T00:00:00Z",
    },
    status: {
      workspaceId,
      status: "ready",
      revision: 3,
      latestJob: {
        id: 9,
        workspaceId,
        jobType: "incremental",
        status: "completed",
        queuedAt: "2026-01-02T10:00:00Z",
        startedAt: "2026-01-02T10:00:01Z",
        finishedAt: "2026-01-02T10:00:05Z",
        filesTotal: 4,
        filesProcessed: 4,
        filesSkipped: 1,
        errorCount: 0,
        warningCount: 2,
        workspaceRevision: 3,
        errorMessage: null,
      },
      fileCount: 4,
      symbolCount: 22,
      diagnosticCounts: { total: 3, errors: 1, warnings: 2, info: 0 },
    },
    languages: [
      { language: "cpp", fileCount: 3 },
      { language: "c", fileCount: 1 },
    ],
  };
}

describe("WorkspaceSummaryComponent", () => {
  let store: StateStore;

  beforeEach(() => {
    store = new StateStore({ workspaceId: 1 });
    vi.restoreAllMocks();
  });

  it("renders workspace details, index totals, languages, diagnostics, and job result", async () => {
    const untrustedName = '<img src=x onerror="alert(1)">';
    vi.spyOn(api, "getWorkspaceSummary").mockResolvedValue(makeSummary(1, untrustedName));
    const summary = new WorkspaceSummaryComponent(store);

    await summary.show();

    const element = summary.getElement();
    expect(element.querySelector("h1")?.textContent).toBe(untrustedName);
    expect(element.querySelector("img")).toBeNull();
    expect(element.querySelector(".summary-file-count")?.textContent).toBe("4");
    expect(element.querySelector(".summary-symbol-count")?.textContent).toBe("22");
    expect(element.querySelector(".summary-diagnostic-count")?.textContent).toBe("3");
    expect(element.querySelectorAll(".workspace-summary-languages tbody tr")).toHaveLength(2);
    expect(element.querySelector(".summary-job-progress")?.textContent).toContain("4 of 4 files");
    expect(element.querySelector(".summary-job-counts")?.textContent).toContain("2 warnings");
  });

  it("discards an older workspace response after switching workspaces", async () => {
    let resolveFirst!: (summary: WorkspaceSummaryDto) => void;
    const first = new Promise<WorkspaceSummaryDto>((resolve) => { resolveFirst = resolve; });
    vi.spyOn(api, "getWorkspaceSummary").mockImplementation((workspaceId) =>
      workspaceId === 1 ? first : Promise.resolve(makeSummary(2, "Second workspace"))
    );
    const summary = new WorkspaceSummaryComponent(store);
    const firstLoad = summary.show(true);

    store.setWorkspace(2);
    await summary.show(true);
    resolveFirst(makeSummary(1, "Stale workspace"));
    await firstLoad;

    expect(summary.getElement().querySelector("h1")?.textContent).toBe("Second workspace");
  });

  it("shows current indexing progress from the existing status poll", async () => {
    const response = makeSummary();
    vi.spyOn(api, "getWorkspaceSummary").mockResolvedValue(response);
    const summary = new WorkspaceSummaryComponent(store);
    await summary.show();

    summary.updateStatus({
      ...response.status,
      status: "indexing",
      latestJob: { ...response.status.latestJob!, status: "running", filesProcessed: 2 },
    });

    expect(summary.getElement().querySelector(".summary-job-status")?.textContent)
      .toContain("Running");
    expect(summary.getElement().querySelector(".summary-job-progress")?.textContent)
      .toContain("2 of 4 files");
    expect(api.getWorkspaceSummary).toHaveBeenCalledTimes(1);
  });

  it("rechecks language counts when polling detects changes during a refresh", async () => {
    let resolveRefresh!: (summary: WorkspaceSummaryDto) => void;
    const inFlight = new Promise<WorkspaceSummaryDto>((resolve) => { resolveRefresh = resolve; });
    const updated = makeSummary();
    updated.status.revision = 4;
    updated.status.fileCount = 5;
    updated.languages.push({ language: "rust", fileCount: 1 });
    let callCount = 0;
    const getSummary = vi.spyOn(api, "getWorkspaceSummary").mockImplementation((workspaceId) => {
      callCount++;
      if (callCount === 1) return Promise.resolve(makeSummary(workspaceId));
      if (callCount === 2) return inFlight;
      return Promise.resolve(updated);
    });
    const summary = new WorkspaceSummaryComponent(store);
    await summary.show();
    const refreshRequest = summary.refresh();

    summary.updateStatus({ ...makeSummary().status, revision: 4, fileCount: 5 });
    resolveRefresh(makeSummary());
    await refreshRequest;

    await vi.waitFor(() => expect(getSummary).toHaveBeenCalledTimes(3));
    expect(summary.getElement().querySelector(".summary-file-count")?.textContent).toBe("5");
    expect(summary.getElement().textContent).toContain("rust");
  });

  it("shows the summary in the code pane while retaining open file tabs", async () => {
    const openTabs = [{ fileId: 12, relativePath: "src/main.cpp", name: "main.cpp" }];
    store = new StateStore({ workspaceId: 1, openTabs });
    vi.spyOn(api, "getWorkspaceSummary").mockResolvedValue(makeSummary());
    const codeWindow = new CodeWindowComponent(store);

    await codeWindow.showWorkspaceSummary();

    expect(codeWindow.getElement().querySelector(".workspace-summary-name")?.textContent)
      .toBe("Sample workspace");
    expect(codeWindow.getElement().querySelectorAll(".code-tab")).toHaveLength(1);
    expect(store.getState().openTabs).toEqual(openTabs);
  });

  it("offers retry after a failed summary request", async () => {
    vi.spyOn(api, "getWorkspaceSummary")
      .mockRejectedValueOnce(new Error("offline"))
      .mockResolvedValueOnce(makeSummary());
    const summary = new WorkspaceSummaryComponent(store);

    await summary.show(true);
    expect(summary.getElement().textContent).toContain("offline");
    (summary.getElement().querySelector(".workspace-summary-retry") as HTMLButtonElement).click();

    await vi.waitFor(() => expect(summary.getElement().querySelector("h1")?.textContent)
      .toBe("Sample workspace"));
  });
});
