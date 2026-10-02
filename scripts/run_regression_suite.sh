#!/usr/bin/env bash
# ==============================================================================
# CodeLenses Pre-Release Regression Suite Runner
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build"
SKIP_FORMAT=false
SKIP_PACKAGE=false
RUN_BENCHMARKS=false
VERBOSE=false

print_help() {
    cat <<EOF
CodeLenses Release Regression Suite Runner

Usage: $0 [options]

Options:
  --build-dir <dir>    Specify build directory (default: ${REPO_ROOT}/build)
  --skip-format        Skip clang-format check
  --skip-package       Skip CPack packaging verification
  --run-benchmarks     Run optional benchmarks (requires CODELENSES_BUILD_BENCHMARKS=ON)
  --verbose            Enable verbose test output
  -h, --help           Display this help message and exit
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --skip-format)
            SKIP_FORMAT=true
            shift
            ;;
        --skip-package)
            SKIP_PACKAGE=true
            shift
            ;;
        --run-benchmarks)
            RUN_BENCHMARKS=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            print_help
            exit 1
            ;;
    esac
done

cd "${REPO_ROOT}"

echo "================================================================================"
echo " Starting CodeLenses Pre-Release Regression Suite"
echo " Build Directory: ${BUILD_DIR}"
echo " Timestamp:       $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
echo "================================================================================"

# Stage 1: Code Formatting
if [[ "${SKIP_FORMAT}" == "false" ]]; then
    echo ""
    echo "[Stage 1/5] Checking Code Formatting..."
    if command -v clang-format &>/dev/null; then
        ./scripts/format.sh --check
        echo "✓ Code formatting check passed."
    else
        echo "⚠ clang-format not found in PATH; skipping format check."
    fi
else
    echo ""
    echo "[Stage 1/5] Skipping Code Formatting check (--skip-format)."
fi

# Stage 2: Compilation & Test Build
echo ""
echo "[Stage 2/5] Building Project and Test Suites..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"
echo "✓ Build completed successfully."

# Stage 3: Core CTest Regression Suite
echo ""
echo "[Stage 3/5] Executing Unit, Integration, and Migration Tests..."
CTEST_ARGS=(--test-dir "${BUILD_DIR}" --output-on-failure)
if [[ "${VERBOSE}" == "true" ]]; then
    CTEST_ARGS+=(-V)
fi

ctest "${CTEST_ARGS[@]}" --parallel "$(nproc)"
echo "✓ All automated unit, integration, and migration tests passed."

# Stage 4: Performance & Footprint Regression Thresholds
if [[ "${RUN_BENCHMARKS}" == "true" ]]; then
    echo ""
    echo "[Stage 4/5] Executing Performance & Footprint Benchmarks..."
    BENCHMARK_BINARY="${BUILD_DIR}/tests/codelenses_benchmarks"
    if [[ ! -x "${BENCHMARK_BINARY}" ]]; then
        echo "✗ Benchmark executable not found. Configure with -DCODELENSES_BUILD_BENCHMARKS=ON and rebuild." >&2
        exit 1
    fi
    "${BENCHMARK_BINARY}" "[benchmark]"
    echo "✓ All performance benchmarks satisfied regression thresholds."
else
    echo ""
    echo "[Stage 4/5] Skipping optional performance benchmarks (use --run-benchmarks to enable)."
fi

# Stage 5: Release Packaging & Sanity Check
if [[ "${SKIP_PACKAGE}" == "false" ]]; then
    echo ""
    echo "[Stage 5/5] Generating and Verifying Release Packages..."
    cmake --build "${BUILD_DIR}" --target package

    TAR_PKG=$(find "${BUILD_DIR}" -maxdepth 1 -name "codelenses-*-Linux.tar.gz" | head -n 1)
    DEB_PKG=$(find "${BUILD_DIR}" -maxdepth 1 -name "codelenses-*-Linux.deb" | head -n 1)
    SH_PKG=$(find "${BUILD_DIR}" -maxdepth 1 -name "codelenses-*-Linux.sh" | head -n 1)

    if [[ -z "${TAR_PKG}" || ! -f "${TAR_PKG}" ]]; then
        echo "✗ Release tarball not generated!" >&2
        exit 1
    fi
    echo "✓ Release tarball verified: $(basename "${TAR_PKG}")"

    if [[ -n "${DEB_PKG}" && -f "${DEB_PKG}" ]]; then
        echo "✓ Release Debian package verified: $(basename "${DEB_PKG}")"
    fi

    if [[ -n "${SH_PKG}" && -f "${SH_PKG}" ]]; then
        echo "✓ Release self-extracting archive verified: $(basename "${SH_PKG}")"
    fi

    # Verify tarball contents
    if ! tar -tzf "${TAR_PKG}" | grep "bin/codelenses" >/dev/null; then
        echo "✗ Packaging defect: bin/codelenses missing from archive" >&2
        exit 1
    fi
    if ! tar -tzf "${TAR_PKG}" | grep "share/codelenses/codelenses.sample.json" >/dev/null; then
        echo "✗ Packaging defect: sample configuration missing from archive" >&2
        exit 1
    fi
    echo "✓ Archive structure and installed assets verified."
else
    echo ""
    echo "[Stage 5/5] Skipping Packaging verification (--skip-package)."
fi

echo ""
echo "================================================================================"
echo " ✓ CodeLenses Pre-Release Regression Suite PASSED Successfully!"
echo " System is verified and ready for release."
echo "================================================================================"
exit 0
