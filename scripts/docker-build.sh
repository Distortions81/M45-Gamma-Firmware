#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

IMAGE="${M45_DOCKER_IMAGE:-m45-gamma-firmware-build}"
IDF_VERSION="${M45_IDF_VERSION:-v5.5.3}"
BUILD_IMAGE=1
FORWARD_ARGS=()
FLASH_PORTS=()
BUILD_DIR_SET=0

usage() {
    cat <<EOF
Usage:
  scripts/docker-build.sh [docker options] [build.sh options]

Default action:
  Build the firmware with scripts/build.sh --build.

Docker options:
  --docker-image NAME   Image name to build and run (default: $IMAGE)
  --idf-version TAG     espressif/idf tag to use (default: $IDF_VERSION)
  --no-image-build      Reuse an existing local image
  -h, --help            Show this help

Docker builds use build/docker by default. Pass --build-dir to override it.

Examples:
  scripts/docker-build.sh
  scripts/docker-build.sh --flash /dev/ttyUSB0
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

device_gid() {
    local path="$1"
    stat -c '%g' "$path" 2>/dev/null || stat -f '%g' "$path" 2>/dev/null || true
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --docker-image)
            need_value "$1" "${2-}"
            IMAGE="$2"
            shift 2
            ;;
        --docker-image=*)
            IMAGE="${1#*=}"
            shift
            ;;
        --idf-version)
            need_value "$1" "${2-}"
            IDF_VERSION="$2"
            shift 2
            ;;
        --idf-version=*)
            IDF_VERSION="${1#*=}"
            shift
            ;;
        --no-image-build)
            BUILD_IMAGE=0
            shift
            ;;
        --flash)
            need_value "$1" "${2-}"
            FLASH_PORTS+=("$2")
            FORWARD_ARGS+=("$1" "$2")
            shift 2
            ;;
        --flash=*)
            value="${1#*=}"
            [[ -n "$value" ]] || die "--flash requires a value"
            FLASH_PORTS+=("$value")
            FORWARD_ARGS+=("$1")
            shift
            ;;
        --build-dir)
            need_value "$1" "${2-}"
            BUILD_DIR_SET=1
            FORWARD_ARGS+=("$1" "$2")
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR_SET=1
            FORWARD_ARGS+=("$1")
            shift
            ;;
        --wifi-ssid|--wifi-pass|--wifi-password|--pool-host|--pool-port|--pool-user|\
        --pool-pass|--pool-password|--diff|--difficulty|--hostname|--freq|\
        --frequency|--voltage|--oled-width|--oled-height|--oled-addr)
            die "Docker builds use repository defaults; configure Wi-Fi and pool from the device UI"
            ;;
        --wifi-ssid=*|--wifi-pass=*|--wifi-password=*|--pool-host=*|--pool-port=*|\
        --pool-user=*|--pool-pass=*|--pool-password=*|--diff=*|--difficulty=*|\
        --hostname=*|--freq=*|--frequency=*|--voltage=*|--oled-width=*|\
        --oled-height=*|--oled-addr=*)
            die "Docker builds use repository defaults; configure Wi-Fi and pool from the device UI"
            ;;
        --auto-diff|--difficulty-auto|--manual-diff|--no-auto-diff|--difficulty-manual)
            die "Docker builds use repository defaults; configure Wi-Fi and pool from the device UI"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            FORWARD_ARGS+=("$@")
            break
            ;;
        *)
            FORWARD_ARGS+=("$1")
            shift
            ;;
    esac
done

if [[ ${#FORWARD_ARGS[@]} -eq 0 ]]; then
    FORWARD_ARGS=(--build)
fi

if [[ "$BUILD_DIR_SET" -eq 0 ]]; then
    FORWARD_ARGS=(--build-dir build/docker "${FORWARD_ARGS[@]}")
fi

if [[ "$BUILD_IMAGE" -eq 1 ]]; then
    docker build \
        --build-arg "IDF_VERSION=$IDF_VERSION" \
        --tag "$IMAGE" \
        "$REPO_DIR"
fi

RUN_ARGS=(
    --rm
    --env HOME=/tmp/m45-home
    --env M45_SDKCONFIG=/tmp/m45-sdkconfig
    --volume "$REPO_DIR:$REPO_DIR"
    --workdir "$REPO_DIR"
    --user "$(id -u):$(id -g)"
)

if [[ -t 0 && -t 1 ]]; then
    RUN_ARGS+=(-it)
fi

for port in "${FLASH_PORTS[@]}"; do
    if [[ "$port" == /dev/* ]]; then
        [[ -e "$port" ]] || die "serial device not found: $port"
        RUN_ARGS+=(--device "$port:$port")
        gid="$(device_gid "$port")"
        if [[ -n "$gid" ]]; then
            RUN_ARGS+=(--group-add "$gid")
        fi
    fi
done

docker run "${RUN_ARGS[@]}" "$IMAGE" \
    bash -lc '
        mkdir -p "$HOME"
        git config --global --add safe.directory /opt/esp/idf >/dev/null 2>&1 || true
        git config --global --add safe.directory /opt/esp/idf/components/openthread/openthread >/dev/null 2>&1 || true
        exec "$@"
    ' \
    bash scripts/build.sh "${FORWARD_ARGS[@]}"
