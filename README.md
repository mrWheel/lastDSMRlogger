# lastDSMRlogger

Buildnumber: 4
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
|`components/logger_settings/`     |Versioned NVS settings service, validation against parser profiles, data source and interval rules |
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
|P1 data    |P1 interface output → GPIO44 (UART0 RX), always the same pin                     |
|P1 UART0 TX|GPIO43 (the fixed UART0 pin pair; the meter never needs our TX)                  |
|P1 GND     |P1 interface GND → board GND (common ground with the meter interface)            |
|P1 request |Externally enabled in this release (the meter's request line stays asserted)     |
|Console    |Native USB serial/JTAG (the `usbmodem` port used for flashing), never UART0      |

The receiver always uses hardware UART0 on the fixed P1 pins RX GPIO44 and
TX GPIO43; the P1 port selection is no longer configurable. The console runs
on the native USB serial/JTAG peripheral so GPIO43/44 are free for P1. The board component
reserves the display (2, 4, 5, 11, 12, 15) and, when inputs are used, the
encoder and buttons (1, 6, 16, 17); the encoder/button inputs are never
started, so those pins stay untouched. The former J5 RX pin selection is
retired: P1 always uses the fixed UART0 pin pair 43/44.

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
WebSocket support. The SPA opens with the **Telegrams** view. Embedded
assets are served with `Cache-Control: no-store` so a browser reload never
runs a stale `app.js` after a firmware update. The web server task runs on
an 8 KB stack: WebSocket command handling builds the telegram detail JSON
(string builders plus `vsnprintf`) on that stack and the 4 KB default
overflows, which previously killed the server and blanked the detail panes.

The interface uses an iOS/macOS look and feel (system fonts, translucent
header, grouped cards, an iOS blue accent, automatic light/dark mode). The
tab bar is an iOS-style segmented control built by `app.js` from the `VIEWS`
registry: adding a feature button takes one registry entry in `app.js`, a
matching `<section>` in `index.html`, and an answer to the view's command on
the device.

- **Settings**: prefilled with the configuration currently stored in NVS and
  refilled after every save, so the form always shows the settings in force.
  A *Data source* toggle (iOS-style switch) switches between **Live data**
  (the fixed P1 input on UART0, GPIO43/44) and **Test data** (the internal
  generator); an **Interval** field in seconds sets how often a telegram is
  read: only one telegram is accepted per interval. The interval is
  validated by clamping — a value below 2 becomes 2 and a value above 600
  becomes 600 — so a save never fails over the interval alone. Country, protocol
  profile (human-readable label, stable ID persisted, kept separate from the
  version reported by the meter), and M-Bus channels 1–4 with Auto, Disabled
  or an explicit device type. All dropdowns are filled from the valid
  choices the device reports. Save validates and applies the complete
  configuration as one operation and stores it in NVS; on any failure the
  active configuration is preserved and the reason is shown.
- **Telegrams**: a read-only **scrolling log window** over the retained
  records, newest on top. Rows are plain lines and carry no click selection.
  Each row shows the sequence number and the **meter time of OBIS
  `0-0:1.0.0`** — the `YYMMDDHHMMSSW` stamp (for example `260208173738S`,
  `2026-02-08T17:37:38`) shown as `17:37:38`; `--:--:--` when the frame
  publishes no usable timestamp. The detail pane fills once automatically
  and then stays frozen on that record — new telegrams refresh the log rows
  only, so the decoded summary and the raw telegram never blink away. The
  **Show Last** button always (re)shows the last telegram that was read in. Each record shows sequence, meter time, receive
  time (uptime and age: wall clock time is unavailable in this release),
  active profile, reported meter version, validation status, the decoded
  summary (parser-supplied values, units and timestamps; disabled channels
  omitted; a channel without a reading shows `(unavailable)` while a
  measured zero stays visible) and the raw frame shown as plain text with
  original line endings and trailer.

On-device TFT debug lines show the active browser screen, the last accepted
telegram, and the Wi-Fi portal/connected state.

### API

|Method |Path                        |Purpose                                                       |
|-------|----------------------------|--------------------------------------------------------------|
|WS     |`/ws`                       |SPA transport: status/telegram pushes + `cmd=config\|save\|status\|telegrams\|telegram\|raw\|view`, raw bytes as binary frame |
|GET    |`/api/config`               |NVS settings + active state + valid choices (data sources, interval bounds, countries, profiles) |
|POST   |`/api/config`               |Save form-urlencoded settings incl. `rxGpio` and `interval` (interval clamped to 2..600; one atomic operation) |
|GET    |`/api/status`               |Reception state, counters, last diagnostic, uptime            |
|GET    |`/api/telegrams`            |Metadata of retained records (incl. `meterTimeText` from OBIS `0-0:1.0.0`), newest first |
|GET    |`/api/telegram/<seq>`       |Decoded detail of one record                                  |
|GET    |`/api/telegram/<seq>/raw`   |Exact accepted frame bytes as `text/plain`                    |
|POST   |`/api/view`                 |Reports the active browser screen name to the device display  |

### Test data mode

Selecting **Test data** on the data-source toggle (RX GPIO `-1`) in the
Settings view starts an internal generator instead of the UART receiver. It
feeds complete, CRC-correct synthetic telegrams through the same parser and
event path once per configured interval (2–600 s), randomising the timestamps and the
measurement values (power, energy totals, tariff, voltages, currents and,
once an M-Bus channel is declared as an expected device type, its reading).
A (re)started generator reads at least one complete telegram right away —
after an interval change or any reconfiguration the view never waits a whole
interval for the first reading.
The frames follow the shapes of the verified parser fixtures, so Dutch and
Belgian profiles both decode. The status strip then shows
*test data (no UART)*. Both data-source selections (live on GPIO44, test on
`-1`) and the interval bounds are validated by the settings service for
every save path.

### UART0 TX forwarding

Every accepted telegram is also forwarded out on the UART0 TX pin (GPIO43)
as the exact accepted frame bytes. At most one forwarding burst happens per
five seconds — or per interval when the interval is larger — and every
burst sends the frame **twice** back to back. The burst runs in a dedicated
low-priority receiver task, so blocking writes never stall reception. Test
mode installs its own TX driver for this (it opens no receive UART); the
live path forwards through the driver owned by the parserDSMR adapter.

After a reconfiguration IDF may log `uart: GPIO 43/44 is not usable, maybe
used by others` once per driver (re)install. This is a cosmetic bookkeeping
quirk of ESP-IDF 6.0.2: `uart_driver_delete()` keeps its revoked-pin mask in
a 32-bit variable, so reservations of GPIOs ≥ 32 are never revoked and the
next install reports them busy. The pin routing itself is applied normally
and forwarding works; the warning can be ignored.

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
2. Open the SPA, set the data-source slider to **Live data**, choose an
   interval (2–600 s), country and profile, then Save. The console shows
   `receiving on UART 0 at ... baud, profile ...` and the status strip moves
   from *awaiting configuration* to *awaiting data* to *accepted telegram*.
3. With a conditioned P1 signal on GPIO44, one telegram per configured
   interval is read into the Telegrams view; console and reception operate
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
