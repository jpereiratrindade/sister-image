#!/usr/bin/env bash
set -euo pipefail
export SISTER_IMAGE_ACCESS_MODE=local
exec "$(dirname "${BASH_SOURCE[0]}")/runtime.sh" "${1:-start}"
