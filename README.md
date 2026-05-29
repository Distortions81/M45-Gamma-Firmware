# M45 Gamma Firmware [ALPHA]

![M45 Bitaxe Speed Alpha web dashboard](docs/screenshots/dashboard.png)

M45 is experimental firmware for Bitaxe Gamma 602 miners. It boots at stock
ASIC settings, exposes a local web dashboard, and lets you opt in to fan,
pool, and overclock controls from the browser.

[Open the Web Flasher](https://distortions81.github.io/M45-Gamma-Firmware/)
to flash a prepared build from Chrome or Edge using USB serial.

Use it at your own risk. Overclocking or bad cooling can permanently damage
the ASIC, regulator, fan, wiring, or power supply, and can create a fire risk.

## Supported Hardware

This firmware targets Bitaxe Gamma 602 hardware:

- ESP32-S3 with 16 MB flash and octal PSRAM.
- BM1370 ASIC, stock default `525 MHz` at `1150 mV`.
- TPS546 PMBus regulator at I2C address `0x24`.
- EMC2101 fan controller on the Bitaxe I2C bus.
- SSD1306 OLED at `0x3c`, `128x32`, on the Bitaxe I2C bus.

Other boards may need source changes before they are safe to run.

## Build With Docker

Docker is the easiest build path. You do not need to install ESP-IDF on your
host machine.

```sh
scripts/docker-build.sh
```

The first run builds a local Docker image from `espressif/idf:v5.5.3`. Firmware
artifacts are written to `build/docker/`, including:

- `build/docker/M45-Firmware.bin`
- `build/docker/bootloader/bootloader.bin`
- `build/docker/partition_table/partition-table.bin`
- `build/docker/flasher_args.json`

You can bake default Wi-Fi and pool settings into the firmware:

```sh
scripts/docker-build.sh \
  --wifi-ssid "My WiFi" \
  --wifi-pass "secret" \
  --pool-host public-pool.io \
  --pool-port 3333 \
  --pool-user "bc1q...worker" \
  --pool-pass x \
  --build
```

You can also skip those options and configure Wi-Fi and mining from the first
boot setup page.

Build-time Wi-Fi and pool values are stored in the ignored `sdkconfig` file and
embedded in the firmware image. Do not share a firmware image that contains
private credentials.

To produce the static web flasher package locally:

```sh
scripts/docker-web-flasher.sh
```

The package is written to `dist/web-flasher/` and includes `index.html`,
a release list, and one merged factory image named like
`esp-miner-factory-602-v0.0.1.bin`. The web flasher follows the Bitaxe flasher
model: it writes the factory image as one file, skips the NVS settings range by
default, and only writes that range when you select erase settings.

After flashing a current factory image, the dashboard Update page can install
later builds over the air. OTA accepts either `build/docker/M45-Firmware.bin`
or the same `esp-miner-factory-602-*.bin` factory image used by the web flasher.
Older single-app installs need one web-flasher update before OTA is available.

GitHub Actions builds a factory image when a GitHub Release is published,
attaches only that `.bin` to the release, and publishes the web flasher through
GitHub Pages. The Pages site mirrors matching release images so older releases
can be selected from the flasher.

## Flash

On Linux, the Docker wrapper can pass a serial device into the container:

```sh
scripts/docker-build.sh --flash /dev/ttyUSB0
```

Use `/dev/ttyACM0` instead if that is how your board appears. Add `--monitor`
to open the serial monitor after flashing:

```sh
scripts/docker-build.sh --flash /dev/ttyUSB0 --monitor
```

If USB passthrough from Docker is not available on your machine, build with
Docker and flash from a host ESP-IDF shell:

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

## First Boot

If the device has no saved Wi-Fi credentials, it starts a temporary setup
network named `m-XXXX` and shows a Wi-Fi QR code on the OLED. Join that
network, open the displayed setup address, then enter Wi-Fi and pool settings.

After the device joins Wi-Fi, the OLED shows the local dashboard address and an
HTTP QR code. Open that address from a phone or computer on the same network.

## What You Get

- Live dashboard for hashrate, ASIC state, temperatures, fan, power, best diff,
  block-found alerts, and pool status.
- Runtime settings for Wi-Fi, mining pool, fan mode, and ASIC speed.
- Overclocking disabled by default. When disabled, stock clock and voltage are
  enforced even if higher saved values exist.
- Primary and backup Stratum pools with automatic return to the primary pool.
- Wallet payout detection from coinbase data when the pool exposes enough
  information.

## Safety Behavior

The firmware holds ASIC reset low and turns TPS546 output off if a critical
condition is detected. Default shutdown limits are:

- ASIC temperature reaches `69 C`.
- Enabled ASIC VOUT falls below `0.700 V`.
- ASIC VOUT reaches `1.400 V`.
- TPS546 temperature reaches `98 C`.
- Input VIN reaches `5.5 V`.
- TPS546 output current reaches `30 A`.
- TPS546 or temperature monitor reads fail while hardware is active.

These limits are last-resort protections, not a substitute for proper cooling,
power delivery, and monitoring.

## Build Without Docker

If ESP-IDF `v5.5.3` is installed locally:

```sh
scripts/build.sh --build
scripts/build.sh --flash /dev/ttyUSB0
```

`scripts/build.sh --help` lists all build-time configuration options.

## Licensing

This repository is licensed under GPL-3.0. See `LICENSE`.

Third-party license details and upstream hardware references are tracked in
`THIRD_PARTY_NOTICES.md`. BM1370 hardware and protocol references come from
[`bitaxeorg/esp-miner`](https://github.com/bitaxeorg/esp-miner).
