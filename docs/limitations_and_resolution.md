# Limitations and Language-Specific Resolution Behavior

This document details the semantic resolution behavior, confidence semantics, and known limitations across all programming languages supported by **CodeLenses**.

---

## 1. Overview and Architecture

CodeLenses adopts a **syntax-first, local-first** architectural model. Source parsing and syntax extraction are powered by [Tree-sitter](https://tree-sitter.github.io/tree-sitter/), ensuring deterministic performance, low latency, and robust error recovery on malformed or incomplete code.

Because Tree-sitter is a concrete syntax tree parser rather than a full semantic compiler frontend (such as `clang` or `javac`), semantic resolution is performed in a decoupled resolution phase by the `Resolver` and `SymbolResolver` engine.

### Core Principles

1. **Explicit Confidence, No Silent Discards:**
   Every reference, relation, and dependency candidate is tagged with an explicit resolution state and confidence score ($0.0 \le c \le 1.0$). References that cannot be resolved with certainty are retained with `unresolved`, `ambiguous`, or `external` states rather than silently omitted.
2. **Layered Resolution:**
   Resolution proceeds from innermost lexical scopes outward to explicit imports, project dependencies, and finally workspace-wide heuristics.
3. **Reproducibility:**
   Resolution decisions are deterministic and do not depend on external network services.

---

## 2. Resolution States and Confidence Matrix

The CodeLenses database schema and HTTP REST API return the following resolution states:

| State | Description | Typical Confidence |
| :--- | :--- | :--- |
| `resolved` | A single unambiguous symbol or dependency target was successfully identified. | $0.80$ – $1.00$ |
| `ambiguous` | Multiple valid candidate targets match the occurrence (e.g. function overloads, homonyms across packages). | $0.50$ |
| `external` | Target resides outside the indexed workspace (e.g. system headers, standard libraries, node_modules, NuGet packages). | $0.20$ – $0.80$ |
| `unresolved` | Occurrence could not be matched to any symbol in the workspace or known dependencies. | $0.00$ |

### Confidence Scoring Criteria

- **$1.00$ (Certain):**
  - Same-file exact lexical scope match (innermost shadowing).
  - Explicit fully qualified name match (e.g., `namespace::Class::method` or `module.function`) with unique definition.
  - Quoted include or relative import matching a single registered workspace file.
- **$0.90$ (High):**
  - Cross-file symbol match through an explicit import / include dependency path.
- **$0.80$ (Heuristic / Workspace Fallback):**
  - Unqualified name match with a unique definition across compatible languages in the workspace when no explicit import exists.
- **$0.50$ (Ambiguous):**
  - Multiple definitions exist within the same scope, import set, or workspace fallback (e.g., C++ function overloads, Python methods with the same name across unrelated classes).
- **$0.00$ (Unresolved):**
  - No matching symbol or file target found.

---

## 3. Language-Specific Resolution Semantics

### 3.1 C (`Language::c`)

#### Extraction & Resolution
- **Symbols Extracted:** Functions, global/static variables, structs, unions, enums, enum values, typedefs, macro definitions (`#define`), and `#include` directives.
- **Include Resolution:**
  - `#include "header.h"`: Resolved relative to the directory containing the source file first, then relative to user-specified quote include paths (`-iquote`).
  - `#include <header.h>`: Resolved via configured system include directories (`-I`, `-isystem`).
- **Compilation Database:**
  - When `compile_commands.json` is provided, per-file include paths and preprocessor definitions (`-D`) are applied during dependency resolution.
- **Cross-File Resolution:**
  - Non-static functions and global variables are resolved across translation units with language compatibility matching C and C++.

#### Known Limitations
1. **Preprocessor Macros:** Macros that expand into symbol definitions, declarations, or control flow are parsed in their unexpanded form. Macro-generated functions (e.g., `DECLARE_LIST(Type)`) are not syntactically unrolled.
2. **Conditional Compilation (`#if` / `#ifdef`):** All syntactic paths are parsed regardless of preprocessor branch conditions unless controlled by compile commands.
3. **Function Pointers:** Indirect calls through function pointers or callback tables cannot be resolved to their dynamic targets.
4. **Duplicate Static Symbols:** `static` file-scoped symbols with the same name in different `.c` files are isolated by file ID; unqualified external queries are flagged as `ambiguous`.

---

### 3.2 C++ (`Language::cpp`)

#### Extraction & Resolution
- **Symbols Extracted:** Namespaces, classes, structs, unions, template classes/functions, methods, constructors/destructors, enums, enum classes, type aliases (`using` / `typedef`), operator overloads, and friend declarations.
- **Relationships Extracted:** Class inheritance (`extends`), interface/abstract class implementation, `using namespace` imports, member method calls, constructor invocations, and header inclusions.
- **Qualification Matching:**
  - Fully and partially qualified names (`ns::Class::method`) are resolved by hierarchical token matching.
  - Same-file lexical scopes resolve methods to class bodies and class bodies to enclosing namespaces.

#### Known Limitations
1. **Templates and SFINAE:** Template specializations, recursive template metaprogramming, concepts, and expression SFINAE are not evaluated. Symbols are indexed by their unspecialized template names.
2. **Argument-Dependent Lookup (ADL):** Function call resolution relies on lexical scope and namespace hierarchy rather than full ADL based on argument types.
3. **Overload Disambiguation:** Without full type inference, multiple overloads sharing the same name within a scope are returned with resolution `ambiguous` and confidence $0.5$.
4. **Virtual Dispatch:** Method calls on polymorphic base pointers resolve to the base declaration rather than dynamic runtime overrides; overrides are navigable through the inheritance relationship graph.

---

### 3.3 C# (`Language::csharp`)

#### Extraction & Resolution
- **Symbols Extracted:** Namespaces, classes, interfaces, structs, records, enums, methods, properties, fields, events, delegates, and `using` directives.
- **Relationships Extracted:** Class inheritance, interface implementation, method invocation, and property accesses.
- **Scope Resolution:** Dot-separated hierarchical scopes (`Namespace.SubNamespace.Class.Method`) with `using` directive resolution across the workspace.

#### Known Limitations
1. **Partial Classes:** Partial type definitions across multiple files are indexed as distinct symbol occurrences linked by identical qualified names, but member merging is syntactic rather than unified in a single AST.
2. **External NuGet Packages / BCL:** Standard library types (`System.String`, `System.Collections.Generic.List`) and NuGet assemblies are identified as `external` without decompilation.
3. **Dynamic / Reflection:** Invocations via `dynamic`, `System.Reflection`, or source generators are marked `unresolved`.

---

### 3.4 Python (`Language::python`)

#### Extraction & Resolution
- **Symbols Extracted:** Modules, classes, functions, async functions, methods, class/instance variables, decorators, and import statements (`import foo.bar`, `from foo import bar as baz`).
- **Import Resolution:**
  - Absolute imports: resolved against the workspace root and package roots containing `__init__.py`.
  - Relative imports: leading dots (`.`, `..`) are resolved relative to the containing package directory.
- **Confidence Modeling:**
  - Attribute calls on `self` or explicit instance variables match enclosing class methods with high confidence ($0.9$–$1.0$).
  - Dynamic chained attribute references (e.g. `obj.factory().process()`) are classified as heuristic workspace matches ($0.8$ or $0.5$ if ambiguous).

#### Known Limitations
1. **Dynamic Dispatch & Duck Typing:** Python's dynamic runtime does not enforce static types; calls on untyped parameters match methods by name across the workspace.
2. **Runtime Monkey-Patching & `sys.path` Manipulation:** Runtime mutations to `sys.modules`, `sys.path`, or dynamic `__getattr__` / `importlib.import_module()` calls are outside static analysis scope.
3. **Virtualenv / Site-Packages:** Third-party packages installed in virtual environments (`venv`, `.venv`) are treated as `external` and excluded by default ignore policies.

---

### 3.5 TypeScript & JavaScript (`Language::typescript`, `Language::javascript`)

#### Extraction & Resolution
- **Symbols Extracted:** Modules, classes, interfaces, type aliases, enums, functions, arrow functions, generator functions, methods, exported constants, and CommonJS / ES module imports.
- **Specifier Resolution:**
  - Relative paths: `./file`, `../dir/file` with automatic extension probing (`.ts`, `.tsx`, `.js`, `.jsx`, `.d.ts`) and directory `index` resolution (`index.ts`, `index.js`).
  - Path mappings: `tsconfig.json` / `jsconfig.json` `compilerOptions.baseUrl` and `compilerOptions.paths` aliases (e.g. `@/*` $\to$ `src/*`).
- **JSX/TSX Support:** Custom components (`<MyComponent />`) are extracted and linked to their component function or class definitions.

#### Known Limitations
1. **Dynamic `require()` and `import()`:** Variable or expression imports (e.g. `require(pkgName)`) cannot be statically resolved.
2. **Flow & Complex Conditional Types:** Advanced mapped types, conditional types, and control-flow based type narrowing are not evaluated.
3. **`node_modules` Dependencies:** Packages under `node_modules` are categorized as `external` and ignored during indexing to maintain performance.

---

### 3.6 Go (`Language::go`)

#### Extraction & Resolution
- **Symbols Extracted:** Package declarations, imports, structs, interfaces, functions, methods with receiver types, constants, variables, and type definitions.
- **Module & Package Resolution:**
  - Module path extraction from `go.mod`.
  - Imports matching module prefixes (`github.com/org/repo/pkg`) are resolved to their workspace subdirectories.
  - Multi-file package support: all files sharing `package foo` within a directory share package scope.

#### Known Limitations
1. **Implicit Interface Implementation:** Go interfaces are satisfied implicitly without `implements` keywords; structural satisfaction is indexed where syntactically discernible, but complex method sets may be labeled `ambiguous`.
2. **Build Tags (`//go:build`):** Architecture- or OS-specific files (`_linux.go`, `_windows.go`) are indexed collectively, which may introduce homonyms across build targets.
3. **External Dependencies:** Vendor or `$GOPATH`/`pkg/mod` modules outside the workspace directory are classified as `external`.

---

### 3.7 Java (`Language::java`)

#### Extraction & Resolution
- **Symbols Extracted:** Packages, imports (single-type and wildcard `*`), classes, interfaces, enums, record types, methods, constructors, fields, and annotations.
- **Package Hierarchy:** Directory structure is mapped to package specifiers (`package com.example.service;`).
- **Inheritance & Overrides:** `@Override` annotations, `extends`, and `implements` link subclasses to parent classes and interfaces.

#### Known Limitations
1. **Wildcard Imports:** Wildcard imports (`import com.example.util.*;`) resolve against all types declared within the target package, tagging conflicts as `ambiguous`.
2. **Classpath / External JARs:** JDK classes (`java.lang.*`, `java.util.*`) and Maven/Gradle dependencies are marked `external`.
3. **Overload Resolution without Full Types:** Methods with identical names but differing parameter types are flagged `ambiguous` with confidence $0.5$ in the absence of complete argument type deduction.

---

### 3.8 POSIX Shell & Bash (`Language::shell`, `Language::bash`)

#### Extraction & Resolution
- **Symbols Extracted:** Function declarations (`function foo()` and `foo()`), environment and local variable assignments (`VAR=val`), `export` statements, and `source` / `.` directives.
- **Source Resolution:**
  - Direct static paths (`source ./scripts/helper.sh`) are resolved relative to the script directory or workspace root.
- **Command Occurrences:** Calls to script functions, built-in utilities, and external executables are captured.

#### Known Limitations
1. **Dynamic Path Expansion:** Dynamic variable expansions in source paths (e.g. `source "$BASE_DIR/lib.sh"` or `source "$(dirname "$0")/util.sh"`) cannot be resolved statically and are marked `unresolved`.
2. **Subshell & Dynamic Scopes:** Shell variables are dynamically scoped; variable references inside functions or pipelines do not enforce lexical boundaries.
3. **Built-ins vs Executables:** Built-in shell commands (e.g., `cd`, `echo`, `test`) are identified heuristically but cannot account for user-specific shell aliases.

---

## 4. Cross-Language Resolution

CodeLenses allows multi-language polyglot workspaces. The resolver enforces **Language Compatibility** rules to avoid invalid cross-language matches:

| Source Occurrence Language | Permitted Target Symbol Languages |
| :--- | :--- |
| **C** | C, C++ |
| **C++** | C++, C |
| **TypeScript** | TypeScript, JavaScript |
| **JavaScript** | JavaScript, TypeScript |
| **Shell / Bash** | Shell, Bash |
| **C#** | C# |
| **Python** | Python |
| **Go** | Go |
| **Java** | Java |

Occurrences will **never** resolve to symbols in incompatible languages (for example, a Python call to `open()` will never resolve to a C `open` function or Go `Open` method).

---

## 5. Summary and Guidance for Clients

When building user interfaces or downstream integrations with the CodeLenses API:
- Display resolution state badges (`Resolved`, `Ambiguous`, `External`, `Unresolved`) alongside reference lists.
- For `ambiguous` occurrences, present all candidate targets to the user with their respective file locations and ranking.
- For `external` occurrences, indicate that the definition resides in an external library or standard runtime header.
- Provide compile command context (`compile_commands.json`) whenever indexing C or C++ codebases for optimal include precision.
