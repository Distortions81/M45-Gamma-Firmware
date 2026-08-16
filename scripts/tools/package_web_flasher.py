#!/usr/bin/env python3
import argparse
import html
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys


DEFAULT_NAME = "M45 Gamma Firmware"
DEFAULT_BOARD_VERSION = "602"
REPO_URL = "https://github.com/M45Core/M45-Gamma-Firmware"
REPOSITORY = "M45Core/M45-Gamma-Firmware"
ESPTOOL_JS_URL = "https://unpkg.com/esptool-js@0.4.6/bundle.js"
NVS_START = 0x9000
NVS_SIZE = 0x6000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_MAGIC = b"\xaa\x50"
CANONICAL_FACTORY_OFFSET = 0x10000


def die(message):
    print(f"error: {message}", file=sys.stderr)
    return 1


def git_text(args, default):
    try:
        return subprocess.check_output(["git", *args], text=True).strip() or default
    except (OSError, subprocess.CalledProcessError):
        return default


def default_version():
    version = git_text(["describe", "--tags", "--always"], "dev")
    dirty = git_text(["status", "--porcelain"], "")
    return f"{version}-dirty" if dirty else version


def filename_version(version):
    return version.replace("/", "-").replace("\\", "-")


def default_firmware_name(board_version, version):
    return f"esp-miner-factory-{board_version}-{filename_version(version)}.bin"


def load_flash_args(build_dir):
    path = build_dir / "flasher_args.json"
    if not path.exists():
        raise FileNotFoundError(f"{path} does not exist; build firmware first")
    return json.loads(path.read_text())


def sorted_flash_files(flash_files):
    return sorted(flash_files.items(), key=lambda item: int(item[0], 0))


def flash_setting(flasher_args, name, default):
    return flasher_args.get("flash_settings", {}).get(name, default)


def merge_command(esptool, chip, flasher_args, output_path, parts):
    # Mirrors ESP-Miner merge_bin.sh: factory image starts at 0x0 and is flashed as one file.
    args = [
        *esptool,
        "--chip",
        chip,
        "merge_bin",
        "--flash_mode",
        flash_setting(flasher_args, "flash_mode", "dio"),
        "--flash_size",
        flash_setting(flasher_args, "flash_size", "16MB"),
        "--flash_freq",
        flash_setting(flasher_args, "flash_freq", "80m"),
    ]
    for part in parts:
        args.extend([f"0x{part['offset']:x}", str(part["source"])])
    args.extend(["-o", str(output_path)])
    return args


def run_merge(chip, flasher_args, output_path, parts):
    attempts = []
    if shutil.which("esptool.py"):
        attempts.append(["esptool.py"])
    attempts.append([sys.executable, "-m", "esptool"])

    last = None
    for esptool in attempts:
        command = merge_command(esptool, chip, flasher_args, output_path, parts)
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            if result.stdout.strip():
                print(result.stdout.rstrip())
            return
        last = result
    if last and last.stdout:
        print(last.stdout.rstrip(), file=sys.stderr)
    raise subprocess.CalledProcessError(last.returncode if last else 1, last.args if last else attempts[-1])


def embedded_canonical_partition_table(build_dir):
    header = build_dir / "esp-idf/main/migration_partition_table.h"
    if not header.exists():
        raise FileNotFoundError(f"missing canonical partition table: {header}")
    table = bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", header.read_text())
    )
    if not table.startswith(PARTITION_TABLE_MAGIC):
        raise ValueError("generated canonical partition table has invalid magic")
    return table


def canonical_factory_range(table):
    for offset in range(0, len(table), 32):
        magic, part_type, subtype, address, size = struct.unpack_from(
            "<HBBII", table, offset
        )
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            continue
        label = table[offset + 12 : offset + 28].split(b"\0", 1)[0]
        if label == b"factory" and part_type == 0 and subtype == 0:
            return address, size
    raise ValueError("canonical table has no factory application")


def validate_factory_image(path, canonical_table, app):
    if path.stat().st_size > 9 * 1024 * 1024:
        raise ValueError(f"{path} exceeds the v0.0.9 OTA upload limit")
    factory_offset, factory_size = canonical_factory_range(canonical_table)
    if factory_offset != CANONICAL_FACTORY_OFFSET or app.stat().st_size > factory_size:
        raise ValueError("canonical factory range cannot contain the bridge app")
    if path.stat().st_size < CANONICAL_FACTORY_OFFSET + app.stat().st_size:
        raise ValueError(f"{path} is too small for the canonical factory app")
    with path.open("rb") as firmware:
        firmware.seek(PARTITION_TABLE_OFFSET)
        table = firmware.read(len(canonical_table))
        firmware.seek(CANONICAL_FACTORY_OFFSET)
        embedded_app = firmware.read(app.stat().st_size)
    if table != canonical_table:
        raise ValueError(f"{path} does not contain the canonical partition table")
    if embedded_app != app.read_bytes():
        raise ValueError(f"{path} does not contain the exact canonical factory app")


