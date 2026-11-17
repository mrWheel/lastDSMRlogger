# lastDSMRlogger

Buildnumber: 16
Last update-date: 2026-11-16

ESP-IDF application for the ESP32-S3 on the TFT LCD Display EC11 Piggyback
Board v3. It reads P1 telegrams from a smart meter through parserDSMR and
shows recent telegrams in a single-page web application with three views:
Settings, Telegrams and Realtime (a web replica of the on-device power
panel). Configuration is stored in NVS and survives a power
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
|parserDSMR                |`https://github.com/mrWheel/parserDSMR` checkout at `../parserDSMR`                                         |revision `686a3aeefb5c4b107ce9884023fab4014b184b31` (v0.9.1, includes the raw-frame API and `dsmrParserVersion()`; `PROG_VERSION` now has internal linkage, so it no longer clashes with the application's own symbol) |
|esp32_s3_piggyback        |`https://github.com/mrWheel/esp32-s3-Piggyback`, component directory `components/esp32_s3_piggyback`       |commit `912fa25b86fdb62d8f1005ae57e227c5803db373` (v1.3.0), vendored under `components/` |
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
accepted frame including every original line ending and trailer byte), the
UART overflow/framing error reporting in the adapter and the corrected phase
tables are committed in the parser repository at the pinned revision. The
same revision exposes `dsmrParserVersion()`; the application logs that
version at boot and shows it on the display's boot debug lines.

The Dutch and Belgian phase tables publish the objects the meters really use
and that dsmr2Lib and the verified sample fixture confirm — per-phase
delivered kW on `1-0:21/41/61.7.0`, per-phase returned kW on
`1-0:22/42/62.7.0`, phase current on `1-0:31/51/71.7.0` and phase voltage on
`1-0:32/52/72.7.0`.

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
reserves the display (2, 4, 5, 11, 12, 15) and, for the panel views
described in section 6, the AUX button (1), the encoder (16/17) and the
unused encoder push button (6). The former J5 RX pin selection is
retired: P1 always uses the fixed UART0 pin pair 43/44.

RX polarity is a runtime setting in the SPA (Settings view → *P1 RX
polarity*): *Normal* reads the idle-high signal directly and *Inverted* flips
the receive line for interface boards that invert before the ESP32-S3. The
choice is stored with the configuration and applied on the next receiver
start; the console confirms it with `P1 RX on UART0 gpio 44, polarity
...`. If a live source stays *awaiting data* and the monitor only shows
`diagnostic: size limit` or `diagnostic: inter-byte timeout`, the bytes are
arriving but are unreadable — a wrong RX polarity (or a baud/frame mismatch)
is the usual cause, so try the other switch position. The former build-time
Kconfig polarity choice is retired.

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
  generator); a *P1 RX polarity* toggle (iOS-style switch) selects **Normal**
  or **Inverted** reading of the live signal (ignored by the test generator);
  a *P1 Tx polarity (Forwarding Data)* toggle (iOS-style switch) selects
  **Normal** or **Inverted** for both copies each forwarding burst sends out
  on the UART0 TX pin (see *UART0 TX forwarding*);
  an **Interval** field in seconds sets how often a telegram is
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
- **Realtime**: a web replica of the on-device power panel. One
  centre-zero bar per phase plus **Totaal** — consumption red to the left
  of centre, feed-in green to the right — on the very same scale ladder
  the display uses (500/1000/1700/2400/3100/3800 W, the device picks the
  step and sends it), each row carrying its current and voltage chips and
  its signed watt value with animated fills. Below the bars a sparkline
  follows the total power across the last 90 accepted telegrams of the
  page visit, with a half-scale gridline and zero line. The view refreshes
  at least every five seconds, and every read interval when the interval
  is shorter: the device pushes the newest power frame the moment a
  telegram is accepted and the SPA polls `cmd=power` on its own timer as
  well. `GET /api/power` serves the same JSON without WebSocket support.

