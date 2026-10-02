import type { StateStore } from "../state";
import type { JobDto, WorkspaceStatusDto, WorkspaceSummaryDto } from "../types";
import { api } from "../api";

const activeJobStatuses = new Set(["queued", "running", "cancelling"]);

function formatStatus(status: string): string {
  if (!status) return "Unknown";
  return status.charAt(0).toUpperCase() + status.slice(1);
}

function formatTimestamp(value?: string | null): string {
  if (!value) return "";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
}

export class WorkspaceSummaryComponent {
  private store: StateStore;
  private element: HTMLElement;
  private summary: WorkspaceSummaryDto | null = null;
  private observedStatus: WorkspaceStatusDto | null = null;
  private requestController: AbortController | null = null;
  private requestWorkspaceId: number | null = null;
  private requestVersion = 0;
  private activeRequest: Promise<void> | null = null;
  private visible = false;
  private refreshing = false;
  private refreshAgain = false;
  private errorMessage: string | null = null;

  constructor(store: StateStore) {
    this.store = store;
    this.element = document.createElement("div");
    this.element.className = "workspace-summary";
    this.element.hidden = true;
    this.renderLoading();
  }

  getElement(): HTMLElement {
    return this.element;
  }

  async show(forceRefresh = false): Promise<void> {
    this.visible = true;
    this.element.hidden = false;
    const workspaceId = this.store.getState().workspaceId;
    if (workspaceId === null) {
      this.renderNoWorkspace();
      return;
    }

    if (this.summary && this.summary.workspace.id !== workspaceId) {
      this.abortRequest();
      this.summary = null;
      this.observedStatus = null;
      this.errorMessage = null;
    }

    if (this.summary) {
      this.renderSummary();
    } else {
      this.renderLoading();
    }

    if (forceRefresh || !this.summary) {
      await this.refresh();
    }
  }

  hide(): void {
    this.visible = false;
    this.element.hidden = true;
    this.abortRequest();
  }

  async refresh(): Promise<void> {
    const workspaceId = this.store.getState().workspaceId;
    if (workspaceId === null) {
      this.renderNoWorkspace();
      return;
    }
    if (this.requestWorkspaceId === workspaceId && this.activeRequest) {
      return this.activeRequest;
    }

    this.abortRequest();
    const controller = new AbortController();
    const requestVersion = ++this.requestVersion;
    this.requestController = controller;
    this.requestWorkspaceId = workspaceId;
    this.refreshing = true;
    this.errorMessage = null;
    if (!this.summary || this.summary.workspace.id !== workspaceId) {
      this.renderLoading();
    } else {
      this.renderSummary();
    }

    const request = (async (): Promise<void> => {
      try {
        const summary = await api.getWorkspaceSummary(workspaceId, controller.signal);
        if (
          controller.signal.aborted ||
          requestVersion !== this.requestVersion ||
          this.store.getState().workspaceId !== workspaceId
        ) {
          return;
        }
        if (this.observedStatus?.workspaceId === workspaceId) {
          summary.status = this.observedStatus;
        }
        this.summary = summary;
        this.errorMessage = null;
        if (this.visible) this.renderSummary();
      } catch (error) {
        if (
          controller.signal.aborted ||
          requestVersion !== this.requestVersion ||
          this.store.getState().workspaceId !== workspaceId
        ) {
          return;
        }
        this.errorMessage = error instanceof Error ? error.message : String(error);
        if (this.visible) {
          if (this.summary?.workspace.id === workspaceId) this.renderSummary();
          else this.renderError();
        }
      } finally {
        if (requestVersion === this.requestVersion) {
          this.refreshing = false;
          this.requestController = null;
          this.requestWorkspaceId = null;
          this.activeRequest = null;
          const refreshAgain = this.refreshAgain;
          this.refreshAgain = false;
          if (this.visible && this.summary?.workspace.id === workspaceId) {
            this.renderSummary();
          }
          if (refreshAgain && this.visible && this.store.getState().workspaceId === workspaceId) {
            void this.refresh();
          }
        }
      }
    })();
    this.activeRequest = request;
    return request;
  }

