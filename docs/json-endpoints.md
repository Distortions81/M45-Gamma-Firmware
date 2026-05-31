# JSON Endpoints

This document describes the HTTP JSON API served by the device web UI.

All paths are relative to the device origin, for example
`http://m45-firmware.local/api/status`.

Most endpoints return `application/json` and set no-store cache headers. Errors
use:

```json
{"ok":false,"error":"message"}
```

Some UI endpoints support `X-Page-Token`. If the header is omitted, the request
is accepted. If it is present and does not match the latest `page_token` from
`GET /api/status`, the response is `409 Conflict`:

```json
{"page_token":"...","reload":true}
```

## M45 Endpoints

### `GET /api/status`

Returns live device status. This is the primary dashboard polling endpoint.

Supports `X-Page-Token`.

Response fields:

| Field | Type | Notes |
| --- | --- | --- |
| `page_token` | string | Token used by the web UI for stale-page detection. |
| `device_name`, `version`, `build_id`, `build_time` | string | Firmware identity. |
| `ota_supported` | boolean | Whether an OTA partition is available. |
| `wifi_connected`, `ip`, `wifi_rssi_dbm` | boolean/string/number | Wi-Fi state. |
| `hardware`, `booting`, `setup_mode`, `setup_ssid`, `setup_ip` | mixed | Hardware and setup mode state. |
| `asic_ready`, `asic_power_enabled`, `model`, `asic_model`, `asic_chips` | mixed | ASIC identity and power state. |
| `frequency_mhz`, `hashrate_ghs`, `hashrate_nominal_ghs` | number | Current clock and hashrate. |
| `domain_hashrate_ghs`, `domain_hashrates_ghs` | number/array | Domain hashrate totals. |
| `asic_error_rate_percent`, `expected_hashrate_ghs` | number | Performance estimates. |
| `voltage_mv`, `voltage_base_mv`, `voltage_temp_compensation_enabled`, `voltage_temp_compensation_mv` | mixed | Effective ASIC voltage target. `voltage_temp_compensation_mv` is signed relative to the base voltage. |
| `overclock_enabled`, `auto_clock_enabled`, `auto_clock_active`, `auto_domain_reboot_enabled`, `safety_limits_unrestricted` | boolean | Overclock, auto-clock, lost-domain watchdog, and unrestricted safety-limit state. |
| `auto_clock_target_temp_c`, `auto_clock_target_frequency_mhz`, `auto_clock_target_voltage_mv`, `auto_clock_next_up_frequency_mhz`, `auto_clock_power_now_w`, `auto_clock_power_target_w`, `auto_clock_next_up_power_w`, `auto_clock_thermal_resistance_c_per_w`, `auto_clock_output_current_ceiling_a`, `auto_clock_next_up_output_current_a`, `auto_clock_input_voltage_limited`, `auto_clock_output_current_limited`, `auto_clock_vr_temp_limited`, `auto_clock_power_limited`, `auto_clock_temperature_limited`, `auto_clock_hold_reason` | mixed | Auto-clock target, controller telemetry, and clock-limiting state. The `power_*_w` fields are ASIC heat load and estimated cooling target watts derived from temperature headroom, not a configured watt limit. |
| `asic_temp_c`, `fan_percent`, `fan_rpm`, `fan_auto`, `fan_auto_off_allowed`, `fan_target_temp_c` | mixed | Cooling state. |
| `tps546_valid`, `tps546_read_vout`, `tps546_read_vin`, `tps546_read_iout`, `tps546_temp_c`, `tps546_model` | mixed | Regulator telemetry. |
| `asic_power_watts`, `asic_efficiency_j_per_th`, `power_fault`, `hardware_fault`, `hardware_fault_msg` | mixed | Power, efficiency, and fault state. `asic_efficiency_j_per_th` is computed from ASIC watts and measured hashrate. |
| `pool`, `pool_port`, `pool_using_backup`, `stratum_connected`, `stratum_connected_seconds`, `stratum_response_ms` | mixed | Pool connection state. |
| `stratum_share_submit_us`, `stratum_share_submit_max_us`, `stratum_share_write_us`, `stratum_share_write_max_us` | number | Native M45 share-submit timing in microseconds. |
| `work_received`, `shares_accepted`, `shares_rejected`, `valid_nonces`, `nonce_errors` | number | Mining counters. |
| `best_diff`, `pool_difficulty`, `pool_difficulty_auto`, `pool_suggested_difficulty` | mixed | Difficulty state. |
| `payout_status`, `payout_percent_x100` | string/number | Coinbase payout detection. |
| `block_alert_active`, `block_alert_diff` | boolean/number | Block-found alert state. |
| `limits` | object | Active safety limits. |
| `asic_loss` | object | Present only when built with `M45_ASIC_LOSS_METRICS`. |