On-device TFT debug lines show the active browser screen, the last accepted
telegram, and the Wi-Fi portal/connected state. With the first accepted
telegram the screen is cleared and fully taken over by the **power panel**
(no debug lines remain). Every screen opens with a white **name bar** at
the top with its name left aligned in black text — `TELEGRAM nnn`,
`PER FASE` or `SYSTEMINFO` — and a sub band below it with optional right
aligned text (on the [TELEGRAM] screen the meter date and time of OBIS
`0-0:1.0.0`, `DD-MM-YYYY HH:MM` without seconds; on the [PER FASE] screen
the selected `Fase n` or `Totaal`). All panel text is rendered by the
logger itself: it stretches the component's 5×7 font into a per-text cell
size, so sizes like 9×14 or 7×11 are possible instead of only the two
integer font scales of the component. The [TELEGRAM] screen shows one row
per phase plus **Totaal**. Each row has a header band (phase current left
and phase voltage right larger than the centred phase label; the total row
shows the summed current and the average voltage) and a centre-zero watt
bar carrying its integer scale labels (`-3800`, `0`, `3800`) inside the
bar, followed by the watt value. Like the DSMRlogger32
power bars, the net power is *returned minus delivered*: consumption reads
negative and is drawn **red** to the left of centre, feed-in reads positive
and is drawn **green** to the right. The bar scale adapts to the size of
the largest absolute watt of the frame: the smallest step of
3800 → 3100 → 2400 → 1700 → 1000 → 500 W that fits it (a frame above
3800 W fills the bar at the 3800 W step). The panel layout adapts to the
orientation reported by the board component (320×240 or 240×320).

The board inputs are driven through the component's event queue (AUX button
and encoder). A **short AUX press** from the power panel opens a text
screen showing *Geleverd vermogen* and *Opgewekte vermogen* of the selected
object; the **encoder** cycles Fase 1 → Fase 2 → Fase 3 → Totaal (and back
when turning the other way). A **medium-long AUX press** replaces the bars
with a system information view: accepted and rejected telegram counts, IP
address, SSID, uptime, country/protocol and the P1 RX polarity (normal or
inverted). Both AUX screens carry a white bottom bar with `AUX: Terug` on
the left. A short AUX press returns to the graphics panel. New telegrams
keep refreshing the graphics panel only; an open AUX view stays on screen
until its own short press returns, and it shows the latest frame when
opened or reselected with the encoder.

Console diagnostics are deduplicated: an identical diagnostic text (for
example a repeating `M-Bus device type mismatch @ 0-1:24.1.0`) reaches the
console at most once per reception session — it shows again after a boot or
a settings change, which resets the deduplication. The reception counters
and the web status strip stay complete regardless.

### API

