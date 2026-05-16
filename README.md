# M45 Bitaxe Speed Alpha

**Alpha firmware for Bitaxe Gamma 602 speed testing and overclocking.**

This firmware is built for users who want faster boot, higher hashrate
experiments, direct speed controls, and tighter local telemetry than a general
purpose web firmware. It defaults to stock ASIC settings and keeps
overclocking behind an explicit enable switch because the higher presets can
damage hardware without the right power supply and cooling.

Use this at your own risk. Overclocking can permanently damage the ASIC,
voltage regulator, fan, wiring, or power supply, and can create a fire risk.

## Purpose

- Speed-focused Bitaxe Gamma 602 firmware for BM1370 tuning.
- Local web UI for live stats, settings, fan control, and overclock presets.
- Stock-safe default behavior with opt-in overclocking.
- Faster startup path with direct TPS546 regulator control and early safety
  validation.
- Minimal ESP-IDF app shape with only the required Bitaxe hardware pieces.

## Hardware References

This firmware uses BM1370 ASIC hardware/protocol details from
[`bitaxeorg/esp-miner`](https://github.com/bitaxeorg/esp-miner).

- BM1370 chip detection, initialization register writes, UART baud commands,
  frequency register programming, nonce-space setup, job packet format, result
  decoding, register reads, and version-rolling result handling.

## Hardware Guidance

The defaults target Bitaxe Gamma 602:

- ESP32-S3 with 16 MB flash and octal PSRAM.
- SSD1306 OLED at `0x3c`, `128x32`, on the Bitaxe I2C bus.
- BM1370 at `525 MHz`, `1150 mV`.
- TPS546 PMBus regulator on I2C address `0x24`.
- EMC2101 fan controller on the Bitaxe I2C bus.

## Features

- Faster boot and ASIC bring-up through command-controlled TPS546 startup.
- Better performance tuning with frequency and voltage presets from stock
  through high overclock ranges.
- Overclocking is disabled by default; when disabled, firmware enforces stock
  frequency and voltage even if higher values are saved.
- Enabling overclocking requires a warning modal and starts from the stock
  preset.
- Colored overclock stats for ASIC state, clock, voltage, hashrate, ASIC temp,
  VR temp, fan, and power.
- Better speed controls, including preset selection, manual frequency/voltage
  entry, and voltage offset adjustment.
- Reboot is not required to change settings; web changes are applied at
  runtime.
- Better fan controls with auto, fixed, no-fan, custom target temperature, and
  warnings when cooling is near max.
- Overclock safety guidance for fan speed, voltage-regulator heatsinks, liquid
  cooling at high clocks, and upgraded PSU recommendations.
- Compact web dashboard with expected ranges, safety limits, colored telemetry
  bars, block-found alerts, best-diff tracking, and reset controls.
- Coinbase payout decode checks with wallet percentage and not-in-wallet
  warnings.
- Backup Stratum pool fallback with automatic return to the primary pool.
- Random per-boot page token and no-store headers to prevent stale cached web
  UI/API responses after refreshes.
- OLED mining display throttled during active mining and only redrawn when the
  text changes.
- Stratum queue clears automatically when pool difficulty increases so old work
  does not linger after a target change.
- Safety shutdown for ASIC temperature, ASIC voltage, TPS546 temperature, input
  voltage, regulator faults, and monitor read failures.

## Startup And Safety

- TPS546 startup is command-controlled, with a 2 ms PMBus TON_RISE and a
  first validation read after 10 ms. Firmware releases ASIC reset as soon as
  TPS546 telemetry reaches the target VOUT, up to a 150 ms startup timeout.
- EMC2101 fan controller is set to 100% PWM at early boot before NVS config,
  display, Wi-Fi, regulator, or ASIC bring-up.
- Safety shutdown holds ASIC reset low and turns TPS546 output off if ASIC
  temperature reaches `69 C`, enabled ASIC VOUT falls below `0.700 V`, ASIC
  VOUT reaches `1.400 V`, TPS546 temperature reaches `98 C`, input VIN reaches
  `5.5 V`, or the TPS546/temperature monitors fail while hardware is active.

## Licensing

This repository is licensed under GPL-3.0. See `LICENSE`.

Third-party license details and upstream hardware references are tracked in
`THIRD_PARTY_NOTICES.md`. The esp-miner GPL text is also kept at
`LICENSES/esp-miner-GPL-3.0.txt`, and the bundled QR code library uses the MIT
license text at `LICENSES/MIT.txt`.
