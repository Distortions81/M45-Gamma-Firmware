# M45 Gamma Firmware [ALPHA]

![M45 Bitaxe web dashboard](docs/screenshots/dashboard.png)

M45 is experimental firmware for Bitaxe Gamma 602 miners. It boots at stock
ASIC settings, exposes a local web dashboard, and lets you opt in to fan,
pool, and overclock controls from the browser.

[Open the Web Flasher](https://m45core.github.io/M45-Gamma-Firmware/)
to flash a prepared build from Chrome or Edge using USB serial.

Use it at your own risk. Overclocking or bad cooling can permanently damage
the ASIC, regulator, fan, wiring, or power supply, and can create a fire risk.

## Updates And OTA

Use the [Web Flasher](https://m45core.github.io/M45-Gamma-Firmware/) for
the first install or when you need to recover a device over USB serial.

OTA accepts either:

- `M45-Firmware.bin`, the app image produced by `scripts/docker-build.sh`.
- `esp-miner-factory-602-*.bin`, the merged factory image used by the web
  flasher.

GitHub Actions builds the canonical factory image when the mandatory bridge
release is published, attaches only that `.bin`, and publishes the canonical
web flasher through GitHub Pages. Legacy releases are not mirrored or listed.

### Final v0.0.9 Channel Migration

This branch produces the final release offered by the v0.0.9 update channel.
After it is installed into either legacy OTA slot, the bridge release:

1. Detects the exact v0.0.9 partition layout and 16 MB flash.
2. Copies its running application to the canonical factory address.
3. Verifies the copied ESP image and SHA-256 digest.
4. Initializes the canonical OTA metadata area.
5. Writes and reads back the canonical partition table as the final operation.
6. Reboots on the canonical layout.

The legacy update endpoint exists only to advertise this mandatory bridge.
`v0.0.10` is its sole and final production release; earlier binaries are not
mirrored or listed. After migration, the Update page reads
`https://m45core.github.io/M45-Gamma-Firmware/canonical-releases.json`.
That manifest advertises raw application images so future updates remain small:

```json
{
  "releases": [
    {
      "version": "v0.1.0",
      "name": "v0.1.0",
      "ota_path": "firmware/v0.1.0/esp-miner.bin",
      "flash_path": "firmware/v0.1.0/esp-miner-factory-602-v0.1.0.bin"
    }
  ]
}
```

Future production releases belong only in that canonical manifest. No legacy
partition image is built, published, or supported.

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

Docker builds use repository defaults and do not embed local Wi-Fi or pool
settings. Configure Wi-Fi and mining from the first boot setup page.

To produce local OTA upload files:

```sh
scripts/docker-ota.sh
```

The OTA files are written to `dist/ota/` and include the raw bridge app and
the canonical merged image, plus SHA-256 checksums. The v0.0.9 partition CSV is
retained only as a migration-test fixture; it is never packaged for flashing.

To produce the static web flasher package locally:

```sh
scripts/docker-web-flasher.sh
```

The package is written to `dist/web-flasher/` and includes `index.html`,
a canonical release manifest, and one canonical merged image named like
`esp-miner-factory-602-v0.0.1.bin`. The web flasher follows the Bitaxe flasher
model: it writes the factory image as one file, skips the NVS settings range by
default, and only writes that range when you select erase settings.

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
- Single-pool mining or weighted multi-pool mining with per-pool weights.
- Wallet payout detection from coinbase data when the pool exposes enough
  information.
- Native M45 JSON endpoints plus ESP-Miner-compatible JSON routes for tools
  that expect the Bitaxe API shape. See
  [`docs/json-endpoints.md`](docs/json-endpoints.md).

## M45 Features Over ESP-Miner

M45-specific additions compared with the stock ESP-Miner-style workflow:

- OLED first-boot Wi-Fi setup QR code and post-setup dashboard QR code.
- Browser controls for Wi-Fi, mining pools, fan mode, ASIC
  clock/voltage, temperature compensation, display sleep, screensaver and ASIC power.
- Configurable safety limits for VIN, ASIC voltage, ASIC temperature, TPS546
  temperature, and TPS546 current, with an explicit unrestricted mode for
  advanced testing.
- Runtime TPS546 PMBus limit updates when safety limits change, including
  values outside the normal ranges when unrestricted mode is enabled.
- Auto-clock preset selection holds or steps down instead of increasing when
  VIN is at or below `5.01 V`, TPS546 current or VR temperature is close to
  configured safety limits, or estimated ASIC watts exceed the enabled watt
  cap, which defaults to `40 W`. Automatic increases are capped at `1200 MHz`.
- Coinbase payout detection, block-found alerting, and best-diff reset.
- Weighted multi-pool mining uses per-pool job slices so pools can keep their
  own Stratum version mask without clearing unrelated ASIC work.
- Weighted pools use per-pool usernames or an optional default username.
- Native M45 JSON endpoints plus ESP-Miner-compatible JSON routes documented in
  [`docs/json-endpoints.md`](docs/json-endpoints.md).

## Safety Behavior

The firmware holds ASIC reset low and turns TPS546 output off if a critical
condition is detected. Default shutdown limits are:

- ASIC temperature reaches `69 C`.
- Enabled ASIC VOUT falls below `0.700 V`.
- ASIC VOUT reaches `1.400 V`.
- TPS546 temperature reaches `110 C`.
- Input VIN falls below `4.8 V`.
- Input VIN reaches `5.5 V`.
- TPS546 output current reaches `33 A`.
- TPS546 or temperature monitor reads fail while hardware is active.

These limits are last-resort protections, not a substitute for proper cooling,
power delivery, and monitoring.
ASIC temperature read failures are tolerated only while the lost-domain
auto-reboot watchdog is already waiting to power-cycle the ASIC.

The dashboard can explicitly unlock unrestricted safety-limit ranges for
advanced testing. That option only removes firmware setting caps; it does not
make unsafe values safe.

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