  updateStatus(status: WorkspaceStatusDto): void {
    if (status.workspaceId !== this.store.getState().workspaceId) return;
    const previous = this.observedStatus;
    this.observedStatus = status;

    const summary = this.summary;
    if (!summary || summary.workspace.id !== status.workspaceId) return;
    const previousSummaryStatus = summary.status;
    const countsChanged =
      previousSummaryStatus.revision !== status.revision ||
      previousSummaryStatus.fileCount !== status.fileCount ||
      previousSummaryStatus.symbolCount !== status.symbolCount ||
      previousSummaryStatus.diagnosticCounts.total !== status.diagnosticCounts.total ||
      previousSummaryStatus.diagnosticCounts.errors !== status.diagnosticCounts.errors ||
      previousSummaryStatus.diagnosticCounts.warnings !== status.diagnosticCounts.warnings ||
      previousSummaryStatus.diagnosticCounts.info !== status.diagnosticCounts.info;
    summary.status = status;
    if (this.visible) this.renderSummary();

    if (!this.visible) return;
    const oldJob = previous?.workspaceId === status.workspaceId
      ? previous.latestJob
      : previousSummaryStatus.latestJob;
    const newJob = status.latestJob;
    const jobChanged = oldJob?.id !== newJob?.id ||
      (!!oldJob && !!newJob && activeJobStatuses.has(oldJob.status) && !activeJobStatuses.has(newJob.status));
    if (countsChanged || jobChanged) {
      if (this.activeRequest) this.refreshAgain = true;
      else void this.refresh();
    }
  }

  private abortRequest(): void {
    this.requestVersion++;
    this.requestController?.abort();
    this.requestController = null;
    this.requestWorkspaceId = null;
    this.activeRequest = null;
    this.refreshing = false;
    this.refreshAgain = false;
  }

  private renderLoading(): void {
    this.element.innerHTML = `
      <div class="workspace-summary-state" role="status">
        <div class="workspace-summary-eyebrow">Workspace summary</div>
        <h1>Loading workspace…</h1>
      </div>
    `;
  }

  private renderNoWorkspace(): void {
    this.element.innerHTML = `
      <div class="workspace-summary-state">
        <div class="workspace-summary-eyebrow">Workspace summary</div>
        <h1>Select a workspace</h1>
        <p>Choose a workspace from the toolbar to see its index overview.</p>
      </div>
    `;
  }

  private renderError(): void {
    this.element.innerHTML = `
      <div class="workspace-summary-state">
        <div class="workspace-summary-eyebrow">Workspace summary</div>
        <h1>Summary unavailable</h1>
        <p class="workspace-summary-error"></p>
        <button class="btn-icon workspace-summary-retry">Retry</button>
      </div>
    `;
    this.element.querySelector<HTMLElement>(".workspace-summary-error")!.textContent =
      this.errorMessage || "The workspace summary could not be loaded.";
    this.element.querySelector<HTMLButtonElement>(".workspace-summary-retry")!
      .addEventListener("click", () => void this.refresh());
  }

