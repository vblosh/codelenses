#!/bin/sh

# Shell utility functions for sample workspace
APP_ENV="${APP_ENV:-development}"
DEFAULT_TIMEOUT=30

log_info() {
    printf "[INFO] %s\n" "$1"
}

log_error() {
    printf "[ERROR] %s\n" "$1" >&2
}

ensure_dir() {
    target_dir="$1"
    if [ ! -d "$target_dir" ]; then
        mkdir -p "$target_dir"
    fi
}
