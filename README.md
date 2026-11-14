# lastDSMRlogger

Buildnumber: 1
Last update-date: 2026-11-15

ESP-IDF application for the ESP32-S3 on the TFT LCD Display EC11 Piggyback
Board v3. It reads P1 telegrams from a smart meter through parserDSMR and
shows recent telegrams in a single-page web application with two views:
Settings and Telegrams. Configuration is stored in NVS and survives a power
cycle; the recent telegram history lives in RAM.

## 1. Project layout

|Path                              |Role                                                                                   |
|----------------------------------|---------------------------------------------------------------------------------------|
|`main/`                           |Application entry, NVS init, Wi-Fi provisioner task, P1 interface Kconfig              |
|`components/logger_settings/`     |Versioned NVS settings service, validation against parser profiles and board allowlist |
|`components/logger_receiver/`     |Receive task ownership of UART adapter + parser, diagnostics, RAM telegram history     |
|`components/logger_web/`          |esp_http_server: SPA assets and JSON API                                               |
|`components/logger_board/`        |On-device TFT debug lines through the esp32_s3_piggyback component                     |
|`components/esp32_s3_piggyback/`  |Vendored board component from mrWheel (pinned, see section 3)                          |
|`../parserDSMR/components/parserDSMR/` |Pinned parser dependency via EXTRA_COMPONENT_DIRS (see section 3)                 |

## 2. Build, flash, monitor

Activate ESP-IDF 6.0.2 first (the ESP-IDF exports script is provided by the
installer):

```
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
```

Normal build path:

```
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Flash and upload are performed by the user; this project never flashes
automatically.

## 3. Dependencies and pinned revisions

|Dependency                |Source                                                                                                     |Pin                                                          |
|--------------------------|-----------------------------------------------------------------------------------------------------------|-------------------------------------------------------------|
|parserDSMR                |`https://github.com/mrWheel/parserDSMR` checkout at `../parserDSMR`                                         |revision `5b4004508c6e6bf6f507c8c1931f5d458c71797f` + local raw-frame API changes (pending commit)|
|esp32_s3_piggyback        |`https://github.com/mrWheel/esp32-s3-Piggyback`, component directory `components/esp32_s3_piggyback`       |commit `cd2901728f2acde393a2bc88c96b6e77ba9275c3`, vendored under `components/` |
|esp-idf-wifi-provisioner  |ESP Component Registry `michmich/esp-idf-wifi-provisioner`                                                  |version `0.4.0` (upstream commit `2c7aaa6aa0e319a429186ee470e137b8579f632f`) |

parserDSMR is consumed as the checkout's `components/parserDSMR` directory
through `EXTRA_COMPONENT_DIRS` in the root `CMakeLists.txt`. The checkout
preserves the relative symlinks from that component directory to the root
`src/`, `include/` and `adapter/` directories. Clone it beside this project:

```
cd ..
git clone https://github.com/mrWheel/parserDSMR.git
```

The raw byte view on `dsmrTelegramResult` (`rawBytes`/`rawLength`, the exact
accepted frame including every original line ending and trailer byte) and the
UART overflow/framing error reporting in the adapter are local additions to
parserDSMR; commit them in the parser repository and update the pin above.

The provisioner is resolved into `managed_components/` by the ESP-IDF
component manager at configure time.

## 4. Hardware and P1 wiring

The P1 data signal passes through a P1 interface board that provides signal
conditioning and a 3.3 V-compatible signal at the ESP32-S3 input.