|Method |Path                        |Purpose                                                       |
|-------|----------------------------|--------------------------------------------------------------|
|WS     |`/ws`                       |SPA transport: status/telegram/power pushes + `cmd=config\|save\|status\|telegrams\|telegram\|raw\|power\|view`, raw bytes as binary frame |
|GET    |`/api/config`               |NVS settings + active state + valid choices (data sources, interval bounds, countries, profiles) |
|POST   |`/api/config`               |Save form-urlencoded settings incl. `rxGpio`, `rxInverted` (0/1) and `interval` (interval clamped to 2..600; one atomic operation) |
|GET    |`/api/status`               |Reception state, counters, last diagnostic, uptime            |
|GET    |`/api/telegrams`            |Metadata of retained records (incl. `meterTimeText` from OBIS `0-0:1.0.0`), newest first |
|GET    |`/api/telegram/<seq>`       |Decoded detail of one record                                  |
|GET    |`/api/telegram/<seq>/raw`   |Exact accepted frame bytes as `text/plain`                    |
|GET    |`/api/power`                |Realtime power frame of the newest accepted telegram (phases and total watts/amps/volts, meter time, scale ladder step) |
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
as the exact accepted frame bytes. Every burst sends the frame **twice**
back to back at 115200 baud and runs in a dedicated low-priority receiver
task, so the blocking writes never stall reception. Both copies go out
straight from the retained history slot of the accepted telegram (sized for
`DSMR_MAX_TELEGRAM_BYTES`, 12 KB — the parser's frame cap): reception is
suspended for the whole burst, so the slot cannot be evicted while its
bytes are on the wire and the forwarded bytes are exactly the bytes that
were accepted. The per-burst UART0 driver sizes its TX ring to the frame
plus slack, keeping only a few kilobytes of the internal heap busy while a
burst runs.

Normal reception is suspended for the duration of a burst and everything
captured while it runs is discarded: the live path stops the parserDSMR
adapter receive task — the only task allowed to feed the parser — installs
its own UART0 driver to drive TX, deletes that driver again once the line is
idle and restarts the adapter. Before resuming, the hardware RX FIFO is
flushed and the parser's partial frame is dropped, so stale bytes can never
reach the parser, the logger or a new forwarding trigger. If the meter is
mid-telegram when reception resumes, the parser's start-of-telegram
detection skips the overlapping frame whole and the next complete telegram
is processed normally. Test mode opens no receive UART at all; frames the
generator produces during a burst are discarded the same way.

Polarity follows the two SPA toggles. During a burst both copies leave the
pin at the selected *P1 Tx polarity*, and the RXD inversion of the current
*P1 RX polarity* setting is kept in the mask while it is active. Once TX is
idle again the TX line returns to the polarity of the current P1 Rx
polarity setting (the hardware update is skipped when it is already
correct), and on resume RX is restored to its own setting. Each copy is
confirmed on the wire with `uart_wait_tx_done()` (5 s timeout; a
maximum-sized copy takes ≈1.5 s); a partial write or timeout skips the
remaining copies and reception is still restored safely. A settings change
that arrives mid-burst waits for the burst to finish and applies to the
next operation, so both copies of one telegram always share one polarity.
The console runs on USB-Serial-JTAG and never touches UART0. The console
reports every burst with `forwarded <n> bytes x 2 on UART0 TX, tx polarity
..., sent in <ms> ms, free heap <n>`.

At most one forwarding burst happens per five seconds — or per interval when
the interval is larger — so bursts stay paced at the 2 and 4 second
intervals, while above four seconds every accepted telegram is forwarded.

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
|Telegram history                        |4 records × (12 KB raw + up to 96 field snapshots) ≈ 100 KB     |
|Per-burst UART0 TX driver               |TX ring = frame length + 512 B, internal heap, only during a burst |
|Requirement                             |accommodates at least two maximum-sized telegram records        |

The module carries 8 MB of octal PSRAM (@80 MHz, verified at boot:
`Found 8MB PSRAM device`): the parser workspace, the staging record and the
telegram history are allocated with `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
in `loggerReceiverInit()` and fall back to internal RAM when PSRAM is
absent. Only task code touches those buffers — never an ISR or a DMA
descriptor — so the radios, task stacks and drivers keep the internal DRAM
heap (~200 KB free at rest) entirely to themselves.

The oldest record is evicted first. Rejected candidates only update
diagnostics and counters; the last accepted record stays intact. HTTP
handlers copy consistent snapshots and never hold the receive-side lock while
writing to the network, so slow or disconnected browser clients have no
effect on UART reception.

## 8. Hardware verification

1. `idf.py -p PORT flash monitor` shows the board debug lines after boot.
2. Open the SPA, set the data-source slider to **Live data**, choose the P1
   RX polarity (Normal/Inverted) matching the interface board, choose the
   P1 Tx polarity (Normal/Inverted) for the forwarded copies, choose an
   interval (2–600 s), country and profile, then Save. The console shows
   `P1 RX on UART0 gpio 44, polarity ..., tx idles at ...` plus
   `receiving on UART 0 at ... baud, profile ...` and the status strip moves
   from *awaiting configuration* to *awaiting data* to *accepted telegram*.
   Every accepted telegram then also produces a
   `forwarded <n> bytes x 2 on UART0 TX, tx polarity ..., sent in <ms> ms`
   line. (After updating from settings version 4 the stored configuration is
   rejected once: open Settings and Save to store the version 5 blob.)
3. With a conditioned P1 signal on GPIO44, one telegram per configured
   interval is read into the Telegrams view; console and reception operate
   together. The TFT switches to the power panel with the first accepted
   telegram and shows the phase and total watt bars with the meter
   date/time. A short AUX press opens the delivered/returned power text
   screen (the encoder cycles Fase 1 → Fase 2 → Fase 3 → Totaal), a
   medium-long AUX press shows the system information view, and a short
   press returns to the bars.
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
