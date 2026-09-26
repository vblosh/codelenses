#!/bin/bash

# Static source directives
source ./lib/helpers.sh
. ./config/settings.sh
source "lib/common.sh"

# Dynamic source directives
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/modules/extra.sh"
source "$DYNAMIC_CONFIG"

init_app() {
    setup_helpers
    load_settings
}

init_app
