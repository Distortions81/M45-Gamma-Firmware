#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="build/docker"
OUTPUT_DIR="dist/web-flasher"
VERSION=""

usage() {
    cat <<'EOF'
Usage:
  scripts/package-web-flasher.sh [options]

Options:
  --build-dir DIR   ESP-IDF build directory (default: build/docker)
  --output DIR      Web flasher output directory (default: dist/web-flasher)
  --version TEXT    Firmware version for manifest and page
  -h, --help        Show this help
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

need_value() {
    local opt="$1"
    local value="${2-}"
    [[ -n "$value" ]] || die "$opt requires a value"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            need_value "$1" "${2-}"
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR="${1#*=}"
            [[ -n "$BUILD_DIR" ]] || die "--build-dir requires a value"
            shift
            ;;
        --output)
            need_value "$1" "${2-}"
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --output=*)
            OUTPUT_DIR="${1#*=}"
            [[ -n "$OUTPUT_DIR" ]] || die "--output requires a value"
            shift
            ;;
        --version)
            need_value "$1" "${2-}"
            VERSION="$2"
            shift 2
            ;;
        --version=*)
            VERSION="${1#*=}"
            [[ -n "$VERSION" ]] || die "--version requires a value"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

cd "$REPO_DIR"

ARGS=(
    --build-dir "$BUILD_DIR"
    --output "$OUTPUT_DIR"
)

if [[ -n "$VERSION" ]]; then
    ARGS+=(--version "$VERSION")
fi

python3 scripts/tools/package_web_flasher.py "${ARGS[@]}"
