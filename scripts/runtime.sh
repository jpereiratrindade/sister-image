#!/usr/bin/env bash
set -euo pipefail
# Runtime consumes SISTER_RUNTIME_MODE, SISTER_RUNTIME_INSTANCE_ID,
# SISTER_RUNTIME_STATE_DIR, SISTER_RUNTIME_RUN_DIR, SISTER_RUNTIME_DATA_DIR,
# SISTER_RUNTIME_CLEANUP_SCOPE. The Python adapter enforces their isolation.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/runtime.py" "${1:-run}"
