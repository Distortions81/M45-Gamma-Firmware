#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

IMAGE="${M45_DOCKER_IMAGE:-m45-gamma-firmware-build}"
IDF_VERSION="${M45_IDF_VERSION:-v5.5.3}"
BUILD_IMAGE=1
BUILD_DIR="build/docker"
OUTPUT_DIR="dist/ota"
VERSION=""
BOARD_VERSION="602"

usage() {
    cat <<EOF
Usage:
  scripts/docker-ota.sh [options]

Build firmware in Docker and collect local OTA upload files.

Output files:
  M45-Firmware.bin
  esp-miner-factory-${BOARD_VERSION}-*.bin
  sha256sums.txt
  version.txt

Options:
  --docker-image NAME   Image name to build and run (default: $IMAGE)
  --idf-version TAG     espressif/idf tag to use (default: $IDF_VERSION)
  --no-image-build      Reuse an existing local image
  --build-dir DIR       ESP-IDF build directory (default: $BUILD_DIR)
  --output DIR          OTA output directory (default: $OUTPUT_DIR)
  --version TEXT        Firmware version for factory image name
  --board-version N     Bitaxe board version for factory image naming (default: $BOARD_VERSION)
  -h, --help            Show this help
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
        --docker-image)
            need_value "$1" "${2-}"
            IMAGE="$2"
            shift 2
            ;;
        --docker-image=*)
            IMAGE="${1#*=}"
            [[ -n "$IMAGE" ]] || die "--docker-image requires a value"
            shift
            ;;
        --idf-version)
            need_value "$1" "${2-}"
            IDF_VERSION="$2"
            shift 2
            ;;
        --idf-version=*)
            IDF_VERSION="${1#*=}"
            [[ -n "$IDF_VERSION" ]] || die "--idf-version requires a value"
            shift
            ;;
        --no-image-build)
            BUILD_IMAGE=0
            shift
            ;;
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
        --board-version)
            need_value "$1" "${2-}"
            BOARD_VERSION="$2"
            shift 2
            ;;
        --board-version=*)
            BOARD_VERSION="${1#*=}"
            [[ -n "$BOARD_VERSION" ]] || die "--board-version requires a value"
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

docker run "${RUN_ARGS[@]}" "$IMAGE" \
    bash -lc '
        set -euo pipefail
        mkdir -p "$HOME"
        git config --global --add safe.directory /opt/esp/idf >/dev/null 2>&1 || true
        git config --global --add safe.directory /opt/esp/idf/components/openthread/openthread >/dev/null 2>&1 || true

        build_dir="$1"
        output_dir="$2"
        version="$3"
        board_version="$4"

        scripts/build.sh --build-dir "$build_dir" --build

        rm -rf "$output_dir"
        mkdir -p "$output_dir"
        cp "$build_dir/M45-Firmware.bin" "$output_dir/M45-Firmware.bin"

        package_dir="$(mktemp -d "$output_dir/.web-flasher.XXXXXX")"
        cleanup() {
            rm -rf "$package_dir"
        }
        trap cleanup EXIT

        package_args=(--build-dir "$build_dir" --output "$package_dir" --board-version "$board_version")
        if [[ -n "$version" ]]; then
            package_args+=(--version "$version")
        fi
        scripts/package-web-flasher.sh "${package_args[@]}"

        factory_image="$(find "$package_dir" -maxdepth 1 -type f -name "esp-miner-factory-${board_version}-*.bin" -print -quit)"
        [[ -n "$factory_image" ]] || {
            echo "error: factory OTA image was not produced" >&2
            exit 1
        }
        cp "$factory_image" "$output_dir/$(basename "$factory_image")"

        if [[ -f "$package_dir/version.txt" ]]; then
            cp "$package_dir/version.txt" "$output_dir/version.txt"
        elif [[ -n "$version" ]]; then
            printf "%s\n" "$version" > "$output_dir/version.txt"
        else
            git describe --tags --always > "$output_dir/version.txt"
        fi

        (
            cd "$output_dir"
            sha256sum M45-Firmware.bin esp-miner-factory-"$board_version"-*.bin > sha256sums.txt
        )

        echo "OTA update files: $output_dir"
        ls -lh "$output_dir"
    ' bash "$BUILD_DIR" "$OUTPUT_DIR" "$VERSION" "$BOARD_VERSION"