|Signal     |Connection                                                                       |
|-----------|---------------------------------------------------------------------------------|
|P1 data    |P1 interface output → J5 pin of one selectable GPIO: 10, 38, 39, 40, 41, 42, 47, 48 |
|P1 GND     |P1 interface GND → board GND (common ground with the meter interface)            |
|P1 request |Externally enabled in this release (the meter's request line stays asserted)     |
|Console    |UART0 on GPIO43/44 (USB connector of the board), never used for P1                |

The receiver uses hardware UART1 with RX on the selected J5 GPIO and no TX
pin. GPIO43/44 remain reserved for the console UART0. The board component
reserves the display (2, 4, 5, 11, 12, 15) and, when inputs are used, the
encoder and buttons (1, 6, 16, 17); the encoder/button inputs are never
started, so those pins stay untouched. All eight selectable RX pins are wired
only to J5 and are free for general use.

RX polarity is a build-time interface setting: choose
`lastDSMRlogger P1 Interface → P1 RX signal polarity` in
`idf.py menuconfig`. Use *Inverted* only when the interface board presents an
inverted signal; otherwise the default *Direct* applies.

## 5. Wi-Fi first-boot setup

1. Flash and monitor. On first boot (no stored credentials) the device opens
   the captive portal access point `lastDSMRlogger-Setup` (open network).
2. Connect a phone or laptop to that AP and open `http://192.168.4.1/`.
   The board display shows the portal state.
3. Submit your SSID and password. The device joins the network; the display
   shows the SSID and the assigned IP address, and the console log prints it
   as well (`station joined '<SSID>' with IP <ip>`).
4. Open `http://<ip>/` on the local network.

With stored credentials the device connects directly and never opens the
portal. P1 reception keeps running while Wi-Fi is disconnected; after a
reconnect the web interface is reachable again without a reboot. To
re-provision, erase NVS: `idf.py -p PORT erase-flash` before flashing.

How to find the IP: the serial monitor line above, or the DHCP client list of
your router for the host name `esp32-s3-<mac>`.

## 6. Web interface

The SPA is served by the device from the same origin as the API, with all
assets embedded in the firmware. No external service is required. All
dynamic data (status, telegram list, settings, details, raw frames) travels
over one WebSocket at `/ws`: the device pushes status every second and the
telegram list whenever a telegram was accepted, the browser sends
form-urlencoded commands, and raw telegram bytes arrive as a binary frame.
The HTTP API remains available for diagnostics and clients without
WebSocket support. The SPA opens with the **Telegrams** view.

- **Settings**: prefilled with the configuration currently stored in NVS and
  refilled after every save, so the form always shows the settings in force.
  P1 RX GPIO (board allowlist, plus the *Test data* selection), country,
  protocol profile (human-readable label, stable ID persisted, kept separate
  from the version reported by the meter), and M-Bus channels 1–4 with Auto,
  Disabled or an explicit device type. All dropdowns are filled from the
  valid choices the device reports. Save validates and applies the complete
  configuration as one operation and stores it in NVS; on any failure the
  active configuration is preserved and the reason is shown.
- **Telegrams**: updates automatically, latest accepted telegram by default,
  selection of a recent record. New arrivals leave the selected record in
  view; an aged-out selection is reported with a button back to the latest.
  Each record shows sequence, receive time (uptime and age: wall clock time
  is unavailable in this release), active profile, reported meter version,
  validation status, the decoded summary (parser-supplied values, units and
  timestamps; disabled channels omitted; a channel without a reading shows
  `(unavailable)` while a measured zero stays visible) and the raw frame
  shown as plain text with original line endings and trailer.

On-device TFT debug lines show the active browser screen, the last accepted
telegram, and the Wi-Fi portal/connected state.

### API

|Method |Path                        |Purpose                                                       |
|-------|----------------------------|--------------------------------------------------------------|
|WS     |`/ws`                       |SPA transport: status/telegram pushes + `cmd=config\|save\|status\|telegrams\|telegram\|raw\|view`, raw bytes as binary frame |
|GET    |`/api/config`               |NVS settings + active state + valid choices (GPIOs, countries, profiles) |
|POST   |`/api/config`               |Save form-urlencoded settings (one atomic operation)          |
|GET    |`/api/status`               |Reception state, counters, last diagnostic, uptime            |
|GET    |`/api/telegrams`            |Metadata of retained records, newest first                    |
|GET    |`/api/telegram/<seq>`       |Decoded detail of one record                                  |
|GET    |`/api/telegram/<seq>/raw`   |Exact accepted frame bytes as `text/plain`                    |
|POST   |`/api/view`                 |Reports the active browser screen name to the device display  |

### Test data mode

Selecting **Test data (no UART)** (RX GPIO `-1`) in the Settings view starts
an internal generator instead of the UART receiver. It feeds complete,
CRC-correct synthetic telegrams through the same parser and event path at
the profile cadence (clamped to 1–5 s), randomising the timestamps and the
measurement values (power, energy totals, tariff, voltages, currents and,
once an M-Bus channel is declared as an expected device type, its reading).
The frames follow the shapes of the verified parser fixtures, so Dutch and
Belgian profiles both decode. The status strip then shows
*test data (no UART)*. This selection is validated like any other pin: it
must pass through the settings service, and it is the only value outside the
J5 allowlist that is accepted.

## 7. RAM limits (documented target configuration)

|Item                                    |Size                                                            |
|----------------------------------------|----------------------------------------------------------------|
|Parser workspace (telegram + raw copy)  |`DSMR_DEFAULT_WORKSPACE_BYTES` ≈ 40 KB (2 × 12 KB + tables)     |
|Telegram history                        |4 records × (12 KB raw + up to 96 field snapshots) ≈ 100 KB BSS |
|Requirement                             |accommodates at least two maximum-sized telegram records        |

The oldest record is evicted first. Rejected candidates only update
diagnostics and counters; the last accepted record stays intact. HTTP
handlers copy consistent snapshots and never hold the receive-side lock while
writing to the network, so slow or disconnected browser clients have no
effect on UART reception.

## 8. Hardware verification

1. `idf.py -p PORT flash monitor` shows the board debug lines after boot.
2. Open the SPA, choose a J5 GPIO (for example GPIO10), country and profile,
   then Save. The console shows `receiving on UART 1 at ... baud, profile ...`
   and the status strip moves from *awaiting configuration* to *awaiting data*
   to *accepted telegram*.
3. With a conditioned P1 signal on the selected pin, telegrams appear in the
   Telegrams view at the profile cadence; console and reception operate
   together.
4. Power-cycle the device: the configuration survives and reception restarts.
5. Reserved peripheral pins (display, encoder, buttons, SD connector, I²C
   header) remain available for their board functions.
6. Belgian profile acceptance is pending: fixtures must be independently
   verified against Fluvius publications before release sign-off.

## 9. Repository notes

- Never remove existing functionality; this repository keeps Allman braces,
  2-space indentation, lowerCamelCase names and `//--` comments per
  `codingRules.md`.
- The Markdown files of this project stay in English and carry Buildnumber
  and Last update-date metadata near the top.
