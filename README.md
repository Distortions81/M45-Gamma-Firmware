# M45 Gamma Firmware

![M45 Bitaxe web dashboard](docs/screenshots/dashboard.png)

M45 is experimental firmware for Bitaxe Gamma 602 miners. It gives the Gamma a
local web dashboard, QR-assisted setup, weighted multi-pool mining, and opt-in
overclock controls while booting at stock ASIC settings by default.

[Open the Web Flasher](https://m45core.github.io/M45-Gamma-Firmware/)
to flash a prepared build from Chrome or Edge using USB serial.

Use it at your own risk. Overclocking or bad cooling can permanently damage
the ASIC, regulator, fan, wiring, or power supply, and can create a fire risk.

## Highlights

- Local dashboard for hashrate, temperatures, power, fan, pool status, best
  diff, and block-found alerts.
- Swarm dashboard that shows the local miner immediately, discovers M45 and
  AxeOS miners on the local `/24`, and caches last-known peers in the browser.
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
- BOOT-button Wi-Fi recovery and five-second full settings reset with OLED
  instructions.
- Fixed-size, removable hardware fault history.
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

### Physical Recovery

Hold BOOT while starting or resetting the device, and keep holding it for the
full five-second OLED countdown, to erase all settings. Releasing BOOT early
cancels the erase and boots normally.

### Updates And OTA

Use the web flasher above for first install or USB recovery.

OTA accepts either:

- `esp-miner.bin`, the raw application image for installing M45 over stock
  AxeOS/ESP-Miner from its OTA page. On its first M45 boot it imports the
  existing Wi-Fi, hostname, and selected Stratum V1 pool settings before
  writing M45 configuration. Stratum V2 and hardware tuning settings are not
  imported.
- `esp-miner-factory-602-*.bin`, the merged single-file image used by the USB
  web flasher and full-flash tools.
- `M45-Firmware.bin`, a compatibility copy of the raw application image in
  local OTA packages.

Canonical releases publish `esp-miner.bin` and the merged factory image directly
to GitHub Pages. Run the **Web Flasher Pages** workflow on the release branch
with a required semantic version such as `v0.1.0`; that version is embedded in
the application and written to `canonical-releases.json`. GitHub Pages is the
only update channel for v0.1.0 and later.

The AxeOS settings importer runs only when no M45 configuration exists. It reads
stock settings from AxeOS's `main` NVS namespace and leaves that namespace
unchanged. If the imported AxeOS board identity is missing or is not `602`, M45
still starts Wi-Fi and its dashboard but keeps the ASIC disabled. The OLED
identifies the unsupported board; pressing BOOT at that screen, or choosing
**Return to AxeOS** in the dashboard, boots a validated retained `esp-miner`
application partition. A fresh full flash with erased settings is treated as an
explicit Gamma 602 installation.

OTA images carry a Gamma 602 board identity and a configuration epoch. Firmware
release numbers may move forward or backward. When installing firmware with an
older configuration epoch, settings are erased before it boots. At startup,
firmware also checks the stored settings schema before reading any setting; a
schema newer than it supports is erased and the device returns to setup mode.

Application OTA never rewrites the device partition table. Existing M45 devices
with 3 MB slots and devices transitioned from AxeOS with 4 MB slots therefore
keep their installed layout. New M45 factory images use the AxeOS/ESP-Miner
16 MB partition layout verbatim, including its 4 MB factory and OTA app slots
and retained `www` partition. Normal M45 application images are checked against
the actual target slot size at upload time.

The Update page also has a separate **Install AxeOS** area. It accepts either a
new merged single-file factory image or the legacy `esp-miner.bin` plus matching
`www.bin` pair. Merged images contribute only their application image; M45 does
not rewrite the bootloader, partition table, or NVS during an AxeOS install.

After an OTA reboot, Gamma 602 hardware initialization runs before mining starts.
Configuration-epoch checks erase incompatible newer settings before older
firmware reads them.

### Stored Credentials

Empty Stratum passwords and the conventional default password `x` are not
written to NVS. Wi-Fi passwords and non-default Stratum passwords remain stored
in ordinary NVS; this firmware does not provision eFuses or enable encrypted
NVS.

The Logs page keeps the eight most recent persistent hardware/safety faults.
Each entry has the best available date/time (or boot uptime before time sync),
an individual remove button, and a Clear All action.

### Network Trust And Exposure

M45 is designed for use on a trusted local network. Its HTTP dashboard and API
do not provide user accounts or access control, so any client that can reach the
device can view its status and change its configuration, hardware settings, or
firmware.

Do not expose the device HTTP port through router port forwarding, UPnP, a
public reverse proxy, or a cloud tunnel. Do not place it on an untrusted or guest
LAN/VLAN unless that network prevents other clients from reaching the device.
Browser cross-origin management requests are blocked, but that is not a
substitute for network isolation.

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