def merge_flash_parts(build_dir, output_dir, firmware_name, flasher_args):
    app_filename = flasher_args["app"]["file"]
    chip = flasher_args.get("extra_esptool_args", {}).get("chip", "esp32s3")
    flash_files = sorted_flash_files(flasher_args["flash_files"])
    bootloader_name = next(
        (filename for offset, filename in flash_files if int(offset, 0) == 0), None
    )
    if bootloader_name is None:
        raise ValueError("merged factory image requires a bootloader at offset 0x0")
    bootloader = build_dir / bootloader_name
    app = build_dir / app_filename
    for source in (bootloader, app):
        if not source.exists():
            raise FileNotFoundError(f"missing flash part: {source}")

    canonical_table = embedded_canonical_partition_table(build_dir)
    table_file = output_dir / ".canonical-partition-table.bin"
    table_file.write_bytes(canonical_table)
    parts = [
        {"source": bootloader, "offset": 0},
        {"source": table_file, "offset": PARTITION_TABLE_OFFSET},
        {"source": app, "offset": CANONICAL_FACTORY_OFFSET},
    ]

    firmware = output_dir / firmware_name
    try:
        run_merge(chip, flasher_args, firmware, parts)
    finally:
        table_file.unlink(missing_ok=True)
    validate_factory_image(firmware, canonical_table, app)
    return firmware


def write_index(output_dir, name, version, firmware_name, board_version):
    config = {
        "name": name,
        "version": version,
        "boardVersion": board_version,
        "firmwareName": firmware_name,
        "repository": REPOSITORY,
        "repoUrl": REPO_URL,
        "nvsStart": NVS_START,
        "nvsSize": NVS_SIZE,
        "bundledRelease": {
            "version": version,
            "name": f"{version} bundled build",
            "path": firmware_name,
            "source": "bundled",
        },
    }
    page = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__ Web Flasher</title>
