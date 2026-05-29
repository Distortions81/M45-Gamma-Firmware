#!/usr/bin/env python3
import argparse
import html
import json
import pathlib
import shutil
import subprocess
import sys


DEFAULT_NAME = "M45 Gamma Firmware"
DEFAULT_FIRMWARE = "m45-gamma-firmware.bin"
REPO_URL = "https://github.com/Distortions81/M45-Gamma-Firmware"


def die(message):
    print(f"error: {message}", file=sys.stderr)
    return 1


def run(args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def git_text(args, default):
    try:
        return subprocess.check_output(["git", *args], text=True).strip() or default
    except (OSError, subprocess.CalledProcessError):
        return default


def default_version():
    version = git_text(["describe", "--tags", "--always"], "dev")
    dirty = git_text(["status", "--porcelain"], "")
    return f"{version}-dirty" if dirty else version


def chip_family(chip):
    normalized = chip.lower().replace("-", "")
    families = {
        "esp32": "ESP32",
        "esp32s2": "ESP32-S2",
        "esp32s3": "ESP32-S3",
        "esp32c2": "ESP32-C2",
        "esp32c3": "ESP32-C3",
        "esp32c5": "ESP32-C5",
        "esp32c6": "ESP32-C6",
        "esp32c61": "ESP32-C61",
        "esp32h2": "ESP32-H2",
        "esp32p4": "ESP32-P4",
    }
    return families.get(normalized, chip.upper())


def load_flash_args(build_dir):
    path = build_dir / "flasher_args.json"
    if not path.exists():
        raise FileNotFoundError(f"{path} does not exist; build firmware first")
    return json.loads(path.read_text())


def sorted_flash_files(flash_files):
    return sorted(flash_files.items(), key=lambda item: int(item[0], 0))


def merge_firmware(build_dir, output_dir, firmware_name, flasher_args):
    settings = flasher_args["flash_settings"]
    flash_files = sorted_flash_files(flasher_args["flash_files"])
    chip = flasher_args.get("extra_esptool_args", {}).get("chip", "esp32s3")
    merged = output_dir / firmware_name

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        chip,
        "merge_bin",
        "-o",
        str(merged),
        "--flash_mode",
        settings["flash_mode"],
        "--flash_freq",
        settings["flash_freq"],
        "--flash_size",
        settings["flash_size"],
    ]

    for offset, filename in flash_files:
        source = build_dir / filename
        if not source.exists():
            raise FileNotFoundError(f"missing flash part: {source}")
        command.extend([offset, str(source)])

    run(command)
    return chip, merged


def write_manifest(output_dir, firmware_name, name, version, chip):
    manifest = {
        "name": name,
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": chip_family(chip),
                "parts": [
                    {
                        "path": firmware_name,
                        "offset": 0,
                    }
                ],
            }
        ],
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def write_index(output_dir, name, version, firmware_name):
    escaped_name = html.escape(name)
    escaped_version = html.escape(version)
    escaped_firmware = html.escape(firmware_name)
    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{escaped_name} Web Flasher</title>
<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
<style>
:root {{ color-scheme: dark; --bg: #0f1113; --panel: #171b1f; --text: #f1f5f8; --muted: #9aa6af; --line: #2b333a; --blue: #6bb7ff; --bad: #ff6b6b; }}
* {{ box-sizing: border-box; }}
body {{ margin: 0; min-height: 100vh; font: 16px/1.5 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }}
main {{ width: min(760px, 100%); margin: 0 auto; padding: 34px 18px 46px; }}
.top {{ display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 28px; }}
a {{ color: var(--blue); text-decoration: none; }}
a:hover {{ text-decoration: underline; }}
h1 {{ margin: 0; font-size: clamp(30px, 6vw, 48px); line-height: 1.05; }}
.lead {{ margin: 14px 0 24px; color: var(--muted); font-size: 18px; }}
.panel {{ border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 18px; margin-top: 16px; }}
.meta {{ color: var(--muted); font-size: 13px; margin-top: 8px; }}
.warning {{ border-color: #5c3d23; background: #211b13; }}
esp-web-install-button button {{ min-height: 44px; border: 1px solid #4d8dcc; border-radius: 7px; background: #1d5f9f; color: white; padding: 0 18px; font: inherit; font-weight: 760; cursor: pointer; }}
esp-web-install-button button:hover {{ background: #2470b8; }}
ul {{ margin: 8px 0 0; padding-left: 20px; color: var(--muted); }}
code {{ background: #111518; border: 1px solid var(--line); border-radius: 5px; padding: 1px 5px; }}
</style>
</head>
<body>
<main>
<div class="top"><a href="{REPO_URL}">M45 Firmware</a><span class="meta">Version {escaped_version}</span></div>
<h1>{escaped_name} Web Flasher</h1>
<p class="lead">Flash Bitaxe Gamma 602 firmware from a browser using USB serial.</p>
<section class="panel">
<esp-web-install-button manifest="manifest.json">
<button slot="activate" type="button">Flash Firmware</button>
<span slot="unsupported">Use Chrome or Edge on a desktop browser with Web Serial support.</span>
<span slot="not-allowed">Open this page over HTTPS to use Web Serial.</span>
</esp-web-install-button>
<div class="meta">Manifest: <code>manifest.json</code>. Firmware: <code>{escaped_firmware}</code>.</div>
</section>
<section class="panel warning">
<strong>Hardware warning</strong>
<ul>
<li>This firmware is for Bitaxe Gamma 602 hardware.</li>
<li>Overclocking or bad cooling can permanently damage hardware.</li>
<li>Choose erase on first install if you are replacing unrelated firmware.</li>
</ul>
</section>
</main>
</body>
</html>
"""
    (output_dir / "index.html").write_text(page)


def copy_metadata(build_dir, output_dir):
    for filename in ("flasher_args.json", "flash_args"):
        source = build_dir / filename
        if source.exists():
            shutil.copy2(source, output_dir / filename)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build/docker")
    parser.add_argument("--output", default="dist/web-flasher")
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--version", default=None)
    parser.add_argument("--firmware-name", default=DEFAULT_FIRMWARE)
    args = parser.parse_args()

    build_dir = pathlib.Path(args.build_dir).resolve()
    output_dir = pathlib.Path(args.output).resolve()
    version = args.version or default_version()

    try:
        flasher_args = load_flash_args(build_dir)
        if output_dir.exists():
            shutil.rmtree(output_dir)
        output_dir.mkdir(parents=True)

        chip, merged = merge_firmware(build_dir, output_dir, args.firmware_name, flasher_args)
        write_manifest(output_dir, args.firmware_name, args.name, version, chip)
        write_index(output_dir, args.name, version, args.firmware_name)
        copy_metadata(build_dir, output_dir)
        (output_dir / "version.txt").write_text(version + "\n")
    except (KeyError, FileNotFoundError, subprocess.CalledProcessError) as err:
        return die(str(err))

    print(f"web flasher package: {output_dir}")
    print(f"merged firmware: {merged}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
