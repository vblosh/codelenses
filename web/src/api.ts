import type {
  WorkspaceListResponse,
  WorkspaceDto,
  WorkspaceStatusDto,
  WorkspaceSummaryDto,
  WorkspaceTreeDto,
  FileMetadataDto,
  FileContentDto,
  HighlightResponseDto,
  FileOutlineDto,
  OccurrencesResponseDto,
  SymbolDetailDto,
  ReferencerDto,
  PaginatedResult,
  SourceSearchHitDto,
  SymbolSearchHitDto,
  JobDto,
  DiagnosticsResponseDto,
  FileCompileCommandDto,
  WorkspaceCompileCommandsDto,
  UpdateWorkspaceRequest,
  CreateWorkspaceRequest,
} from "./types";

export class ApiClient {
  private baseUrl: string;

  constructor(baseUrl: string = "") {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
  }

  private async request<T>(endpoint: string, options: RequestInit = {}): Promise<T> {
    const url = `${this.baseUrl}${endpoint}`;
    const headers = new Headers(options.headers || {});
    if (!headers.has("Accept")) {
      headers.set("Accept", "application/json");
    }

    const response = await fetch(url, { ...options, headers });
    if (!response.ok) {
      let errorMessage = `HTTP ${response.status}: ${response.statusText}`;
      try {
        const errorJson = await response.json();
        if (errorJson?.error?.message) {
          errorMessage = errorJson.error.message;
        } else if (errorJson?.message) {
          errorMessage = errorJson.message;
        }
      } catch {
        // Ignored, use status text
      }
      throw new Error(errorMessage);
    }

    return (await response.json()) as T;
  }

  async getWorkspaces(): Promise<WorkspaceListResponse> {
    return this.request<WorkspaceListResponse>("/api/v1/workspaces");
  }

  async getWorkspace(id: number): Promise<WorkspaceDto> {
    return this.request<WorkspaceDto>(`/api/v1/workspaces/${id}`);
  }

