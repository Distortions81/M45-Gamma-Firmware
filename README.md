# M45 Gamma Firmware

![M45 Bitaxe web dashboard](docs/screenshots/dashboard.png)

M45 is experimental firmware for Bitaxe Gamma 602 miners. It gives the Gamma a
local web dashboard, QR-assisted setup, weighted multi-pool mining, and opt-in
overclock controls while booting at stock ASIC settings by default.

[Open the Web Flasher](https://distortions81.github.io/M45-Gamma-Firmware/)
to flash a prepared build from Chrome or Edge using USB serial.

Use it at your own risk. Overclocking or bad cooling can permanently damage
the ASIC, regulator, fan, wiring, or power supply, and can create a fire risk.

## Highlights

- Local dashboard for hashrate, temperatures, power, fan, pool status, best
  diff, and block-found alerts.
- Browser controls for Wi-Fi, pools, fan mode, ASIC clock/voltage, display
  sleep, screensaver, and ASIC power.
- Single-pool or weighted multi-pool mining with per-pool weights and
  usernames.
- Experimental auto-clock overclocking with temperature, VIN, current,
  VR-temperature, and watt-cap guardrails.
- Stock clock and voltage enforcement unless overclocking is explicitly
  enabled.
- Runtime safety-limit controls, including TPS546 PMBus limit updates.
- OLED QR codes for first-boot Wi-Fi setup and the post-setup dashboard URL.
- Wallet payout detection from coinbase data when the pool exposes enough
  information, block-found alerting, and best-diff reset.
- Native M45 JSON endpoints plus ESP-Miner-compatible JSON routes for existing
  tooling.

See [`docs/json-endpoints.md`](docs/json-endpoints.md) for native M45 and
ESP-Miner-compatible API routes.

## Technical Reference

### Hardware

This firmware targets Bitaxe Gamma 602 hardware:

- ESP32-S3 with 16 MB flash and octal PSRAM.
- BM1370 ASIC, stock default `525 MHz` at `1150 mV`.
- TPS546 PMBus regulator at I2C address `0x24`.
- EMC2101 fan controller on the Bitaxe I2C bus.
- SSD1306 OLED at `0x3c`, `128x32`, on the Bitaxe I2C bus.

Other boards may need source changes before they are safe to run.

### First Boot

If the device has no saved Wi-Fi credentials, it starts a temporary setup
network named `m-XXXX` and shows a Wi-Fi QR code on the OLED. Join that
network, open the displayed setup address, then enter Wi-Fi and pool settings.

After the device joins Wi-Fi, the OLED shows the local dashboard address and an
HTTP QR code.

### Updates And OTA

Use the web flasher above for first install or USB recovery.

OTA accepts either:

- `M45-Firmware.bin`, the app image produced by `scripts/docker-build.sh`.
- `esp-miner-factory-602-*.bin`, the merged factory image used by the web
  flasher.

Release builds publish a merged factory image and update the GitHub Pages web
flasher so older release images can be selected.

### Build And Flash

Docker is the easiest build path:

```sh
scripts/docker-build.sh
```

Firmware artifacts are written to `build/docker/`. Docker builds use repository
defaults and do not embed local Wi-Fi or pool settings.

Flash from Docker on Linux:

```sh
scripts/docker-build.sh --flash /dev/ttyUSB0 --monitor
```

Use `/dev/ttyACM0` instead if that is how your board appears.

Build local OTA upload files:

```sh
scripts/docker-ota.sh
```

Build the static web flasher package:

```sh
scripts/docker-web-flasher.sh
```

If ESP-IDF `v5.5.3` is installed locally:

```sh
scripts/build.sh --build
scripts/build.sh --flash /dev/ttyUSB0
```

`scripts/build.sh --help` lists all build-time configuration options.

### Safety Behavior

The firmware holds ASIC reset low and turns TPS546 output off if a critical
condition is detected. Default shutdown limits include:

- ASIC temperature reaches `69 C`.
- Enabled ASIC VOUT falls below `0.700 V` or reaches `1.400 V`.
- TPS546 temperature reaches `110 C`.
- Input VIN falls below `4.8 V` or reaches `5.5 V`.
- TPS546 output current reaches `33 A`.
- TPS546 or temperature monitor reads fail while hardware is active.

These limits are last-resort protections, not a substitute for proper cooling,
power delivery, and monitoring. Unrestricted safety-limit ranges are available
for advanced testing, but they do not make unsafe values safe.

## Licensing

This repository is licensed under GPL-3.0. See `LICENSE`.

Third-party license details and upstream hardware references are tracked in
`THIRD_PARTY_NOTICES.md`. BM1370 hardware and protocol references come from
[`bitaxeorg/esp-miner`](https://github.com/bitaxeorg/esp-miner).
