# Third-Party Notices

## bitaxeorg/esp-miner

This firmware uses BM1370 hardware/protocol details from `bitaxeorg/esp-miner`.
No esp-miner source files are currently vendored or compiled.

- Upstream: [https://github.com/bitaxeorg/esp-miner](https://github.com/bitaxeorg/esp-miner)
- Upstream license: GPL-3.0
- License text: `LICENSES/esp-miner-GPL-3.0.txt`

Hardware/protocol details used from this upstream project:

- BM1370 chip ID probing, initialization register writes, UART baud commands,
  frequency register programming, nonce-space/hash-counting setup, job packet
  layout, ASIC result decoding, register reads, and version-rolling result
  handling.

Local firmware behavior, Stratum protocol handling, Bitcoin job construction,
hashing utilities, app state, TPS546 PMBus control, OLED, Wi-Fi, HTTP, and
ASIC safety policy are implemented in this repository's `main/` sources. Local
replacements now provide the Bitaxe I2C helper API, EMC2101 register constants,
BM1370 protocol implementation, BM1370 packet CRC5/CRC16 helpers, BM1370 UART
serial helpers, PLL divider calculation, frequency transition logic, Bitmain
response parsing/chip counting, difficulty mask encoding, and nonce timeout
calculation.

## M45-Core-Firmware

No `M45-Core-Firmware` source files are currently compiled or vendored in this
repository. The local OLED/status code was written in this repo while following
the general shape of that firmware.

## QRCode

The OLED setup view uses a bundled QR code generator in
`main/third_party/qrcode`.

- Upstream: [https://github.com/ricmoo/QRCode](https://github.com/ricmoo/QRCode)
- Additional upstream reference:
  [https://www.nayuki.io/page/qr-code-generator-library](https://www.nayuki.io/page/qr-code-generator-library)
- License: MIT
- License text: `LICENSES/MIT.txt`
- Copyright notices: Richard Moore and Project Nayuki, as preserved in
  `main/third_party/qrcode/qrcode.c` and `main/third_party/qrcode/qrcode.h`.
