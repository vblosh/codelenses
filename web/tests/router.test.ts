import { describe, it, expect } from "vitest";
import { parseHash, buildHash } from "../src/router";

describe("Router hash parsing and serialization", () => {
  it("parses empty or missing hash", () => {
    expect(parseHash("")).toEqual({
      workspaceId: null,
      fileId: null,
      line: null,
      symbolId: null,
    });
    expect(parseHash("#")).toEqual({
      workspaceId: null,
      fileId: null,
      line: null,
      symbolId: null,
    });
  });

  it("parses full route hash with numbers", () => {
    const hash = "#workspace=1&file=42&line=100&symbol=5";
    expect(parseHash(hash)).toEqual({
      workspaceId: 1,
      fileId: 42,
      line: 100,
      symbolId: 5,
    });
  });

  it("handles partial route parameters", () => {
    const hash = "#file=10&line=25";
    expect(parseHash(hash)).toEqual({
      workspaceId: null,
      fileId: 10,
      line: 25,
      symbolId: null,
    });
  });

  it("builds hash string from params", () => {
    const result = buildHash({
      workspaceId: 2,
      fileId: 15,
      line: 30,
      symbolId: 4,
    });
    expect(result).toBe("#workspace=2&file=15&line=30&symbol=4");
  });

  it("omits null or undefined values in hash", () => {
    const result = buildHash({
      workspaceId: 1,
      fileId: null,
      line: 12,
    });
    expect(result).toBe("#workspace=1&line=12");
  });

  it("returns empty string when all params are null", () => {
    expect(buildHash({})).toBe("");
    expect(buildHash({ workspaceId: null, fileId: null })).toBe("");
  });
});

import { StateStore } from "../src/state";
import { Router } from "../src/router";
import { vi } from "vitest";

describe("Router class and history integration", () => {
  it("initializes from hash and updates store", () => {
    window.location.hash = "#workspace=1&file=10&line=5";
    const store = new StateStore();
    const router = new Router(store);
    router.init();

    expect(store.getState().workspaceId).toBe(1);
    expect(store.getState().selectedFileId).toBe(10);
    expect(store.getState().selectedLine).toBe(5);
  });

  it("uses history.pushState when store changes to preserve back/forward navigation", () => {
    window.location.hash = "#workspace=1";
    const store = new StateStore({ workspaceId: 1 });
    const router = new Router(store);
    router.init();

    const pushSpy = vi.spyOn(window.history, "pushState");
    store.selectFile(42, 100);

    expect(pushSpy).toHaveBeenCalled();
    const lastCall = pushSpy.mock.calls[pushSpy.mock.calls.length - 1];
    expect(lastCall[2]).toContain("#workspace=1&file=42&line=100");
  });

  it("syncs state on window popstate event", () => {
    const store = new StateStore({ workspaceId: 1 });
    const router = new Router(store);
    router.init();

    window.location.hash = "#workspace=2&file=20";
    window.dispatchEvent(new PopStateEvent("popstate"));

    expect(store.getState().workspaceId).toBe(2);
    expect(store.getState().selectedFileId).toBe(20);
  });
});
