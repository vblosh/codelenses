import { describe, it, expect, vi } from "vitest";
import {
  createSourceLineElement,
  renderSourceLines,
} from "../src/rendering/source-lines";
import type { DiagnosticItem } from "../src/types";

describe("Source lines rendering", () => {
  it("creates a single code line element with data-line and correct markup", () => {
    const onClick = vi.fn();
    const elem = createSourceLineElement(
      42,
      `<span>code</span>`,
      false,
      undefined,
      onClick
    );

    expect(elem.className).toBe("code-line");
    expect(elem.dataset.line).toBe("42");

    const lineNum = elem.querySelector(".line-number");
    expect(lineNum).not.toBeNull();
    expect(lineNum?.textContent).toBe("42");

    const content = elem.querySelector(".line-content");
    expect(content).not.toBeNull();
    expect(content?.innerHTML).toBe("<span>code</span>");

    elem.click();
    expect(onClick).toHaveBeenCalledWith(42);
  });

  it("adds selected class when isSelected is true", () => {
    const elem = createSourceLineElement(10, "hello", true);
    expect(elem.classList.contains("selected")).toBe(true);
  });

  it("renders diagnostic markers in gutter when diagnostics are present", () => {
    const diags: DiagnosticItem[] = [
      { line: 5, severity: "error", message: "Syntax error" },
    ];
    const elem = createSourceLineElement(5, "errLine", false, diags);

    expect(elem.classList.contains("has-error")).toBe(true);
    const marker = elem.querySelector(".line-diag-marker");
    expect(marker).not.toBeNull();
    expect(marker?.classList.contains("has-error")).toBe(true);
    expect(marker?.getAttribute("title")).toContain("[ERROR] Syntax error");
  });

  it("renders multiple source lines into a container with correct start offset", () => {
    const container = document.createElement("div");
    const lines = ["line1", "line2", "line3"];

    renderSourceLines(container, {
      highlightedLines: lines,
      startLineNumber: 10,
      selectedLine: 11,
    });

    const rendered = container.querySelectorAll<HTMLElement>(".code-line");
    expect(rendered).toHaveLength(3);
    expect(rendered[0].dataset.line).toBe("10");
    expect(rendered[1].dataset.line).toBe("11");
    expect(rendered[2].dataset.line).toBe("12");

    expect(rendered[1].classList.contains("selected")).toBe(true);
    expect(rendered[0].classList.contains("selected")).toBe(false);
  });
});