`limits` contains:

```json
{
  "input_voltage_min_v": 4.8,
  "input_voltage_expected_min_v": 4.95,
  "input_voltage_expected_max_v": 5.4,
  "input_voltage_max_v": 5.5,
  "asic_voltage_min_v": 0.7,
  "asic_voltage_expected_min_v": 1.075,
  "asic_voltage_expected_max_v": 1.225,
  "asic_voltage_max_v": 1.4,
  "asic_voltage_target_v": 1.15,
  "asic_temp_expected_max_c": 65.0,
  "asic_temp_max_c": 69.0,
  "tps546_temp_expected_max_c": 90.0,
  "tps546_temp_max_c": 110.0,
  "iout_warn_a": 29.0,
  "iout_fault_a": 33.0,
  "power_warn_w": 33.35,
  "power_fault_w": 37.95,
  "fan_expected_percent": 100
}
```

### `GET /api/setup`

Returns the reduced setup-page configuration used before the device has joined
Wi-Fi.

Response fields:

| Field | Type |
| --- | --- |
| `setup_mode`, `pool_password_set`, `pool_difficulty_auto` | boolean |
| `setup_ssid`, `setup_ip`, `wifi_ssid`, `pool_host`, `pool_user` | string |
| `pool_port`, `pool_difficulty`, `pool_suggested_difficulty` | number |

### `POST /api/setup`

Saves first-boot Wi-Fi and primary pool settings, then reboots.

The request is rejected with `409 Conflict` unless the same Wi-Fi credentials
have passed `POST /api/wifi-test`.

Request body:

```json
{
  "wifi_ssid": "network",
  "wifi_password": "password",
  "pool_host": "public-pool.io",
  "pool_port": 3333,
  "pool_user": "wallet.worker",
  "pool_pass": "x",
  "pool_difficulty_auto": true,
  "pool_difficulty": 1000
}
```

Response:

```json
{"ok":true,"rebooting":true}
```

### `GET /api/networks`

Returns nearby Wi-Fi networks. A scan is started automatically when there is no
cached scan. Pass `?refresh=1` or `?refresh=true` to start a new scan.

While scanning:

```json
{"scanning":true,"networks":[]}
```

When complete:

```json
{
  "scanning": false,
  "networks": [
    {"ssid":"network","rssi":-51,"channel":6,"auth_open":false}
  ]
}
```

### `POST /api/wifi-test`

Starts an asynchronous Wi-Fi connection test. Only one test runs at a time.

Request body:

```json
{"wifi_ssid":"network","wifi_password":"password"}
```

Running response, `202 Accepted`:

```json
{"ok":false,"running":true,"id":1}
```

Success response:

```json
{"ok":true,"running":false,"id":1,"connected":true,"ip":"192.168.1.50","rssi_dbm":-51}
```

Failure response, `400 Bad Request`:

```json
{"ok":false,"running":false,"id":1,"error":"authentication failed","reason":15}
```

