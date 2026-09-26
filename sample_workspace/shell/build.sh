#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/utils.sh"

OUTPUT_DIR="${1:-dist}"
BUILD_VERSION="1.0.0"

clean_build_artifacts() {
    log_info "Cleaning output directory: ${OUTPUT_DIR}"
    rm -rf "${OUTPUT_DIR}"
    ensure_dir "${OUTPUT_DIR}"
}

compile_pipeline() {
    local target="$1"
    log_info "Compiling component: ${target} (v${BUILD_VERSION})"
    touch "${OUTPUT_DIR}/${target}.bundle"
}

main() {
    log_info "Starting sample workspace build process..."
    clean_build_artifacts
    compile_pipeline "core"
    compile_pipeline "telemetry"
    log_info "Build finished successfully."
}

main "$@"
