export interface WorkspaceDto {
  id: number;
  rootPath: string;
  name: string;
  includePatterns?: string[];
  excludePatterns?: string[];
  defaultIgnores?: string[];
  compileCommandsPath?: string | null;
  defaultCompileCommand?: string | null;
  revision: number;
  status: string;
  lastError?: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface CreateWorkspaceRequest {
  rootPath: string;
  name?: string;
  includePatterns?: string[];
  excludePatterns?: string[];
  defaultIgnores?: string[];
  compileCommandsPath?: string | null;
  defaultCompileCommand?: string | null;
}

export interface WorkspaceListResponse {
  workspaces: WorkspaceDto[];
  total: number;
}

export interface LibraryDto {
  id: number;
  workspaceId: number;
  name: string;
  language: string;
  provider: string;
  sdkVersion?: string | null;
  targetEnvironment?: string | null;
  languageStandard?: string | null;
  targetFramework?: string | null;
  sysroot?: string | null;
  sourceRoots: string[];
  defaultIncludeRoots: string[];
  defines: string[];
  includePatterns: string[];
  excludePatterns: string[];
  fingerprint: string;
  status: string;
  lastError?: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface LibraryListResponse {
  libraries: LibraryDto[];
  total: number;
}

export interface WorkspaceLibrariesResponse {
  workspaceId: number;
  libraries: LibraryDto[];
  total: number;
}

export interface CreateLibraryRequest {
  name: string;
  language: string;
  sourceRoots: string[];
  rootPath?: string;
  provider?: string;
  sdkVersion?: string | null;
  targetEnvironment?: string | null;
  languageStandard?: string | null;
  targetFramework?: string | null;
  sysroot?: string | null;
  defaultIncludeRoots?: string[];
  defines?: string[];
  includePatterns?: string[];
  excludePatterns?: string[];
}

export interface OriginMetadataDto {
  origin?: "project" | "library" | string;
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface AttachLibraryRequest {
  profileId: number;
}

export interface DiagnosticCountsDto {
  total: number;
  errors: number;
  warnings: number;
  info: number;
}

export interface JobDto {
  id: number;
  workspaceId: number;
  jobType: string;
  status: string;
  queuedAt: string;
  startedAt?: string | null;
  finishedAt?: string | null;
  filesTotal: number;
  filesProcessed: number;
  filesSkipped: number;
  errorCount: number;
  warningCount: number;
  workspaceRevision?: number | null;
  errorMessage?: string | null;
}

export interface WorkspaceStatusDto {
  workspaceId: number;
  status: string;
  revision: number;
  latestJob?: JobDto | null;
  fileCount: number;
  symbolCount: number;
  diagnosticCounts: DiagnosticCountsDto;
}

export interface TreeNodeDto {
  name: string;
  path: string;
  type: "directory" | "file";
  fileId?: number | null;
  sizeBytes: number;
  isBinary: boolean;
  language: string;
}

export interface WorkspaceTreeDto {
  workspaceId: number;
  path: string;
  entries: TreeNodeDto[];
}

export interface FileMetadataDto {
  id: number;
  workspaceId: number;
  path: string;
  relativePath: string;
  name: string;
  extension?: string | null;
  language: string;
  encoding: string;
  sizeBytes: number;
  modifiedNs: number;
  contentHash?: string | null;
  isBinary: boolean;
  isGenerated: boolean;
  isDeleted: boolean;
  indexedAt?: string | null;
  createdAt: string;
  updatedAt: string;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface FileContentDto {
  fileId: number;
  path: string;
  content: string;
  totalSizeBytes: number;
  totalLines: number;
  startLine: number;
  endLine: number;
  startByte: number;
  endByte: number;
  isBinary: boolean;
  contentHash: string;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface PositionDto {
  line: number;
  column: number;
  byte: number;
}

export interface RangeDto {
  start: PositionDto;
  end: PositionDto;
}

export interface HighlightToken {
  line: number;
  startColumn?: number;
  start?: number;
  length?: number;
  end?: number;
  tokenType?: number | string;
  kind?: string;
  tokenModifiers?: number;
}

export interface HighlightLegendDto {
  tokenTypes?: string[];
  tokenModifiers?: string[];
  [key: string]: any;
}

export interface HighlightResponseDto {
  fileId: number;
  language?: string;
  legend: HighlightLegendDto;
  tokens: HighlightToken[];
}

export interface SymbolDto {
  id: number;
  workspaceId: number;
  fileId: number;
  symbolKey: string;
  name: string;
  qualifiedName?: string | null;
  displayName?: string | null;
  kind: string;
  language: string;
  signature?: string | null;
  documentation?: string | null;
  containerName?: string | null;
  scopeSymbolId?: number | null;
  visibility?: string | null;
  isDefinition: boolean;
  isDeclaration: boolean;
  range: RangeDto;
  createdAt: string;
  relativePath?: string | null;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface SymbolOutlineNodeDto {
  id: number;
  name: string;
  qualifiedName?: string | null;
  kind: string;
  signature?: string | null;
  scopeSymbolId?: number | null;
  range: RangeDto;
  children: SymbolOutlineNodeDto[];
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface FileOutlineDto {
  fileId: number;
  outline: SymbolOutlineNodeDto[];
}

export interface OccurrenceDto {
  id: number;
  workspaceId: number;
  fileId: number;
  symbolId?: number | null;
  occurrenceKind: string;
  name: string;
  range: RangeDto;
  confidence: number;
  resolution: string;
}

export interface OccurrencesResponseDto {
  fileId: number;
  occurrences: OccurrenceDto[];
  total: number;
}

export interface ReferencerDto {
  id: number;
  referenceKind: string;
  name: string;
  resolution: string;
  confidence: number;
  range: RangeDto;
  fileId: number;
  relativePath: string;
  containingSymbolId?: number | null;
  containingSymbolName?: string | null;
  containingQualifiedName?: string | null;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface SymbolDetailDto {
  symbol: SymbolDto;
  file: FileMetadataDto;
  declarations: SymbolDto[];
  callersCount: number;
  calleesCount: number;
  referencersCount: number;
}

export interface PaginatedResult<T> {
  items: T[];
  total: number;
  limit: number;
  offset: number;
  hasMore: boolean;
}

export interface SourceSearchHitDto {
  fileId: number;
  relativePath: string;
  snippet: string;
  rank: number;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface SymbolSearchHitDto {
  id: number;
  fileId: number;
  relativePath: string;
  name: string;
  qualifiedName?: string | null;
  kind: string;
  rank: number;
  line?: number | null;
  origin?: OriginMetadataDto["origin"];
  ownerWorkspaceId?: number;
  libraryProfileId?: number | null;
  targetFramework?: string | null;
}

export interface DiagnosticItem {
  id?: number;
  workspaceId?: number;
  fileId?: number;
  relativePath?: string;
  filePath?: string;
  line: number;
  column?: number;
  severity: "error" | "warning" | "info";
  message: string;
  code?: string;
  source?: string;
}

export interface DiagnosticsResponseDto {
  workspaceId?: number;
  fileId?: number;
  diagnostics: DiagnosticItem[];
  total: number;
}

export interface OpenTabDto {
  fileId: number;
  relativePath: string;
  name: string;
}

export type IndexStatus = "idle" | "running" | "failed";

export interface CompileCommandDto {
  directory: string;
  file: string;
  output?: string | null;
  arguments: string[];
  includeDirs: string[];
  defines: string[];
  languageStandard?: string | null;
}

export interface FileCompileCommandDto {
  fileId: number;
  hasCompileCommand: boolean;
  isWorkspaceDefault?: boolean;
  databasePath?: string | null;
  isAutoDetected: boolean;
  compileCommand?: CompileCommandDto | null;
}

export interface WorkspaceCompileCommandsDto {
  configuredPath?: string | null;
  effectivePath?: string | null;
  exists: boolean;
  isAutoDetected: boolean;
  totalCommands: number;
  defaultCompileCommand?: string | null;
}

export interface UpdateWorkspaceRequest {
  name?: string;
  includePatterns?: string[];
  excludePatterns?: string[];
  compileCommandsPath?: string | null;
  defaultCompileCommand?: string | null;
}

export interface AppState {
  workspaceId: number | null;
  selectedFileId: number | null;
  selectedLine: number | null;
  selectedSymbolId: number | null;
  openTabs: OpenTabDto[];
  expandedFolders: Set<string>;
  indexStatus: IndexStatus;
  activeMobileTab: "explorer" | "code" | "outline" | "inspector";
  activeInspectorTab: "references" | "diagnostics" | "compile-command";
  searchQuery: string;
  searchType: "source" | "symbol";
  isSearching: boolean;
}