<style>
:root { color-scheme: dark; --bg: #101214; --panel: #181d21; --text: #eef3f6; --muted: #9aa7b1; --line: #303941; --accent: #58a6ff; --accent-strong: #1f6feb; --danger: #ff7b72; }
* { box-sizing: border-box; }
body { margin: 0; min-height: 100vh; font: 16px/1.5 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }
main { width: min(760px, 100%); margin: 0 auto; padding: 32px 18px 46px; }
.top { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 26px; color: var(--muted); font-size: 14px; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
h1 { margin: 0; font-size: clamp(30px, 7vw, 50px); line-height: 1.05; letter-spacing: 0; }
.lead { margin: 14px 0 24px; color: var(--muted); font-size: 18px; }
.panel { border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 18px; margin-top: 16px; }
.field { display: grid; gap: 7px; margin-bottom: 14px; }
label, .label { color: var(--muted); font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 0; }
select { width: 100%; min-height: 44px; border: 1px solid var(--line); border-radius: 7px; background: #101418; color: var(--text); padding: 0 12px; font: inherit; }
.check { display: flex; align-items: flex-start; gap: 10px; margin: 14px 0 18px; color: var(--text); text-transform: none; font-size: 15px; font-weight: 600; }
.check input { margin-top: 4px; }
.check small { display: block; margin-top: 2px; color: var(--muted); font-size: 13px; font-weight: 500; }
button { min-height: 44px; border: 1px solid #4287d7; border-radius: 7px; background: var(--accent-strong); color: white; padding: 0 18px; font: inherit; font-weight: 760; cursor: pointer; }
button:hover { background: #2b7ddd; }
button:disabled { cursor: not-allowed; opacity: .58; }
progress { display: block; width: 100%; height: 12px; margin: 16px 0 10px; accent-color: var(--accent); }
.status { min-height: 24px; color: var(--muted); }
.status.error { color: var(--danger); }
.status.ok { color: #7ee787; }
.meta { color: var(--muted); font-size: 13px; margin-top: 10px; overflow-wrap: anywhere; }
.warning { border-color: #5b4326; background: #211b13; }
ul { margin: 8px 0 0; padding-left: 20px; color: var(--muted); }
code { background: #111518; border: 1px solid var(--line); border-radius: 5px; padding: 1px 5px; }
</style>
</head>
<body>
<main>
<div class="top"><a href="__REPO_URL__">M45 Firmware</a><span>Board 602</span></div>
<h1>__NAME__</h1>
<p class="lead">Flash Bitaxe Gamma 602 firmware from Chrome or Edge using USB serial.</p>
<section class="panel">
<div class="field">
<label for="firmware-select">Firmware</label>
<select id="firmware-select"></select>
</div>
<label class="check" for="erase-settings"><input id="erase-settings" type="checkbox"><span>Erase settings<small>Select this when flashing from stock Bitaxe/ESP-Miner or other firmware.</small></span></label>
<button id="flash-button" type="button">Connect &amp; Flash</button>
<progress id="progress" max="100" value="0"></progress>
<div id="status" class="status">Ready.</div>
<div id="release-meta" class="meta"></div>
</section>
<section class="panel warning">
<strong>Hardware warning</strong>
<ul>
<li>This firmware is for Bitaxe Gamma 602 hardware.</li>
<li>Default flashing skips NVS at <code>0x9000-0xefff</code> so M45 settings stay intact.</li>
<li>Select erase settings when flashing from stock Bitaxe/ESP-Miner or other firmware.</li>
<li>Overclocking or bad cooling can permanently damage hardware.</li>
</ul>
</section>
</main>
<script type="module">
import { ESPLoader, Transport } from "__ESPTOOL_JS_URL__";

const CONFIG = __CONFIG__;
const NVS_END = CONFIG.nvsStart + CONFIG.nvsSize;
const select = document.getElementById("firmware-select");
const eraseSettings = document.getElementById("erase-settings");
const flashButton = document.getElementById("flash-button");
const progress = document.getElementById("progress");
const statusEl = document.getElementById("status");
const releaseMeta = document.getElementById("release-meta");
let firmwareOptions = [CONFIG.bundledRelease];

function setStatus(message, kind = "") {
  statusEl.textContent = message;
  statusEl.className = kind ? `status ${kind}` : "status";
}

function releaseLabel(release) {
  const name = release.name && release.name !== release.version ? ` - ${release.name}` : "";
  return `${release.version}${name}`;
}

function normalizeRelease(release) {
  const path = release && (release.flash_path || release.path);
  if (!release || !release.version || !path) return null;
  return {
    version: String(release.version),
    name: release.name ? String(release.name) : String(release.version),
    path: String(path),
    digest: release.digest ? String(release.digest) : "",
    size: Number(release.size || 0),
    source: release.source ? String(release.source) : "canonical",
  };
}

async function loadManifestReleases() {
  const response = await fetch("canonical-releases.json", { cache: "no-store" });
  if (!response.ok) return [];
  const manifest = await response.json();
  return (manifest.releases || []).map(normalizeRelease).filter(Boolean);
}

function renderReleases() {
  select.textContent = "";
  firmwareOptions.forEach((release, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = releaseLabel(release);
    select.appendChild(option);
  });
  updateReleaseMeta();
}

function updateReleaseMeta() {
  const release = firmwareOptions[Number(select.value) || 0];
  if (!release) {
    releaseMeta.textContent = "";
    return;
  }
  const details = [release.path];
  if (release.digest) details.push(release.digest);
  releaseMeta.textContent = details.join(" | ");
}

async function loadReleases() {
  const byVersion = new Map();
  for (const release of [CONFIG.bundledRelease, ...(await loadManifestReleases())]) {
    const normalized = normalizeRelease(release);
    if (normalized) byVersion.set(normalized.version, normalized);
  }
  firmwareOptions = Array.from(byVersion.values());
  renderReleases();
}

async function calculateSHA256(data) {
  const hashBuffer = await crypto.subtle.digest("SHA-256", data);
  return Array.from(new Uint8Array(hashBuffer))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

async function fetchFirmware(release) {
  const response = await fetch(release.path, { cache: "no-store" });
  if (!response.ok) throw new Error(`Failed to download firmware (${response.status}).`);
  const firmware = await response.arrayBuffer();
  if (release.digest && release.digest.startsWith("sha256:")) {
    const expected = release.digest.slice("sha256:".length);
    const actual = await calculateSHA256(firmware);
    if (actual !== expected) throw new Error("Firmware SHA256 verification failed.");
  }
  return firmware;
}

function flashParts(firmwareBinaryString, keepConfig) {
  if (keepConfig) {
    return [
      {
        data: firmwareBinaryString.slice(0, CONFIG.nvsStart),
        address: 0,
      },
      {
        data: firmwareBinaryString.slice(NVS_END),
        address: NVS_END,
      },
    ];
  }
  return [
    {
      data: firmwareBinaryString,
      address: 0,
    },
  ];
}

async function flashFirmware() {
  if (!("serial" in navigator)) {
    setStatus("Use Chrome or Edge on a desktop browser with Web Serial support.", "error");
    return;
  }

  const selected = firmwareOptions[Number(select.value) || 0];
  if (!selected) {
    setStatus("No firmware release is available.", "error");
    return;
  }

  let transport = null;
  let port = null;
  flashButton.disabled = true;
  select.disabled = true;
  eraseSettings.disabled = true;
  progress.value = 0;
  setStatus("Select the device serial port.");

  try {
    port = await navigator.serial.requestPort();
    transport = new Transport(port);
    const loader = new ESPLoader({
      transport,
      baudrate: 115200,
      romBaudrate: 115200,
      terminal: {
        clean() {},
        writeLine(data) {
          console.debug(data);
        },
        write(data) {
          console.debug(data);
        },
      },
    });

    setStatus("Connecting...");
    await loader.main();

    setStatus(`Downloading ${selected.version}...`);
    const firmwareArrayBuffer = await fetchFirmware(selected);
    const firmwareUint8Array = new Uint8Array(firmwareArrayBuffer);
    const firmwareBinaryString = Array.from(
      firmwareUint8Array,
      (byte) => String.fromCharCode(byte)
    ).join("");

    const keepConfig = !eraseSettings.checked;
    const parts = flashParts(firmwareBinaryString, keepConfig);
    setStatus("Flashing 0%...");

    await loader.writeFlash({
      fileArray: parts,
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => {
        const percent = Math.round((written / total) * 100);
        progress.value = percent;
        setStatus(percent === 100 ? "Flash write complete." : `Flashing ${percent}%...`);
      },
      calculateMD5Hash: () => "",
    });

    setStatus("Resetting device...");
    await loader.hardReset();
    await transport.disconnect();
    transport = null;
    if (port && port.readable) await port.close();
    progress.value = 100;
    setStatus("Flash complete. The device is rebooting.", "ok");
  } catch (error) {
    console.error("Flashing failed:", error);
    setStatus(error instanceof Error ? error.message : String(error), "error");
    if (transport) {
      try {
        await transport.disconnect();
      } catch (disconnectError) {
        console.warn("disconnect failed", disconnectError);
      }
    }
  } finally {
    flashButton.disabled = false;
    select.disabled = false;
    eraseSettings.disabled = false;
  }
}

select.addEventListener("change", updateReleaseMeta);
flashButton.addEventListener("click", flashFirmware);

if (!("serial" in navigator)) {
  flashButton.disabled = true;
  setStatus("Use Chrome or Edge on a desktop browser with Web Serial support.", "error");
}

loadReleases();
</script>
</body>
</html>
"""
    replacements = {
        "__TITLE__": html.escape(name, quote=True),
        "__NAME__": html.escape(name),
        "__REPO_URL__": html.escape(REPO_URL, quote=True),
        "__ESPTOOL_JS_URL__": html.escape(ESPTOOL_JS_URL, quote=True),
        "__CONFIG__": json.dumps(config),
    }
    for placeholder, value in replacements.items():
        page = page.replace(placeholder, value)
    (output_dir / "index.html").write_text(page)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build/docker")
    parser.add_argument("--output", default="dist/web-flasher")
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--version", default=None)
    parser.add_argument("--board-version", default=DEFAULT_BOARD_VERSION)
    parser.add_argument("--firmware-name", default=None)
    args = parser.parse_args()

    build_dir = pathlib.Path(args.build_dir).resolve()
    output_dir = pathlib.Path(args.output).resolve()
    version = args.version or default_version()
    firmware_name = args.firmware_name or default_firmware_name(args.board_version, version)

    try:
        flasher_args = load_flash_args(build_dir)
        if output_dir.exists():
            shutil.rmtree(output_dir)
        output_dir.mkdir(parents=True)

        firmware = merge_flash_parts(build_dir, output_dir, firmware_name, flasher_args)
        (output_dir / "canonical-releases.json").write_text(
            json.dumps({"releases": []}, indent=2) + "\n"
        )
        write_index(output_dir, args.name, version, firmware_name, args.board_version)
        (output_dir / "version.txt").write_text(version + "\n")
    except (
        KeyError,
        FileNotFoundError,
        ValueError,
        subprocess.CalledProcessError,
    ) as err:
        return die(str(err))

    print(f"web flasher package: {output_dir}")
    print(f"firmware: {firmware}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