  private renderSummary(): void {
    const summary = this.summary;
    if (!summary) return;
    const { workspace, status } = summary;
    const job = status.latestJob;
    const statusClass = status.status === "error" || job?.status === "failed"
      ? "failed"
      : activeJobStatuses.has(job?.status || "") || status.status === "indexing"
        ? "running"
        : "ready";

    this.element.innerHTML = `
      <div class="workspace-summary-content">
        <header class="workspace-summary-header">
          <div>
            <div class="workspace-summary-eyebrow">Workspace summary</div>
            <h1 class="workspace-summary-name"></h1>
            <p class="workspace-summary-root"></p>
          </div>
          <div class="workspace-summary-actions">
            <span class="workspace-summary-status"></span>
            <button class="btn-icon workspace-summary-refresh">Refresh</button>
          </div>
        </header>

        <section class="workspace-summary-stats" aria-label="Index totals">
          <article class="workspace-summary-card"><span>Files</span><strong class="summary-file-count"></strong></article>
          <article class="workspace-summary-card"><span>Symbols</span><strong class="summary-symbol-count"></strong></article>
          <article class="workspace-summary-card"><span>Diagnostics</span><strong class="summary-diagnostic-count"></strong></article>
        </section>

        <div class="workspace-summary-columns">
          <section class="workspace-summary-section">
            <h2>Languages</h2>
            <table class="workspace-summary-languages">
              <thead><tr><th scope="col">Language</th><th scope="col">Files</th></tr></thead>
              <tbody></tbody>
            </table>
            <p class="workspace-summary-no-languages" hidden>No indexed files yet.</p>
          </section>

          <section class="workspace-summary-section">
            <h2>Diagnostics</h2>
            <dl class="workspace-summary-diagnostics">
              <div><dt>Errors</dt><dd class="summary-errors"></dd></div>
              <div><dt>Warnings</dt><dd class="summary-warnings"></dd></div>
              <div><dt>Info</dt><dd class="summary-info"></dd></div>
            </dl>
          </section>
        </div>

        <section class="workspace-summary-section workspace-summary-job">
          <div class="workspace-summary-job-heading"><h2>Latest indexing job</h2><span class="summary-revision"></span></div>
          <p class="summary-job-empty" hidden></p>
          <div class="summary-job-details">
            <span class="summary-job-status"></span>
            <span class="summary-job-progress"></span>
            <span class="summary-job-time"></span>
          </div>
          <p class="summary-job-counts"></p>
          <p class="workspace-summary-job-error" hidden></p>
        </section>

        <p class="workspace-summary-load-error" hidden></p>
      </div>
    `;

    this.element.querySelector<HTMLElement>(".workspace-summary-name")!.textContent =
      workspace.name || workspace.rootPath;
    this.element.querySelector<HTMLElement>(".workspace-summary-root")!.textContent = workspace.rootPath;
    const statusBadge = this.element.querySelector<HTMLElement>(".workspace-summary-status")!;
    statusBadge.textContent = formatStatus(status.status);
    statusBadge.classList.add(`status-${statusClass}`);
    this.element.querySelector<HTMLElement>(".summary-file-count")!.textContent =
      status.fileCount.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-symbol-count")!.textContent =
      status.symbolCount.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-diagnostic-count")!.textContent =
      status.diagnosticCounts.total.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-errors")!.textContent =
      status.diagnosticCounts.errors.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-warnings")!.textContent =
      status.diagnosticCounts.warnings.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-info")!.textContent =
      status.diagnosticCounts.info.toLocaleString();
    this.element.querySelector<HTMLElement>(".summary-revision")!.textContent =
      `Revision ${status.revision}`;

    const languageBody = this.element.querySelector(".workspace-summary-languages tbody")!;
    for (const item of summary.languages) {
      const row = document.createElement("tr");
      const language = document.createElement("td");
      language.textContent = item.language;
      const count = document.createElement("td");
      count.textContent = item.fileCount.toLocaleString();
      row.append(language, count);
      languageBody.appendChild(row);
    }
    this.element.querySelector<HTMLElement>(".workspace-summary-no-languages")!.hidden =
      summary.languages.length > 0;

    this.renderJob(job);
    const loadError = this.element.querySelector<HTMLElement>(".workspace-summary-load-error")!;
    if (this.errorMessage) {
      loadError.textContent = `${this.errorMessage} `;
      const retry = document.createElement("button");
      retry.className = "workspace-summary-inline-retry";
      retry.textContent = "Retry";
      retry.addEventListener("click", () => void this.refresh());
      loadError.appendChild(retry);
      loadError.hidden = false;
    }

    const refreshButton = this.element.querySelector<HTMLButtonElement>(".workspace-summary-refresh")!;
    refreshButton.disabled = this.refreshing;
    refreshButton.textContent = this.refreshing ? "Refreshing…" : "Refresh";
    refreshButton.addEventListener("click", () => void this.refresh());
  }

  private renderJob(job: JobDto | null | undefined): void {
    const empty = this.element.querySelector<HTMLElement>(".summary-job-empty")!;
    const details = this.element.querySelector<HTMLElement>(".summary-job-details")!;
    const error = this.element.querySelector<HTMLElement>(".workspace-summary-job-error")!;
    if (!job) {
      empty.textContent = "No indexing jobs have run for this workspace.";
      empty.hidden = false;
      details.hidden = true;
      this.element.querySelector<HTMLElement>(".summary-job-counts")!.hidden = true;
      error.hidden = true;
      return;
    }

    empty.hidden = true;
    details.hidden = false;
    this.element.querySelector<HTMLElement>(".summary-job-status")!.textContent =
      `${formatStatus(job.jobType)} · ${formatStatus(job.status)}`;
    this.element.querySelector<HTMLElement>(".summary-job-progress")!.textContent =
      job.filesTotal > 0
        ? `${job.filesProcessed.toLocaleString()} of ${job.filesTotal.toLocaleString()} files processed · ${job.filesSkipped.toLocaleString()} skipped`
        : `${job.filesProcessed.toLocaleString()} files processed · ${job.filesSkipped.toLocaleString()} skipped`;
    this.element.querySelector<HTMLElement>(".summary-job-time")!.textContent =
      formatTimestamp(job.finishedAt || job.startedAt || job.queuedAt);
    const counts = this.element.querySelector<HTMLElement>(".summary-job-counts")!;
    counts.textContent = `${job.errorCount.toLocaleString()} errors · ${job.warningCount.toLocaleString()} warnings`;
    counts.hidden = false;
    if (job.errorMessage) {
      error.textContent = job.errorMessage;
      error.hidden = false;
    } else {
      error.textContent = "";
      error.hidden = true;
    }
  }
}
