import { describe, it, expect, vi, beforeEach } from "vitest";
import { ApiClient } from "../src/api";

describe("ApiClient", () => {
  let client: ApiClient;

  beforeEach(() => {
    client = new ApiClient("http://localhost:8080");
    vi.restoreAllMocks();
  });

  it("fetches workspaces list", async () => {
    const mockData = { workspaces: [{ id: 1, name: "test", rootPath: "/test", revision: 1 }], total: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockData,
    } as any);

    const result = await client.getWorkspaces();
    expect(result.workspaces).toHaveLength(1);
    expect(result.workspaces[0].name).toBe("test");
    expect(global.fetch).toHaveBeenCalledWith("http://localhost:8080/api/v1/workspaces", expect.any(Object));
  });

  it("fetches workspace tree with path query param", async () => {
    const mockTree = { workspaceId: 1, path: "src", entries: [] };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockTree,
    } as any);

    const result = await client.getTree(1, "src/adapters");
    expect(result.workspaceId).toBe(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/tree?path=src%2Fadapters",
      expect.any(Object)
    );
  });

  it("fetches file content with range parameters", async () => {
    const mockContent = { fileId: 5, path: "main.cpp", content: "int main() {}", totalLines: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockContent,
    } as any);

    const result = await client.getFileContent(1, 5, 10, 50);
    expect(result.fileId).toBe(5);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/files/5/content?startLine=10&endLine=50",
      expect.any(Object)
    );
  });

  it("handles HTTP error response with error envelope", async () => {
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: false,
      status: 404,
      statusText: "Not Found",
      json: async () => ({
        error: { code: "not_found", message: "Workspace 99 not found" },
      }),
    } as any);

    await expect(client.getWorkspace(99)).rejects.toThrow("Workspace 99 not found");
  });

  it("triggers indexing with post body", async () => {
    const mockJob = { id: 10, workspaceId: 1, jobType: "incremental", status: "queued" };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockJob,
    } as any);

    const result = await client.triggerIndexing(1, "incremental", false);
    expect(result.id).toBe(10);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/index",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({ jobType: "incremental", forceFull: false }),
      })
    );
  });

  it("searches source and symbols", async () => {
    const mockHits = { items: [{ fileId: 1, relativePath: "a.cpp", snippet: "match", rank: 1.0 }], total: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockHits,
    } as any);

    const result = await client.searchSource(1, "queryText");
    expect(result.items).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/search?q=queryText&limit=50&offset=0",
      expect.any(Object)
    );
  });

  it("fetches workspace diagnostics with query params", async () => {
    const mockDiags = {
      workspaceId: 1,
      diagnostics: [
        { id: 1, workspaceId: 1, fileId: 2, severity: "error", message: "fail", line: 5 },
      ],
      total: 1,
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockDiags,
    } as any);

    const result = await client.getWorkspaceDiagnostics(1, "error", 20, 10);
    expect(result.workspaceId).toBe(1);
    expect(result.diagnostics).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/diagnostics?severity=error&limit=20&offset=10",
      expect.any(Object)
    );
  });

  it("fetches file diagnostics", async () => {
    const mockDiags = {
      fileId: 2,
      diagnostics: [
        { id: 2, workspaceId: 1, fileId: 2, severity: "warning", message: "warn", line: 10 },
      ],
      total: 1,
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockDiags,
    } as any);

    const result = await client.getFileDiagnostics(1, 2);
    expect(result.fileId).toBe(2);
    expect(result.diagnostics).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/files/2/diagnostics",
      expect.any(Object)
    );
  });

  it("fetches file compile command with workspace default flag", async () => {
    const mockCmd = {
      fileId: 2,
      hasCompileCommand: true,
      isAutoDetected: false,
      isWorkspaceDefault: true,
      compileCommand: {
        directory: "/ws",
        file: "main.c",
        arguments: ["clang", "-Iinclude"],
        output: null,
        languageStandard: "c17",
        defines: [],
        includeDirs: ["/ws/include"],
      },
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockCmd,
    } as any);

    const result = await client.getFileCompileCommand(1, 2);
    expect(result.fileId).toBe(2);
    expect(result.isWorkspaceDefault).toBe(true);
    expect(result.compileCommand?.languageStandard).toBe("c17");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/files/2/compile-command",
      expect.any(Object)
    );
  });

  it("fetches workspace compile commands summary", async () => {
    const mockSummary = {
      configuredPath: "compile_commands.json",
      effectivePath: "/ws/compile_commands.json",
      exists: true,
      isAutoDetected: false,
      totalCommands: 42,
      defaultCompileCommand: "clang -Iinclude -DDEBUG=1",
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockSummary,
    } as any);

    const result = await client.getWorkspaceCompileCommands(1);
    expect(result.exists).toBe(true);
    expect(result.defaultCompileCommand).toBe("clang -Iinclude -DDEBUG=1");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/compile-commands",
      expect.any(Object)
    );
  });

  it("updates workspace with defaultCompileCommand via PATCH", async () => {
    const mockWs = {
      id: 1,
      name: "New Name",
      rootPath: "/ws",
      compileCommandsPath: "build/compile_commands.json",
      defaultCompileCommand: "clang -Iinc",
      status: "idle",
      revision: 2,
      createdAt: "",
      updatedAt: "",
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockWs,
    } as any);

    const result = await client.updateWorkspace(1, {
      name: "New Name",
      defaultCompileCommand: "clang -Iinc",
    });
    expect(result.defaultCompileCommand).toBe("clang -Iinc");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1",
      expect.objectContaining({
        method: "PATCH",
        body: JSON.stringify({ name: "New Name", defaultCompileCommand: "clang -Iinc" }),
      })
    );
  });

  it("creates workspace via POST", async () => {
    const mockCreated = {
      id: 2,
      name: "Project Two",
      rootPath: "/path/to/project2",
      status: "idle",
      revision: 1,
      createdAt: "",
      updatedAt: "",
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockCreated,
    } as any);

    const result = await client.createWorkspace({
      rootPath: "/path/to/project2",
      name: "Project Two",
      compileCommandsPath: "build/compile_commands.json",
    });
    expect(result.id).toBe(2);
    expect(result.name).toBe("Project Two");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({
          rootPath: "/path/to/project2",
          name: "Project Two",
          compileCommandsPath: "build/compile_commands.json",
        }),
      })
    );
  });

  it("deletes workspace via DELETE", async () => {
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => ({ status: "deleted", id: 2 }),
    } as any);

    const result = await client.deleteWorkspace(2);
    expect(result.status).toBe("deleted");
    expect(result.id).toBe(2);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/2",
      expect.objectContaining({
        method: "DELETE",
      })
    );
  });

  it("fetches libraries and creates a library profile", async () => {
    const mockLibs = {
      libraries: [
        {
          id: 1,
          workspaceId: 10,
          name: "glibc",
          language: "c",
          provider: "system",
          sourceRoots: ["/usr/include"],
          defaultIncludeRoots: [],
          defines: [],
          includePatterns: [],
          excludePatterns: [],
          fingerprint: "abc",
          status: "idle",
          createdAt: "",
          updatedAt: "",
        },
      ],
      total: 1,
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockLibs,
    } as any);

    const libs = await client.getLibraries();
    expect(libs.libraries).toHaveLength(1);
    expect(libs.libraries[0].name).toBe("glibc");

    const newLib = {
      id: 2,
      workspaceId: 11,
      name: "libstdc++",
      language: "cpp",
      provider: "gcc",
      sourceRoots: ["/usr/include/c++/12"],
      defaultIncludeRoots: [],
      defines: [],
      includePatterns: [],
      excludePatterns: [],
      fingerprint: "def",
      status: "idle",
      createdAt: "",
      updatedAt: "",
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => newLib,
    } as any);

    const created = await client.createLibrary({
      name: "libstdc++",
      language: "cpp",
      sourceRoots: ["/usr/include/c++/12"],
      provider: "gcc",
    });
    expect(created.id).toBe(2);
    expect(created.name).toBe("libstdc++");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/libraries",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({
          name: "libstdc++",
          language: "cpp",
          sourceRoots: ["/usr/include/c++/12"],
          provider: "gcc",
        }),
      })
    );
  });

  it("sends C# target framework metadata when creating a library profile", async () => {
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => ({
        id: 4,
        workspaceId: 14,
        name: "Reference sources",
        language: "csharp",
        provider: "dotnet",
        targetFramework: "net8.0",
        sourceRoots: ["/sdk/ref"],
        defaultIncludeRoots: [],
        defines: [],
        includePatterns: [],
        excludePatterns: [],
        fingerprint: "csharp-fp",
        status: "idle",
        createdAt: "",
        updatedAt: "",
      }),
    } as any);

    const created = await client.createLibrary({
      name: "Reference sources",
      language: "csharp",
      sourceRoots: ["/sdk/ref"],
      provider: "dotnet",
      targetFramework: "net8.0",
    });
    expect(created.targetFramework).toBe("net8.0");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/libraries",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({
          name: "Reference sources",
          language: "csharp",
          sourceRoots: ["/sdk/ref"],
          provider: "dotnet",
          targetFramework: "net8.0",
        }),
      })
    );
  });

  it("deletes and indexes a library profile", async () => {
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => ({ status: "deleted", id: 3 }),
    } as any);

    const delRes = await client.deleteLibrary(3);
    expect(delRes.status).toBe("deleted");
    expect(delRes.id).toBe(3);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/libraries/3",
      expect.objectContaining({
        method: "DELETE",
      })
    );

    const mockJob = { id: 55, workspaceId: 11, jobType: "full", status: "queued" };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockJob,
    } as any);

    const job = await client.indexLibrary(3);
    expect(job.id).toBe(55);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/libraries/3/index",
      expect.objectContaining({
        method: "POST",
      })
    );
  });

  it("attaches and detaches library from workspace", async () => {
    const mockAttached = {
      id: 1,
      workspaceId: 10,
      name: "glibc",
      language: "c",
      provider: "system",
      sourceRoots: ["/usr/include"],
      defaultIncludeRoots: [],
      defines: [],
      includePatterns: [],
      excludePatterns: [],
      fingerprint: "abc",
      status: "idle",
      createdAt: "",
      updatedAt: "",
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockAttached,
    } as any);

    const attachRes = await client.attachLibrary(1, 1);
    expect(attachRes.id).toBe(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/libraries",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({ profileId: 1 }),
      })
    );

    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => ({ status: "detached" }),
    } as any);

    const detachRes = await client.detachLibrary(1, 1);
    expect(detachRes.status).toBe("detached");
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/libraries/1",
      expect.objectContaining({
        method: "DELETE",
      })
    );
  });
});
