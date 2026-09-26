# Sample Workspace

This workspace is an end-to-end multi-language test suite and demonstration workspace for **CodeLenses**. It contains source code files across all target language families supported or specified by CodeLenses, as well as metadata, configuration files, and assets to test file discovery, binary detection, ignore rules, and compilation database integration.

## Language Coverage

The workspace covers all nine language families targeted by CodeLenses (per `docs/requirements.md` FR-5):

1. **C (`c/`)**
   - Header files (`include/common.h`, `src/math_utils.h`) and implementation files (`src/math_utils.c`, `src/processor.c`).
   - Macros, enums, structs, typedefs, function prototypes and definitions, `#include` directives, and cross-file calls.

2. **C++ (`cpp/`)**
   - Header files (`include/buffer.hpp`, `include/filter.hpp`, `include/pipeline.hpp`) and implementation (`src/pipeline.cpp`).
   - Namespaces (`sample::engine`), classes, structs, template types, enum classes, type aliases (`using`), inheritance (`BaseFilter` -> `ThresholdFilter`), virtual methods, and method calls.

3. **C# (`csharp/`)**
   - Source files in `Models/` and `Services/`.
   - Namespaces (`SampleWorkspace.Sensor`), interfaces (`ISensor`), enum types (`SensorCategory`), classes (`TemperatureSensor`, `SensorMonitor`), fields, properties, interface implementation, methods, and `using` directives.

4. **Python (`python/`)**
   - Python package in `telemetry/`.
   - Modules, classes (`TelemetryEvent`, `DeviceState`, `EventProcessor`), methods, functions, decorators (`@timed_operation`), intra-package imports, and function calls.

5. **TypeScript & TSX (`typescript/`)**
   - TypeScript models (`src/models.ts`), service (`src/auth_service.ts`), and React TSX components (`src/components/UserBadge.tsx`).
   - Modules, interfaces (`UserAccount`, `SessionToken`), enums (`UserRole`), type unions (`Result<T>`), classes (`AuthService`), functions, JSX elements, and JSX component references.

6. **JavaScript & JSX (`javascript/`)**
   - JavaScript utilities (`src/helpers.js`) and React JSX view (`src/Notification.jsx`).
   - Classes (`Formatter`), helper functions (`truncate`, `debounce`), ES module export and CommonJS compatibility, JSX components, and calls.

7. **Go (`go/`)**
   - Package `queue` in `pkg/queue/` (`task.go`, `dispatcher.go`).
   - Packages, imports, types, structs (`Task`, `Dispatcher`), interfaces (`TaskHandler`), constants/iota (`TaskState`), functions, and pointer-receiver methods (`(d *Dispatcher) Submit(...)`).

8. **Java (`java/`)**
   - Java package `com.example.billing` (`InvoiceStatus.java`, `Billable.java`, `Invoice.java`).
   - Package statements, imports, enums, interfaces, classes implementing interfaces (`Invoice implements Billable`), fields, constructors, and methods.

9. **POSIX Shell & Bash (`shell/`)**
   - POSIX library script (`lib/utils.sh`).
   - Executable bash script (`build.sh`) with shebang and source directive (`source ...`).
   - Extensionless script with shebang (`healthcheck`).

## Metadata, Ignore, and Build Files

- **`compile_commands.json`**: Clang compilation database for C and C++ files, providing include directories and compiler flags.
- **`.gitignore`**: Defines ignored patterns (e.g. `ignored_dir/`, `node_modules/`, `*.log`, `build/`).
- **`.codelensignore`**: Demonstrates indexer-specific ignore rules (`temp_cache/`).
- **`ignored_dir/ignored_file.txt`**: Tests that directory ignore rules are respected.
- **`config/settings.json`**: Tests handling of non-code browseable files (`Language::unknown`).
- **`assets/binary.dat`**: Tests binary file detection (`quick_check_binary`).
