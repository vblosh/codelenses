#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CHECK_ONLY=false
if [[ "${1:-}" == "--check" ]]; then
    CHECK_ONLY=true
fi

CLANG_FORMAT="${CLANG_FORMAT:-}"
if [[ -z "${CLANG_FORMAT}" ]]; then
    for candidate in clang-format clang-format-19 clang-format-18 clang-format-17; do
        if command -v "${candidate}" &>/dev/null; then
            CLANG_FORMAT="${candidate}"
            break
        fi
    done
fi

if [[ -z "${CLANG_FORMAT}" ]]; then
    echo "Error: clang-format not found in PATH." >&2
    exit 1
fi

echo "Using clang-format: ${CLANG_FORMAT}"

FILES=()
while IFS= read -r -d '' file; do
    FILES+=("$file")
done < <(find "${REPO_ROOT}/include" "${REPO_ROOT}/src" "${REPO_ROOT}/tests" \
    -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cpp" \) -print0 2>/dev/null || true)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C/C++ files found to format."
    exit 0
fi

if [[ "${CHECK_ONLY}" == "true" ]]; then
    echo "Checking format of ${#FILES[@]} files..."
    "${CLANG_FORMAT}" --dry-run --Werror -style=file "${FILES[@]}"
    echo "All files cleanly formatted!"
else
    echo "Formatting ${#FILES[@]} files..."
    "${CLANG_FORMAT}" -i -style=file "${FILES[@]}"
    echo "Formatting complete!"
fi
