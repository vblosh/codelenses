import type { WorkspaceIndexSettings } from "../types";

const fields: [keyof WorkspaceIndexSettings, string, boolean][] = [
  ["language", "Language / header mode (blank for automatic detection)", false],
  ["sourceRoots", "Source directories (one per line; blank uses workspace root)", true],
  ["defaultIncludeRoots", "Default include directories (one per line, in search order)", true],
  ["defines", "Compiler defines (one per line)", true],
  ["languageStandard", "Language standard", false],
  ["provider", "Provider", false],
  ["sdkVersion", "SDK version", false],
  ["targetEnvironment", "Target environment", false],
  ["targetFramework", "Target framework", false],
  ["sysroot", "Sysroot", false],
  ["includePatterns", "Include patterns (one per line)", true],
  ["excludePatterns", "Exclude patterns (one per line)", true],
];

export class IndexingSettingsForm {
  readonly element = document.createElement("details");
  private inputs = new Map<keyof WorkspaceIndexSettings, HTMLInputElement | HTMLTextAreaElement>();
  private configured = false;

  constructor() {
    const summary = document.createElement("summary");
    summary.textContent = "Advanced indexing settings";
    this.element.append(summary);
    for (const [key, title, multiline] of fields) {
      const label = document.createElement("label");
      label.className = "form-group";
      label.style.display = "block";
      label.textContent = title;
      const input = document.createElement(multiline ? "textarea" : "input");
      input.className = "form-input";
      input.dataset.setting = key;
      label.append(input);
      this.element.append(label);
      this.inputs.set(key, input);
    }
  }

  setValue(value?: WorkspaceIndexSettings | null): void {
    this.configured = !!value;
    for (const [key, , multiline] of fields) {
      const v = value?.[key];
      this.inputs.get(key)!.value = multiline && Array.isArray(v) ? v.join("\n") : typeof v === "string" ? v : "";
    }
  }

  getValue(): WorkspaceIndexSettings | undefined {
    const result: Record<string, string | string[]> = {};
    let populated = false;
    for (const [key, , multiline] of fields) {
      const value = this.inputs.get(key)!.value.trim();
      populated ||= !!value;
      result[key] = multiline ? value.split("\n").map(v => v.trim()).filter(Boolean) : value;
    }
    return populated || this.configured ? result as WorkspaceIndexSettings : undefined;
  }
}