### `GET /api/wifi-test`

Returns the current or most recent Wi-Fi test state. Pass `?id=N` to require a
specific test id.

Responses use the same shape as `POST /api/wifi-test`. If no matching test is
available, the response is `404 Not Found`.

### `GET /api/settings`

Returns the full saved settings used by the Settings and Overclock pages.

Response fields:

| Field | Type | Notes |
| --- | --- | --- |
| `wifi_ssid`, `hostname`, `pool_host`, `backup_pool_host`, `pool_user` | string | Saved string settings. |
| `pool_port`, `backup_pool_port` | number | 1-65535. |
| `wifi_password_set`, `pool_password_set` | boolean | Password values are not returned. |
| `pool_difficulty`, `pool_difficulty_auto`, `pool_suggested_difficulty` | mixed | Pool difficulty settings. |
| `overclock_enabled`, `auto_clock_enabled`, `auto_domain_reboot_enabled`, `asic_voltage_temp_compensation_enabled` | boolean | ASIC tuning flags. Auto clock is experimental and requires overclocking plus either fixed fan speed or no fan mode, and a high-current stable 5 V power supply with wiring/connectors rated for the configured current limits. Auto clock is capped at 1200 MHz and also blocks preset increases when cooling temperature headroom is too low, VIN is at or below 5.01 V, or the board is near configured TPS546 output-current and VR-temperature safety limits. Auto domain reboot is off by default; when enabled, the ASIC is power-cycled if a domain remains below 75% of expected per-domain hashrate for at least 60 seconds, and pending lost-domain recovery can tolerate missing ASIC temperature reads until the reboot runs. |
| `asic_frequency_mhz`, `asic_voltage_mv`, `overclock_voltage_offset_mv`, `auto_clock_target_temp_c` | number | ASIC tuning values. |
| `fan_override_enabled`, `fan_override_percent`, `fan_auto_off_allowed`, `fan_target_override_enabled`, `fan_target_temp_c` | mixed | Fan settings. |
| `display_screensaver_enabled`, `display_sleep_minutes`, `display_sleep_max_minutes` | mixed | OLED sleep settings. |
| `safety_limits_unrestricted` | boolean | Allows out-of-normal safety limit settings when true. |
| `limit_*` | number | Safety limits in millivolts, degrees C, or deciamps. |

Safety limit fields:

```text
limit_input_voltage_min_mv
limit_input_voltage_expected_min_mv
limit_input_voltage_expected_max_mv
limit_input_voltage_max_mv
limit_asic_voltage_min_mv
limit_asic_voltage_max_mv
limit_asic_temp_expected_max_c
limit_asic_temp_max_c
limit_tps546_temp_expected_max_c
limit_tps546_temp_max_c
limit_iout_warn_deciamps
limit_iout_fault_deciamps
```

### `POST /api/settings`

Saves and applies full runtime settings.

Passwords are optional. Omit them, or omit masked UI values, to keep existing
passwords. If Wi-Fi credentials change, the same credentials must first pass
`POST /api/wifi-test`.

Request body includes the fields returned by `GET /api/settings`, plus optional
password values:

