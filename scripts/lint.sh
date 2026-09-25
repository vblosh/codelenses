#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/build/debug}"

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    if [[ -f "${REPO_ROOT}/build/compile_commands.json" ]]; then
        BUILD_DIR="${REPO_ROOT}/build"
    else
        echo "Error: compile_commands.json not found in ${BUILD_DIR}." >&2
        echo "Please configure the project first (e.g., cmake --preset debug)." >&2
        exit 1
    fi
fi

CLANG_TIDY="${CLANG_TIDY:-}"
if [[ -z "${CLANG_TIDY}" ]]; then
    for candidate in clang-tidy clang-tidy-19 clang-tidy-18 clang-tidy-17; do
        if command -v "${candidate}" &>/dev/null; then
            CLANG_TIDY="${candidate}"
            break
        fi
    done
fi

if [[ -z "${CLANG_TIDY}" ]]; then
    echo "Error: clang-tidy not found in PATH." >&2
    exit 1
fi

echo "Using clang-tidy: ${CLANG_TIDY}"
echo "Using compilation database: ${BUILD_DIR}/compile_commands.json"

FILES=()
while IFS= read -r -d '' file; do
    if grep -q "\"${file}\"" "${BUILD_DIR}/compile_commands.json" 2>/dev/null; then
        FILES+=("$file")
    fi
done < <(find "${REPO_ROOT}/src" "${REPO_ROOT}/tests" \
    -type f -name "*.cpp" -print0 2>/dev/null || true)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C++ files found to lint."
    exit 0
fi

echo "Running clang-tidy on ${#FILES[@]} files..."
"${CLANG_TIDY}" -p "${BUILD_DIR}" --config-file="${REPO_ROOT}/.clang-tidy" --extra-arg=-Wno-unknown-warning-option "${FILES[@]}"
echo "Static analysis complete!"