  async createWorkspace(req: CreateWorkspaceRequest): Promise<WorkspaceDto> {
    return this.request<WorkspaceDto>("/api/v1/workspaces", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(req),
    });
  }

  async deleteWorkspace(id: number): Promise<{ status: string; id: number }> {
    return this.request<{ status: string; id: number }>(`/api/v1/workspaces/${id}`, {
      method: "DELETE",
    });
  }

  async getTree(workspaceId: number, path: string = ""): Promise<WorkspaceTreeDto> {
    const query = path ? `?path=${encodeURIComponent(path)}` : "";
    return this.request<WorkspaceTreeDto>(`/api/v1/workspaces/${workspaceId}/tree${query}`);
  }

  async getFileMetadata(workspaceId: number, fileId: number): Promise<FileMetadataDto> {
    return this.request<FileMetadataDto>(`/api/v1/workspaces/${workspaceId}/files/${fileId}`);
  }

  async getFileContent(
    workspaceId: number,
    fileId: number,
    startLine?: number,
    endLine?: number
  ): Promise<FileContentDto> {
    const params = new URLSearchParams();
    if (startLine !== undefined) params.set("startLine", String(startLine));
    if (endLine !== undefined) params.set("endLine", String(endLine));
    const query = params.toString() ? `?${params.toString()}` : "";
    return this.request<FileContentDto>(
      `/api/v1/workspaces/${workspaceId}/files/${fileId}/content${query}`
    );
  }

  async getFileHighlights(workspaceId: number, fileId: number): Promise<HighlightResponseDto> {
    return this.request<HighlightResponseDto>(
      `/api/v1/workspaces/${workspaceId}/files/${fileId}/highlights`
    );
  }

  async getFileOutline(workspaceId: number, fileId: number): Promise<FileOutlineDto> {
    return this.request<FileOutlineDto>(`/api/v1/workspaces/${workspaceId}/files/${fileId}/outline`);
  }

  async getFileOccurrences(
    workspaceId: number,
    fileId: number,
    kind?: string
  ): Promise<OccurrencesResponseDto> {
    const query = kind ? `?kind=${encodeURIComponent(kind)}` : "";
    return this.request<OccurrencesResponseDto>(
      `/api/v1/workspaces/${workspaceId}/files/${fileId}/occurrences${query}`
    );
  }

  async getSymbolDetail(workspaceId: number, symbolId: number): Promise<SymbolDetailDto> {
    return this.request<SymbolDetailDto>(
      `/api/v1/workspaces/${workspaceId}/symbols/${symbolId}`
    );
  }

  async getSymbolReferences(
    workspaceId: number,
    symbolId: number,
    limit: number = 50,
    offset: number = 0
  ): Promise<PaginatedResult<ReferencerDto>> {
    return this.request<PaginatedResult<ReferencerDto>>(
      `/api/v1/workspaces/${workspaceId}/symbols/${symbolId}/references?limit=${limit}&offset=${offset}`
    );
  }

  async getWorkspaceStatus(
    workspaceId: number,
    signal?: AbortSignal
  ): Promise<WorkspaceStatusDto> {
    return this.request<WorkspaceStatusDto>(`/api/v1/workspaces/${workspaceId}/status`, {
      signal,
    });
  }

  async getWorkspaceSummary(
    workspaceId: number,
    signal?: AbortSignal
  ): Promise<WorkspaceSummaryDto> {
    return this.request<WorkspaceSummaryDto>(
      `/api/v1/workspaces/${workspaceId}/summary`,
      { signal }
    );
  }

  async triggerIndexing(
    workspaceId: number,
    jobType: string = "incremental",
    forceFull: boolean = false
  ): Promise<JobDto> {
    return this.request<JobDto>(`/api/v1/workspaces/${workspaceId}/index`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ jobType, forceFull }),
    });
  }

  async cancelJob(jobId: number): Promise<JobDto> {
    return this.request<JobDto>(`/api/v1/jobs/${jobId}/cancel`, {
      method: "POST",
    });
  }

  async searchSource(
    workspaceId: number,
    query: string,
    limit: number = 50,
    offset: number = 0
  ): Promise<PaginatedResult<SourceSearchHitDto>> {
    return this.request<PaginatedResult<SourceSearchHitDto>>(
      `/api/v1/workspaces/${workspaceId}/search?q=${encodeURIComponent(query)}&limit=${limit}&offset=${offset}`
    );
  }

  async searchSymbols(
    workspaceId: number,
    query: string,
    limit: number = 50,
    offset: number = 0
  ): Promise<PaginatedResult<SymbolSearchHitDto>> {
    return this.request<PaginatedResult<SymbolSearchHitDto>>(
      `/api/v1/workspaces/${workspaceId}/search/symbols?q=${encodeURIComponent(query)}&limit=${limit}&offset=${offset}`
    );
  }

  async getWorkspaceDiagnostics(
    workspaceId: number,
    severity?: string,
    limit: number = 100,
    offset: number = 0
  ): Promise<DiagnosticsResponseDto> {
    const params = new URLSearchParams();
    if (severity) params.set("severity", severity);
    params.set("limit", String(limit));
    params.set("offset", String(offset));
    return this.request<DiagnosticsResponseDto>(
      `/api/v1/workspaces/${workspaceId}/diagnostics?${params.toString()}`
    );
  }

  async getFileDiagnostics(
    workspaceId: number,
    fileId: number
  ): Promise<DiagnosticsResponseDto> {
    return this.request<DiagnosticsResponseDto>(
      `/api/v1/workspaces/${workspaceId}/files/${fileId}/diagnostics`
    );
  }

  async getFileCompileCommand(
    workspaceId: number,
    fileId: number
  ): Promise<FileCompileCommandDto> {
    return this.request<FileCompileCommandDto>(
      `/api/v1/workspaces/${workspaceId}/files/${fileId}/compile-command`
    );
  }

  async getWorkspaceCompileCommands(
    workspaceId: number
  ): Promise<WorkspaceCompileCommandsDto> {
    return this.request<WorkspaceCompileCommandsDto>(
      `/api/v1/workspaces/${workspaceId}/compile-commands`
    );
  }

  async updateWorkspace(
    workspaceId: number,
    req: UpdateWorkspaceRequest
  ): Promise<WorkspaceDto> {
    return this.request<WorkspaceDto>(
      `/api/v1/workspaces/${workspaceId}`,
      {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(req),
      }
    );
  }

  async getWorkspaceLinks(workspaceId: number): Promise<WorkspaceListResponse> {
    return this.request<WorkspaceListResponse>(`/api/v1/workspaces/${workspaceId}/links`);
  }

  async linkWorkspace(workspaceId: number, targetWorkspaceId: number): Promise<WorkspaceDto> {
    return this.request<WorkspaceDto>(`/api/v1/workspaces/${workspaceId}/links`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ targetWorkspaceId }),
    });
  }

  async unlinkWorkspace(workspaceId: number, targetWorkspaceId: number): Promise<{ status: string }> {
    return this.request<{ status: string }>(`/api/v1/workspaces/${workspaceId}/links/${targetWorkspaceId}`, { method: "DELETE" });
  }
}

export const api = new ApiClient();