```json
{
  "wifi_ssid": "network",
  "wifi_password": "password",
  "hostname": "M45-Firmware-aabbcc",
  "pool_host": "public-pool.io",
  "pool_port": 3333,
  "backup_pool_host": "public-pool.io",
  "backup_pool_port": 3333,
  "pool_user": "wallet.worker",
  "pool_pass": "x",
  "pool_difficulty_auto": true,
  "pool_difficulty": 1000,
  "overclock_enabled": false,
  "auto_clock_enabled": false,
  "auto_domain_reboot_enabled": false,
  "auto_clock_target_temp_c": 62,
  "asic_frequency_mhz": 525,
  "asic_voltage_mv": 1150,
  "overclock_voltage_offset_mv": 0,
  "asic_voltage_temp_compensation_enabled": true,
  "fan_override_enabled": false,
  "fan_override_percent": 100,
  "fan_auto_off_allowed": false,
  "fan_target_override_enabled": false,
  "fan_target_temp_c": 62,
  "display_screensaver_enabled": true,
  "display_sleep_minutes": 0,
  "safety_limits_unrestricted": false,
  "limit_input_voltage_min_mv": 4800,
  "limit_input_voltage_expected_min_mv": 4950,
  "limit_input_voltage_expected_max_mv": 5400,
  "limit_input_voltage_max_mv": 5500,
  "limit_asic_voltage_min_mv": 700,
  "limit_asic_voltage_max_mv": 1400,
  "limit_asic_temp_expected_max_c": 65,
  "limit_asic_temp_max_c": 69,
  "limit_tps546_temp_expected_max_c": 90,
  "limit_tps546_temp_max_c": 110,
  "limit_iout_warn_deciamps": 290,
  "limit_iout_fault_deciamps": 330
}
```

Normal safety limit ranges are enforced when `safety_limits_unrestricted` is
false. When true, limit values may use the full unsigned 16-bit range, but the
relationships below are still required:

```text
input min < input max
input expected min < input expected max
input min <= input expected min
input expected max <= input max
ASIC voltage min < ASIC voltage max
ASIC expected temp <= ASIC stop temp
TPS546 expected temp <= TPS546 stop temp
IOUT warn <= IOUT fault
effective ASIC voltage is within ASIC voltage min/max
```

Response:

```json
{"ok":true,"restart":false,"wifi_reconnect":false,"pool_reconnect":false}
```

### `POST /api/runtime-tune`

Applies temporary ASIC and fan tuning without saving to NVS.

Request body fields are optional:

```json
{
  "overclock_enabled": true,
  "auto_clock_enabled": false,
  "auto_clock_target_temp_c": 62,
  "frequency_mhz": 625,
  "asic_frequency_mhz": 625,
  "voltage_mv": 1200,
  "asic_voltage_mv": 1200,
  "asic_voltage_temp_compensation_enabled": true,
  "fan_override_enabled": true,
  "fan_override_percent": 100,
  "fan_auto_off_allowed": false,
  "fan_target_override_enabled": true,
  "fan_target_temp_c": 62
}
```

`frequency_mhz` and `asic_frequency_mhz` are aliases. `voltage_mv` and
`asic_voltage_mv` are aliases.

Response:

```json
{"ok":true,"runtime":true}
```

### `POST /api/asic-power`

Turns ASIC power on or off.

Request body:

```json
{"enabled":false,"manage_fan":true}
```

`manage_fan` is optional and defaults to `true`.

Response:

```json
{"ok":true,"asic_power_enabled":false,"asic_ready":false}
```

If enabling power while a hardware fault is latched, the response is
`409 Conflict`.

### `POST /api/settings/factory-reset`

Clears saved settings, restores defaults, clears Wi-Fi driver settings, and
reboots.

Response:

```json
{"ok":true,"restart":true,"rebooting":true,"wifi_reconnect":false,"pool_reconnect":false}
```

### `POST /api/best-diff/reset`

Resets best difficulty.

Response:

```json
{"ok":true,"best_diff":0}
```

### `POST /api/block-alert/dismiss`

Dismisses the block-found alert.

Response:

```json
{"ok":true,"block_alert_active":false}
```

### `POST /api/ota`

Uploads and installs firmware, then reboots.

Supports `X-Page-Token`.

Request body is binary, not JSON. Accepted payloads are:

- `M45-Firmware.bin` app images.
- Gamma 602 factory images where the app image can be extracted from the
  embedded partition table.

Limits and errors:

- Maximum payload size is 9 MiB.
- Returns `409 Conflict` in setup mode.
- Returns `409 Conflict` if no OTA partition is available.
- Returns `400 Bad Request` for unsupported or invalid firmware images.

