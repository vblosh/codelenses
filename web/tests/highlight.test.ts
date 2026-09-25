import { describe, it, expect } from "vitest";
import {
  escapeHtml,
  splitHtmlLines,
  renderWithTreeSitterTokens,
  renderWithHighlightJs,
  highlightSource,
} from "../src/rendering/highlight";
import type { HighlightResponseDto } from "../src/types";

describe("Syntax highlighting", () => {
  it("escapes raw HTML characters safely", () => {
    expect(escapeHtml(`<div>"Hello" & 'World' < 5></div>`)).toBe(
      `&lt;div&gt;&quot;Hello&quot; &amp; &#39;World&#39; &lt; 5&gt;&lt;/div&gt;`
    );
  });

  it("balances open spans across line breaks in splitHtmlLines", () => {
    const rawHtml = `<span class="hljs-comment">/* line one\nline two */</span>`;
    const lines = splitHtmlLines(rawHtml);
    expect(lines).toHaveLength(2);
    expect(lines[0]).toBe(`<span class="hljs-comment">/* line one</span>`);
    expect(lines[1]).toBe(`<span class="hljs-comment">line two */</span>`);
  });

  it("renders with Tree-sitter tokens and escapes text", () => {
    const lines = ["const x = 42;"];
    const highlightDto: HighlightResponseDto = {
      fileId: 1,
      legend: {
        tokenTypes: ["keyword", "variable", "operator", "number"],
      },
      tokens: [
        { line: 0, startColumn: 0, length: 5, tokenType: 0 },
        { line: 0, startColumn: 6, length: 1, tokenType: 1 },
        { line: 0, startColumn: 8, length: 1, tokenType: 2 },
        { line: 0, startColumn: 10, length: 2, tokenType: 3 },
      ],
    };

    const rendered = renderWithTreeSitterTokens(lines, highlightDto);
    expect(rendered).toHaveLength(1);
    expect(rendered[0]).toContain(`<span class="tok tok-keyword">const</span>`);
    expect(rendered[0]).toContain(`<span class="tok tok-variable">x</span>`);
    expect(rendered[0]).toContain(`<span class="tok tok-operator">=</span>`);
    expect(rendered[0]).toContain(`<span class="tok tok-number">42</span>`);
  });

  it("renders with highlight.js as fallback", () => {
    const code = `int main() {\n    return 0;\n}`;
    const lines = renderWithHighlightJs(code, "cpp");
    expect(lines).toHaveLength(3);
    // highlight.js wraps keywords like int / return
    expect(lines[0]).toMatch(/class="hljs-/);
    expect(lines[1]).toMatch(/class="hljs-/);
  });

  it("highlightSource prioritizes Tree-sitter tokens and falls back to highlight.js", () => {
    const code = `def foo(): pass`;
    // With tokens:
    const dtoWithTokens: HighlightResponseDto = {
      fileId: 2,
      legend: { tokenTypes: ["keyword", "function"] },
      tokens: [{ line: 0, startColumn: 0, length: 3, tokenType: 0 }],
    };
    const withTokens = highlightSource(code, "python", dtoWithTokens);
    expect(withTokens[0]).toContain(`<span class="tok tok-keyword">def</span>`);

    // Without tokens:
    const fallback = highlightSource(code, "python", null);
    expect(fallback[0]).toMatch(/class="hljs-/);
  });

  it("applies Tree-sitter tokens on later pages of large files using startLine offset", () => {
    // Sliced content starting at line 500
    const slicedCode = "return value;";
    const highlightDto: HighlightResponseDto = {
      fileId: 3,
      legend: { tokenTypes: ["keyword", "variable"] },
      tokens: [
        { line: 500, startColumn: 0, length: 6, tokenType: 0 },
        { line: 500, startColumn: 7, length: 5, tokenType: 1 },
      ],
    };

    const rendered = highlightSource(slicedCode, "cpp", highlightDto, 500);
    expect(rendered).toHaveLength(1);
    expect(rendered[0]).toContain(`<span class="tok tok-keyword">return</span>`);
    expect(rendered[0]).toContain(`<span class="tok tok-variable">value</span>`);
  });
});
