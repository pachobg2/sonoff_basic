# Sonoff Basic 1 — firmware

Converted from `sonoff-basic-1.yaml` (ESPHome) to raw Arduino C++, rewritten
line-by-line against the actual current `plugin_light.ino` source (not just
its spec doc) so the two devices share the same code shape. **Not yet
flashed/tested on hardware** — see the verification note below.

## Files

`sonoff_basic.ino`, `config.h.example`, this `README.md`.

## Hardware

- ESP8266 (Sonoff Basic R1/R2, 1MB flash)
- Relay on GPIO12, active-high
- Onboard blue status LED on GPIO13, active-low — mirrors relay state
- Physical button on GPIO0, active-low (external pull-up on the board;
  `INPUT_PULLUP` also set in firmware) — toggles the relay locally
- Model tag: `P.Switch`, firmware version `1.0.0`

## Stack / conventions (matches plug_light1's actual source)

- `espMqttClient` (bertmelis) — QoS 1 publishes and subscribe, PUBACK
  confirmation handled by the library.
- No ArduinoJson, no JSON on the wire for the switch itself — plain
  `ON`/`OFF` payloads. HA discovery configs are built with manual `String`
  concatenation, same as plug_light1 — a `deviceJson` fragment is built
  once and reused across all four discovery payloads.
- Plain MQTT switch schema (`command_topic`/`state_topic` with
  `payload_on`/`payload_off`), the switch equivalent of plug_light1's
  plain light schema. No `payload_available`/`payload_not_available`
  fields in the discovery payloads — HA's defaults (`online`/`offline`)
  already match what's published, so plug_light1 omits them and this does
  too.
- The primary entity's `unique_id` is the bare `DEVICE_ID` (not
  `DEVICE_ID + "_switch"`); diagnostic sensors get a `_suffix`. Diagnostic
  sensor `name`s are bare (`"WiFi Signal"`, not `"Sonoff Basic 1 WiFi
  Signal"`) since HA already groups them under the device.
- Status LED always mirrors relay state, whatever triggered the change
  (MQTT command or the physical button) — matches the YAML's
  `on_turn_on`/`on_turn_off` automations on the relay switch, which drove
  the LED unconditionally.
- WiFi: non-blocking connect with a 15s per-attempt timeout and immediate
  retry (`connectWifi()`/`maintainWifi()`) — no backoff on WiFi itself.
  MQTT: exponential backoff reconnect (1s doubling to a 30s cap). One
  bounded 5s wait at boot only (not repeated) to give OTA's mDNS responder
  and the first MQTT attempt a real chance at a live link before `loop()`
  takes over. `mqttClient.loop()` called every `loop()` iteration.
- Every publish goes through `checkedPublish()` — same signature as
  plug_light1's (`topic, qos, retain, payload`, returns `bool`) — logs to
  Serial on failure (topic + free heap).
- `WiFi.hostname(DEVICE_ID)` and `mqttClient.setClientId(DEVICE_ID)` set
  explicitly, matching plug_light1.
- LWT: `switch/sonoff_basic_1/availability` = `online`/`offline`, retained.
- Diagnostics via a single `publishDiagnostics(bool force)`: WiFi signal
  (RSSI, dBm) every call, reset reason once ever (unless `force`), MQTT
  fail count (only counted after first successful connect via
  `everConnected`) every call. Republished every 2 minutes
  (`DIAG_INTERVAL_MS`), same cadence as plug_light1.
- Relay state persisted and restored on boot — see **Behavior notes**
  below, the one place this intentionally differs from the literal YAML.
- Plain DHCP, no static IP in firmware.
- `ArduinoOTA` for over-the-air updates after the first USB flash, hostname
  = `DEVICE_ID` (no separate OTA hostname setting, matching plug_light1).
- Manufacturer tag: `P@cho`.

## Behavior notes / assumptions made during conversion

1. **State restore on boot.** The YAML's `switch:` block doesn't set
   `restore_mode`, so ESPHome's default for a GPIO switch is `ALWAYS_OFF`
   — the relay would come up off after every reboot/power cycle. This
   firmware instead **persists relay state to flash and restores it on
   boot**, matching plug_light1's persistence rather than the literal YAML
   default. If you want the original always-off-on-boot behavior instead,
   change `loadPersistedState()` to just set `relayState = false;`.
2. **Model tag.** `P.Switch` was chosen to parallel plug_light1's
   `P.Light`; change it in `config.h` if `smart_switch`'s actual tag is
   different (I don't have that device's `config.h` to confirm it).
3. **Topic layout.** `switch/sonoff_basic_1/...`, following plug_light1's
   domain-prefixed convention (matches `smart_switch`'s
   `switch/smart_switch/...` layout).
4. **EEPROM vs Preferences.** ESP8266's Arduino core doesn't have the
   `Preferences`/NVS library (that's ESP32-only), so persistence uses
   `EEPROM.h` (flash-backed emulation) instead — one byte, relay state
   only. `ESP.getResetReason()` is used directly for the reset-reason
   string, since ESP8266's core already returns a readable string (ESP32
   needs a manual enum-to-string switch, which is why plug_light1 has a
   `resetReasonString()` helper that this sketch doesn't need).

## MQTT topics (plain text, no JSON)

| Purpose | Topic | Payload |
|---|---|---|
| Command (on/off) | `switch/sonoff_basic_1/set` | `ON` / `OFF` |
| State (retained) | `switch/sonoff_basic_1/state` | `ON` / `OFF` |
| Availability / LWT | `switch/sonoff_basic_1/availability` | `online` / `offline` |
| WiFi signal | `switch/sonoff_basic_1/wifi_signal/state` | dBm |
| MQTT fail count | `switch/sonoff_basic_1/mqtt_fail_count/state` | integer |
| Reset reason | `switch/sonoff_basic_1/reset_reason/state` | string |

Discovery configs under `homeassistant/switch/sonoff_basic_1/config` and
`homeassistant/sensor/sonoff_basic_1/<sensor>/config`.

## Config file

Credentials, device identity, and hardware pins live in one `config.h` —
rename `config.h.example` to `config.h` and fill in WiFi/MQTT/OTA
credentials plus `DEVICE_ID`/`DEVICE_NAME`/`MANUFACTURER`/`MODEL`/
`FW_VERSION`. No static IP block. Note the MQTT username macro is
`MQTT_USER` (matching plug_light1's config.h), not `MQTT_USERNAME`.

## Toolchain

Arduino IDE, board package "esp8266 by ESP8266 Community". Libraries:
`espMqttClient` only — no ArduinoJson.

## Verification note

`espMqttClient`'s API (`setServer`/`setCredentials`/`setClientId`/
`setWill`, the `onConnect`/`onDisconnect`/`onMessage` callback signatures,
and `publish`/`subscribe` return values) and the ESP8266-specific calls
(`WiFi.hostname()`, `ESP.getResetReason()`, `EEPROM.begin/read/write/
commit`) were checked directly against the espMqttClient and ESP8266
Arduino core GitHub sources, since a full toolchain compile wasn't
possible in the sandbox that generated this. **This has not been flashed
to real hardware yet.** Given plug_light1's actual bring-up bugs were a
wrong pin in `config.h`, a stale-WiFi-config crash, and a JSON-schema
light that didn't show up in HA, pay particular attention on first flash
to: `RELAY_PIN`/`STATUS_LED_PIN`/`BUTTON_PIN` against your actual board
wiring, and confirm the switch entity appears in HA via discovery.