Success response:

```json
{"ok":true,"rebooting":true}
```

### `POST /api/reboot`

Schedules a reboot.

Response:

```json
{"ok":true,"rebooting":true}
```

## Non-JSON API Helpers

These endpoints are part of the API surface but do not return JSON.

| Method | Path | Response |
| --- | --- | --- |
| `GET` | `/api/logs` | Plain text log stream. Query: `since=<seq>`, `verbose=1` or `verbose=true`. Headers: `X-Log-Next`, `X-Log-Truncated`. Supports `X-Page-Token`. |
| `GET` | `/health` | Plain text `ok`. |

## ESP-Miner Compatibility Endpoints

The compatibility routes are intended for tools that expect the Bitaxe
ESP-Miner API shape. These endpoints set CORS headers:

```text
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, PATCH, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Page-Token
```

### `OPTIONS /api/*`

Preflight response for compatibility API requests.

Response status: `204 No Content`.

### `GET /api/system/info`

Returns a broad ESP-Miner-style status and configuration object.

Important response fields:

| Field group | Fields |
| --- | --- |
| Power and thermal | `power`, `voltage`, `current`, `temp`, `temp2`, `vrTemp`, `coreVoltageActual`, `actualFrequency`, `expectedHashrate` |
| Fans | `fanspeed`, `fanrpm`, `fan2rpm`, `autofanspeed`, `manualFanSpeed`, `minFanSpeed`, `temptarget` |
| Mining | `hashRate`, `hashRate_1m`, `hashRate_10m`, `hashRate_1h`, `errorPercentage`, `sharesAccepted`, `sharesRejected`, `bestDiff`, `bestSessionDiff`, `poolDifficulty`, `responseTime`, `blockFound`, `showNewBlock`, `miningPaused` |
| Memory and uptime | `freeHeap`, `freeHeapInternal`, `freeHeapSpiram`, `uptimeSeconds`, `cpuUsage` |
| Wi-Fi | `wifiStatus`, `wifiRSSI`, `ssid`, `wifiPass`, `ipv4`, `ipv6`, `apEnabled` |
| Firmware and board | `version`, `axeOSVersion`, `idfVersion`, `boardVersion`, `maxPower`, `nominalVoltage`, `smallCoreCount`, `ASICModel`, `isPSRAMAvailable`, `resetReason`, `runningPartition`, `macAddr`, `hostname`, `otaSupported` |
| Stratum | `poolConnectionInfo`, `isUsingFallbackStratum`, `stratumURL`, `stratumPort`, `stratumUser`, `stratumSuggestedDifficulty`, `stratumExtranonceSubscribe`, `stratumTLS`, `stratumCert`, `stratumDecodeCoinbase`, `fallbackStratumURL`, `fallbackStratumPort`, `fallbackStratumUser`, `fallbackStratumSuggestedDifficulty`, `fallbackStratumExtranonceSubscribe`, `fallbackStratumTLS`, `fallbackStratumCert`, `fallbackStratumDecodeCoinbase`, `stratumProtocol`, `activeProtocolLabel`, `activePool`, `activePoolPort` |
| Tuning and display | `overclockEnabled`, `display`, `rotation`, `invertscreen`, `displayTimeout`, `coreVoltage`, `frequency`, `statsFrequency`, `statsLimit`, `boardtemp1`, `boardtemp2` |
| Arrays/objects | `hashrateMonitor`, `sharesRejectedReasons`, `blockSignals`, `coinbaseOutputs` |

Password values are masked as `*****` when saved.

### `GET /api/system/asic`

Returns ASIC metadata and option lists.

Response:

