#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
SDKCONFIG="$REPO_DIR/sdkconfig"
BUILD_DIR="${M45_BUILD_DIR:-build}"
IDF_EXPORT="${IDF_EXPORT_SCRIPT:-}"
BUILD=0
FLASH_PORT=""
MONITOR=0
SHOW=0
UPDATE_ITEMS=()

usage() {
    cat <<'EOF'
Usage:
  scripts/build.sh [options]

Wi-Fi:
  --wifi-ssid SSID
  --wifi-pass PASSWORD

Pool:
  --pool-host HOST
  --pool-port PORT
  --pool-user USER
  --pool-pass PASSWORD
  --diff DIFFICULTY
  --auto-diff
  --manual-diff

Device:
  --hostname NAME
  --freq MHZ
  --voltage MV
  --oled-width PX
  --oled-height PX
  --oled-addr HEX

Actions:
  --show
  --build
  --build-dir DIR
  --flash PORT
  --monitor
  --idf-export PATH

Example:
  scripts/build.sh \
    --wifi-ssid "My WiFi" \
    --wifi-pass "secret" \
    --pool-host public-pool.io \
    --pool-port 3333 \
    --pool-user "bc1q...worker" \
    --pool-pass x \
    --build
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

add_update() {
    local key="$1"
    local type="$2"
    local value="$3"
    UPDATE_ITEMS+=("$key|$type|$value")
}

is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --wifi-ssid)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_WIFI_SSID string "$2"
            shift 2
            ;;
        --wifi-ssid=*)
            add_update CONFIG_M45_BITAXE_WIFI_SSID string "${1#*=}"
            shift
            ;;
        --wifi-pass|--wifi-password)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_WIFI_PASSWORD string "$2"
            shift 2
            ;;
        --wifi-pass=*|--wifi-password=*)
            add_update CONFIG_M45_BITAXE_WIFI_PASSWORD string "${1#*=}"
            shift
            ;;
        --pool-host)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_STRATUM_HOST string "$2"
            shift 2
            ;;
        --pool-host=*)
            add_update CONFIG_M45_BITAXE_STRATUM_HOST string "${1#*=}"
            shift
            ;;
        --pool-port)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--pool-port must be an integer"
            add_update CONFIG_M45_BITAXE_STRATUM_PORT int "$2"
            shift 2
            ;;
        --pool-port=*)
            value="${1#*=}"
            is_uint "$value" || die "--pool-port must be an integer"
            add_update CONFIG_M45_BITAXE_STRATUM_PORT int "$value"
            shift
            ;;
        --pool-user)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_STRATUM_USER string "$2"
            shift 2
            ;;
        --pool-user=*)
            add_update CONFIG_M45_BITAXE_STRATUM_USER string "${1#*=}"
            shift
            ;;
        --pool-pass|--pool-password)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_STRATUM_PASS string "$2"
            shift 2
            ;;
        --pool-pass=*|--pool-password=*)
            add_update CONFIG_M45_BITAXE_STRATUM_PASS string "${1#*=}"
            shift
            ;;
        --diff|--difficulty)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--diff must be an integer"
            add_update CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY int "$2"
            shift 2
            ;;
        --diff=*|--difficulty=*)
            value="${1#*=}"
            is_uint "$value" || die "--diff must be an integer"
            add_update CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY int "$value"
            shift
            ;;
        --auto-diff|--difficulty-auto)
            add_update CONFIG_M45_BITAXE_STRATUM_DIFFICULTY_AUTO bool y
            shift
            ;;
        --manual-diff|--no-auto-diff|--difficulty-manual)
            add_update CONFIG_M45_BITAXE_STRATUM_DIFFICULTY_AUTO bool n
            shift
            ;;
        --hostname)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_HOSTNAME string "$2"
            shift 2
            ;;
        --hostname=*)
            add_update CONFIG_M45_BITAXE_HOSTNAME string "${1#*=}"
            shift
            ;;
        --freq|--frequency)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--freq must be an integer"
            add_update CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ int "$2"
            shift 2
            ;;
        --freq=*|--frequency=*)
            value="${1#*=}"
            is_uint "$value" || die "--freq must be an integer"
            add_update CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ int "$value"
            shift
            ;;
        --voltage)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--voltage must be an integer"
            add_update CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV int "$2"
            shift 2
            ;;
        --voltage=*)
            value="${1#*=}"
            is_uint "$value" || die "--voltage must be an integer"
            add_update CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV int "$value"
            shift
            ;;
        --oled-width)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--oled-width must be an integer"
            add_update CONFIG_M45_BITAXE_OLED_WIDTH int "$2"
            shift 2
            ;;
        --oled-width=*)
            value="${1#*=}"
            is_uint "$value" || die "--oled-width must be an integer"
            add_update CONFIG_M45_BITAXE_OLED_WIDTH int "$value"
            shift
            ;;
        --oled-height)
            need_value "$1" "${2-}"
            is_uint "$2" || die "--oled-height must be an integer"
            add_update CONFIG_M45_BITAXE_OLED_HEIGHT int "$2"
            shift 2
            ;;
        --oled-height=*)
            value="${1#*=}"
            is_uint "$value" || die "--oled-height must be an integer"
            add_update CONFIG_M45_BITAXE_OLED_HEIGHT int "$value"
            shift
            ;;
        --oled-addr)
            need_value "$1" "${2-}"
            add_update CONFIG_M45_BITAXE_OLED_I2C_ADDR hex "$2"
            shift 2
            ;;
        --oled-addr=*)
            add_update CONFIG_M45_BITAXE_OLED_I2C_ADDR hex "${1#*=}"
            shift
            ;;
        --idf-export)
            need_value "$1" "${2-}"
            IDF_EXPORT="$2"
            shift 2
            ;;
        --idf-export=*)
            IDF_EXPORT="${1#*=}"
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
        --build)
            BUILD=1
            shift
            ;;
        --flash)
            need_value "$1" "${2-}"
            FLASH_PORT="$2"
            BUILD=1
            shift 2
            ;;
        --flash=*)
            FLASH_PORT="${1#*=}"
            BUILD=1
            shift
            ;;
        --monitor)
            MONITOR=1
            shift
            ;;
        --show)
            SHOW=1
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

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -n "$IDF_EXPORT" ]]; then
        [[ -f "$IDF_EXPORT" ]] || die "IDF export script not found: $IDF_EXPORT"
        # shellcheck disable=SC1090
        . "$IDF_EXPORT" >/dev/null
    elif [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        # shellcheck disable=SC1091
        . "$IDF_PATH/export.sh" >/dev/null
    elif [[ -f /home/dist/esp/esp-idf/export.sh ]]; then
        # shellcheck disable=SC1091
        . /home/dist/esp/esp-idf/export.sh >/dev/null
    else
        die "idf.py is not on PATH; source ESP-IDF or pass --idf-export PATH"
    fi
fi

run_idf() {
    idf.py -B "$BUILD_DIR" "$@"
}

if [[ ! -f "$SDKCONFIG" ]]; then
    run_idf reconfigure
fi

if [[ ${#UPDATE_ITEMS[@]} -gt 0 ]]; then
    python3 - "$SDKCONFIG" "${UPDATE_ITEMS[@]}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
updates = []
for item in sys.argv[2:]:
    key, typ, value = item.split("|", 2)
    updates.append((key, typ, value))

def render_value(typ, value):
    if typ == "string":
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    if typ == "hex":
        value = value.strip()
        if not value.lower().startswith("0x"):
            value = "0x" + value
        int(value, 16)
        return value
    if typ == "int":
        number = int(value, 10)
        if number < 0:
            raise ValueError("negative values are not supported")
        return str(number)
    raise ValueError(f"unknown type: {typ}")

def render_line(key, typ, value):
    if typ == "bool":
        normalized = value.strip().lower()
        if normalized in {"1", "y", "yes", "true", "on"}:
            return f"{key}=y"
        if normalized in {"0", "n", "no", "false", "off"}:
            return f"# {key} is not set"
        raise ValueError(f"invalid bool: {value}")
    return f"{key}={render_value(typ, value)}"

lines = path.read_text().splitlines()
for key, typ, value in updates:
    rendered = render_line(key, typ, value)
    replaced = False
    for idx, line in enumerate(lines):
        if line.startswith(key + "=") or line == f"# {key} is not set":
            lines[idx] = rendered
            replaced = True
            break
    if not replaced:
        lines.append(rendered)

path.write_text("\n".join(lines) + "\n")
PY
fi

run_idf reconfigure

if [[ "$SHOW" -eq 1 ]]; then
    python3 - "$SDKCONFIG" <<'PY'
import pathlib
import sys

keys = [
    "CONFIG_M45_BITAXE_WIFI_SSID",
    "CONFIG_M45_BITAXE_WIFI_PASSWORD",
    "CONFIG_M45_BITAXE_STRATUM_HOST",
    "CONFIG_M45_BITAXE_STRATUM_PORT",
    "CONFIG_M45_BITAXE_STRATUM_USER",
    "CONFIG_M45_BITAXE_STRATUM_PASS",
    "CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY",
    "CONFIG_M45_BITAXE_STRATUM_DIFFICULTY_AUTO",
    "CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ",
    "CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV",
    "CONFIG_M45_BITAXE_OLED_WIDTH",
    "CONFIG_M45_BITAXE_OLED_HEIGHT",
    "CONFIG_M45_BITAXE_OLED_I2C_ADDR",
]

values = {}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    if line.startswith("# ") and line.endswith(" is not set"):
        key = line[2:-11]
        if key in keys:
            values[key] = "n"
        continue
    if "=" not in line:
        continue
    key, value = line.split("=", 1)
    if key in keys:
        values[key] = value

for key in keys:
    value = values.get(key, "<unset>")
    if key.endswith("PASSWORD") or key.endswith("PASS"):
        value = '"***"' if value not in {"", '""', "<unset>"} else value
    print(f"{key}={value}")
PY
fi

if [[ "$BUILD" -eq 1 ]]; then
    run_idf build
fi

if [[ -n "$FLASH_PORT" ]]; then
    if [[ "$MONITOR" -eq 1 ]]; then
        run_idf -p "$FLASH_PORT" flash monitor
    else
        run_idf -p "$FLASH_PORT" flash
    fi
elif [[ "$MONITOR" -eq 1 ]]; then
    run_idf monitor
fi