```json
{
  "ASICModel": "BM1370",
  "deviceModel": "Gamma",
  "swarmColor": "blue",
  "asicCount": 1,
  "hashDomains": 10,
  "defaultFrequency": 525,
  "frequencyOptions": [400,425,450,475,500,525,550,575,600,625,650,700,750,800,850,900,950,1000,1100,1200,1300,1400,1500],
  "defaultVoltage": 1150,
  "voltageOptions": [700,750,800,850,900,950,1000,1050,1100,1150,1200,1250,1300,1350,1370]
}
```

### `GET /api/system/statistics`

Returns one row of ESP-Miner-style statistics.

Optional query:

```text
columns=hashrate,asicTemp,power
```

If `columns` is omitted or no requested labels are recognized, all labels are
returned.

Supported labels:

```text
hashrate
hashrate_1m
hashrate_10m
hashrate_1h
errorPercentage
asicTemp
asicTemp2
vrTemp
asicVoltage
voltage
power
current
fanSpeed
fanRpm
fan2Rpm
wifiRssi
freeHeap
responseTime
```

Response shape:

```json
{
  "currentTimestamp": 1710000000000,
  "labels": ["hashrate","asicTemp","power","timestamp"],
  "statistics": [[1234.5,58.0,31.2,1710000000000]]
}
```

### `GET /api/system/scoreboard`

Returns an empty scoreboard array.

Response:

```json
[]
```

### `GET /api/system/wifi/scan`

Starts or reads a Wi-Fi scan using the ESP-Miner response shape.

While scanning:

```json
{"networks":[]}
```

When complete:

```json
{"networks":[{"ssid":"network","rssi":-51,"authmode":3}]}
```

### `PATCH /api/system`

Applies ESP-Miner-style settings and saves them to NVS.

All fields are optional:

```json
{
  "hostname": "M45-Firmware-aabbcc",
  "ssid": "network",
  "wifiPass": "password",
  "stratumURL": "public-pool.io",
  "stratumPort": 3333,
  "stratumUser": "wallet.worker",
  "stratumPassword": "x",
  "fallbackStratumURL": "public-pool.io",
  "fallbackStratumPort": 3333,
  "overclockEnabled": false,
  "frequency": 525,
  "coreVoltage": 1150,
  "autofanspeed": true,
  "manualFanSpeed": 100,
  "temptarget": 62,
  "stratumSuggestedDifficulty": 1000
}
```

Notes:

- `wifiPass` and `stratumPassword` preserve existing values when set to
  `*****`.
- `stratumSuggestedDifficulty` of `0` enables automatic difficulty.
- `autofanspeed` accepts JSON booleans or numeric `0`/`1`.
- `manualFanSpeed` of `0` means fan off when manual fan mode is active.

Response:

```json
{"ok":true,"restart":false,"pool_reconnect":false}
```

`restart` is true when Wi-Fi credentials changed.

### `POST /api/system/restart`

Schedules a reboot.

Response:

```json
{"message":"System will restart shortly."}
```

### `POST /api/system/pause`

Pauses mining work dispatch.

Response:

```json
{"message":"Mining paused"}
```

### `POST /api/system/resume`

Resumes mining work dispatch.

Response:

```json
{"message":"Mining resumed"}
```

### `POST /api/system/identify`

Acknowledges identify requests. There is no device-side identify action yet.

Response:

```json
{"message":"Identify acknowledged"}
```

### `POST /api/system/blockFound/dismiss`

Dismisses the block-found notification.

Response:

```json
{
  "blockFound": 0,
  "showNewBlock": false,
  "message": "Block found notification dismissed"
}
```

### `POST /api/system/OTA`

Compatibility alias for `POST /api/ota`. The request body is binary firmware,
not JSON. The success and error responses match `/api/ota`.

### `POST /api/system/OTAWWW`

Always returns `501 Not Implemented`.

Response:

```json
{"ok":false,"error":"WWW OTA partition is not available"}
```

### `GET /api/system/logs`

Compatibility alias for `GET /api/logs`.

The response is plain text, not JSON, with a `Content-Disposition` attachment
header for `m45-logs.txt`.
