# lastDSMRlogger

Buildnumber: 34
Last update-date: 2026-11-21

ESP-IDF application for the ESP32-S3 on the TFT LCD Display EC11 Piggyback
Board v3. It reads P1 telegrams from a smart meter through parserDSMR and
shows recent telegrams in a single-page web application with five views:
Settings, Telegrams, Phase Load (a web replica of the on-device power
panel), Show RingFile and Hours Graph. Configuration is stored in NVS and survives a power
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
|ring_file_store           |ESP Component Registry `mrwheel/ring_file_store`                                                            |`^0.1.3` |
|littlefs                  |ESP Component Registry `joltwallet/littlefs`                                                                |`^1.23.1` |
|smb_server                |ESP Component Registry `mrwheel/smb_server`                                                                 |`^0.3.0` (GPL-3.0, guest-only SMB2/SMB3 file server) |

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
  choices the device reports. An **MQTT** group enters the broker IP/URL,
  broker port, user, password, top topic and broadcast interval
  (0 = no broadcast); a **Network** group enters the hostname and the fixed
  IP (only the last part — empty or 0 means plain DHCP). Save validates and
  applies the complete configuration as one operation and stores it in NVS;
  on any failure the active configuration is preserved and the reason is
  shown.
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
- **Realtime**: a web replica of the on-device power panel, opening with a
  blue name bar that prints **RealtimePower** left aligned and the
  telegram counter right aligned. One centre-zero bar per phase plus
  **Totaal** — consumption red to the left of centre, feed-in green to the
  right — on the very same scale ladder the display uses
  (100/250/500/1000/1700/2400/3100/3800 W, the device picks the step and
  sends it), each row carrying its current and voltage chips and its
  signed watt value centred above the bar with animated fills. Below
  the bars a vertical-bar history follows the total power across the last
  90 accepted measurements kept by the device (one sample per accepted
  telegram, so the history survives page reloads, reconnects and view
  switches): one centre-zero bar per telegram with a brighter cap at the
  value end, drawn over very thin scale division lines every quarter step
  and a zero line. The view refreshes
  at least every five seconds, and every read interval when the interval
  is shorter: the device pushes the newest power frame the moment a
  telegram is accepted and the SPA polls `cmd=power` on its own timer as
  well. `GET /api/power` serves the same JSON without WebSocket support.
- **Show RingFile**: the stored records of one on-device meter ring (hours,
  days or months; a radio group picks the ring, a Refresh button re-requests
  it), newest period first: one table row per stored period with the five
  meter counters (kWh and m³) and the three fictional tariff amounts (EUR).
  `cmd=ringfile&ring=<period>` on the WebSocket, `GET /api/ringfile` with
  the optional `?ring=` query serve the same JSON.
- **Hours Graph**: the hourly usage overview of the hours ring, styled after
  the `assets/hoursGraph.jpeg` mockup: two stacked panels on one shared hour
  axis (oldest hour left, newest right). The upper panel carries grouped
  bars per hour for the used (red) and returned (green) kWh with a centred
  legend; the lower panel a blue line of the gas m³ per hour with its own
  legend. Each panel has grid lines with y labels, a unit label and rotated
  hour labels; hovering a slot pops up the hour with its three amounts. The
  device turns the retained cumulative counters into per-hour differences
  (`cmd=hoursgraph`, or `GET /api/hoursgraph`): the oldest record has no
  predecessor and is left out, a counter reset is clamped to zero, an hour
  that does not directly follow its retained predecessor (imported history
  ending in the past, hours aged out of the ring) is left out so the gap
  shows as a hole instead of one giant bar, and a Refresh button
  re-requests the series (switching to the view requests it too).

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
3800 → 3100 → 2400 → 1700 → 1000 → 500 → 250 → 100 W that fits it (a frame above
3800 W fills the bar at the 3800 W step). The panel layout adapts to the
orientation reported by the board component (320×240 or 240×320).

The board inputs are driven through the component's event queue (AUX button
and encoder). A **short AUX press** from the power panel opens the
per-phase screen: the net watts of the selected object as a large centred
number with a smaller `W` unit next to it, the voltage left and the
current right in smaller digits on one row below it, and a centre-zero
watt bar on the same ladder step the graphics panel picked; the **encoder**
cycles Fase 1 → Fase 2 → Fase 3 → Totaal (and back
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

Only a telegram that was read in and found faulty raises the rejected
count: a crc mismatch, a syntax error, an oversized frame. Nothing else
does: telegrams skipped because they arrive inside the read interval stay
out of every counter, and an abandoned partial frame (the parser's
inter-byte or frame timeout, the meter pausing between bursts) is not an
error — the logger simply awaits the next complete telegram. Data loss has
its own counter and an M-Bus type mismatch annotates an accepted telegram,
so neither adds to the rejected total. Because older firmware counted every
diagnostic, the persisted rejected total is rebased to zero once after this
firmware boots.

### API

|Method |Path                        |Purpose                                                       |
|-------|----------------------------|--------------------------------------------------------------|
|WS     |`/ws`                       |SPA transport: status/telegram/power pushes + `cmd=config\|save\|status\|telegrams\|telegram\|raw\|power\|ringfile\|hoursgraph\|view`, raw bytes as binary frame |
|GET    |`/api/config`               |NVS settings + active state + valid choices (data sources, interval bounds, countries, profiles) |
|POST   |`/api/config`               |Save form-urlencoded settings incl. `rxGpio`, `rxInverted` (0/1) and `interval` (interval clamped to 2..600; one atomic operation) |
|GET    |`/api/status`               |Reception state, counters, last diagnostic, uptime            |
|GET    |`/api/telegrams`            |Metadata of retained records (incl. `meterTimeText` from OBIS `0-0:1.0.0`), newest first |
|GET    |`/api/telegram/<seq>`       |Decoded detail of one record                                  |
|GET    |`/api/telegram/<seq>/raw`   |Exact accepted frame bytes as `text/plain`                    |
|GET    |`/api/power`                |Realtime power frame of the newest accepted telegram (phases and total watts/amps/volts, meter time, scale ladder step, history of the newest 90 accepted total watts) |
|GET    |`/api/ringfile`             |Records of one meter ring (`?ring=hours\|days\|months`, hours by default), newest first |
|GET    |`/api/hoursgraph`           |Per-hour used/returned kWh and gas m³ of the hours ring (differences of consecutive counters), oldest first |
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

### Telnet console and the per-channel control slots

Together with the SPA the device opens a telnet console on port 23
(`telnet <device-ip> 23` from any machine on the local network). Exactly one
operator per channel may control the logger: the SPA's WebSocket session and
one telnet session each hold their own control slot, so a browser page and a
telnet session run side by side. Ordinary page and API requests are not
operators.

- A telnet client connecting while a browser page is open never displaces
  the page: both stay active and the page keeps receiving its pushes.
- A second telnet client displaces the first: the first session receives
  `*** The telnet console was taken over by another telnet client.` and is
  closed. The browser page is not disturbed.
- A second browser page displaces the first: the evicted page shows a popup
  that control was taken over by another browser page, with a **Take back
  control** button that reloads the page and claims the web slot back.
  While the popup is open the page does not auto-reconnect. The telnet
  session is not disturbed.

The console greets with the firmware version and the peer address and answers
these commands (read-only except `populate` and `ring2csv`, which write to
the LittleFS volume):

|Command          |Shows                                                            |
|-----------------|-----------------------------------------------------------------|
|`help`           |the command list                                                 |
|`status`         |reception state, counters, clock, Wi-Fi, control owner **and** the device state block (copyright, hostname, versions, memory, chip, flash) |
|`version`        |firmware and parserDSMR versions                                 |
|`telegrams`      |the telegrams retained in RAM                                    |
|`telegram <seq>` |decoded fields of one retained telegram                          |
|`raw <seq>`      |exact accepted bytes of one retained telegram                    |
|`power`          |the realtime power frame of the newest accepted telegram         |
|`showring <ring>`|stored records of one meter ring: `month`, `days` or `hours`       |
|`ring2csv <ring>`|writes one meter ring to the volume as `RING<ring>.csv` (see below) |
|`populate <ring> [mode]`|imports a LittleFS csv file into one meter ring: `hours`, `days` or `months`, mode `overwrite` or `update` (see below) |
|`store`          |LittleFS volume: files with sizes, capacity and free space       |
|`quit`           |close the session                                                |

Commands are case-insensitive and may be shortened to any leading part that
names exactly one command (`show` and `showri` both mean `showring`); a word
that matches nothing is unknown and one that matches several commands is
reported as ambiguous. `ringfile` stays available as the old synonym of
`showring`.

### SMB network share

Once the station has joined and the meter store has mounted the LittleFS
storage partition, the device also runs an SMB2/SMB3 file server on port 445
(mrwheel's `smb_server` component). It publishes the storage volume as the
read-only guest share `DSMRLOGGER`: the three ring files `hours.rfs`,
`days.rfs` and `months.rfs` can be inspected or copied from any machine on
the local network, while the logger's own records stay protected against
accidental modification.

- macOS: `smbutil view -G //guest@<device-ip>`, then
  `open smb://guest@<device-ip>/DSMRLOGGER` (or Finder → Connect to Server).
- Linux: `mount -t cifs //<device-ip>/DSMRLOGGER /mnt/dsmrlogger -o guest,ro`
- Windows: map `\\<device-ip>\DSMRLOGGER` in Explorer (any credentials work).

The server is guest-only and never signs or encrypts traffic: there are no
user accounts and no transport security, so only trusted local networks
should reach the device. When the storage volume could not be mounted the
share stays off and the console logs the reason.

The share stays read-only by design, so import files (next subsection) land
on the volume through the storage partition image, not through the share.

### MQTT publication

The **MQTT** group in the Settings view publishes the meter data to a broker
on the local network (the broker may be any IP, host name or full
`mqtt://`/`mqtts://` URI; an empty broker keeps publication off):

|Setting                |Effect                                                                                     |
|-----------------------|-------------------------------------------------------------------------------------------|
|Broker IP or URL       |the broker to connect to; empty disables MQTT                                              |
|Broker port            |used when the broker text carries no port (default 1883)                                   |
|User / Password        |broker credentials; both empty for an anonymous broker                                     |
|Top topic              |every broadcast goes to `<top topic>/telegram` (empty selects `lastdsmrlogger`)            |
|Broadcast interval (s) |how often the latest telegram is published; **0 = no broadcast**                           |

When publication is on, the device publishes the **most recently accepted
telegram** as one JSON document every interval seconds — and once right
after every broker connect, so a fresh subscriber sees the latest frame
without waiting. The document is `{"actual":{…}}` with one entry per
decoded field. The entry name is the field's display label when it has one
— an adjusted label wins over the built-in name of the active profile's
field table — and the **OBIS code** when the field carries no label. The
entry value is the parsed value as a JSON **number** when its text spells
a decimal number (so graphs, calculations and automations can consume it
directly) and as a JSON **string** in every other case (timestamps, device
ids, status words). Two fields sharing a label publish the second one
under its OBIS code so the object never holds a duplicate key:

```json
{"actual":{"1-0:1.8.1":1260.013,"electricity_imported_total":1260.013,
 "tarif_indicator":"1","0-0:1.0.0":"2026-11-21T17:37:38+0100"}}
```

Publication runs only while the station has joined: the client comes up at
every Wi-Fi connect (a broker lookup before LWIP is up would crash the
device, so it never starts on a disconnected station), goes down when the
link drops, retries on its own when the broker disappears, and reconfigures
live when the settings are saved.

The hostname and fixed IP in the **Network** group take effect at the same
save: the hostname is stored right away and announced to DHCP and mDNS, and
the fixed IP pins the last octet of the granted address (the network part
comes from the router's lease; empty or 0 returns to plain DHCP). Work that
touches the lease itself — re-requesting it for a new name, pinning or
withdrawing a fixed address — is performed by the Wi-Fi task within seconds
after the save replies, because it briefly leaves the station interface and
would otherwise abort the very browser request that saved it.

### Importing historical periods (`populate`)

The console command `populate {hours|days|months}` imports historical
periods from a semicolon separated csv file on the LittleFS volume into one
meter ring. After the ring selector the command asks for the input file,
relative to the volume root (typically `RINGmonths.csv`); an empty
line cancels.

The file starts with a header line. Its first column names the period key
(`YYMMDDHH`, `YYMMDD` or `YYMM`); the value columns are named by OBIS code
(with or without the `*255` method suffix) or, for the three tariff amounts
that have no code of their own, by the rubrics of the ring print (`t1`, `t2`
and `tg`):

|Column                             |Ring field                |Unit|
|-----------------------------------|--------------------------|----|
|`1-0:1.8.1`                        |energy delivered tariff 1 |kWh |
|`1-0:1.8.2`                        |energy delivered tariff 2 |kWh |
|`1-0:2.8.1`                        |energy returned tariff 1  |kWh |
|`1-0:2.8.2`                        |energy returned tariff 2  |kWh |
|`0-1:24.1.0` (or `0-1:24.2.1`/`.3`)|gas delivered             |m³  |
|`t1`                               |tariff 1 amount           |€   |
|`t2`                               |tariff 2 amount           |€   |
|`tg`                               |gas amount                |€   |

Unknown columns — including the `#` and the empty trailing columns
handwritten files may carry — are skipped. Every data line starts with the
period key (8, 6 or 4 digits) and carries its values in header order; **a
blank or missing value becomes 0**. Several lines of one period update the
same record: the last line wins.

The optional mode argument picks what happens to the records the ring
already holds:

|Mode (default)            |Effect                                                          |
|--------------------------|----------------------------------------------------------------|
|`merge`                   |keeps stored records and backfills every free slot of the retained window; periods older than the ring capacity (49 hours, 32 days, 24 months) are refused and reported |
|`overwrite`               |clears the ring just before the first valid data line, so the file alone names the stored periods and the whole file lands without window refusals |
|`update`                  |only revises periods the ring already stores; every key it does not hold is refused as not stored, nothing is ever added |

An import that never reaches a valid data line (missing file, bad header,
only rejected lines) leaves the ring untouched in all three modes.

```
logger> populate hours overwrite
importing into the hours ring (overwrite).
input file on LittleFS (e.g. RINGmonths.csv, empty line cancels): RINGhours.csv

49 line(s) read, 49 edit(s) written, 0 line(s) rejected, 0 refused
logger> showring hours
```

### Exporting a ring (`ring2csv`)

The console command `ring2csv {hours|days|months}` writes one meter ring
back to the LittleFS volume as a semicolon separated csv file named
`RING<ring>.csv` at the volume root: the header
`YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;0-1:24.1.0*255;t1;t2;tg`
followed by one line per retained record, oldest period first, the key as
eight `YYMMDDHH` digits, the five counters at three decimals and the three
tariff amounts at two. Every field the ring file holds is exported, so the
file is re-importable through `populate` unchanged, and the read-only
`DSMRLOGGER` SMB share exposes it for inspection or editing on any machine.

```
logger> ring2csv hours
wrote 49 record(s) to RINGhours.csv on the volume
logger> populate hours overwrite
importing into the hours ring (overwrite).
input file on LittleFS (e.g. RINGmonths.csv, empty line cancels): RINGhours.csv

49 line(s) read, 49 edit(s) written, 0 line(s) rejected, 0 refused
```

Input files land on the device through the storage partition image: drop
them into `storageImage/` — the folder the image is built from, and the
import csv files land at the volume root (the sample `RINGhours.csv`,
`RINGdays.csv` and `RINGmonths.csv` ship there) —
rebuild and write `build/storage.bin` to the device. macOS Finder leaves
`.DS_Store` files in every folder it browses; the image is built from a
staged copy of the folder that filters `.DS_Store` (and AppleDouble `._`
companions) out, so Finder junk never reaches the LittleFS volume.

```
python $IDF_PATH/components/partition_table/parttool.py \
  -p PORT write_partition --partition-name storage --input build/storage.bin
```

Note: listing a nearly full ring — `showring` or the web pages — makes the
`ring_file_store` iterator inspect every slot on every step, which costs a
very large number of flash reads once a ring is full; a listing of the
49-hour ring can then run for minutes and the task watchdog will report the
busy task while it walks. The minute store keeps running: its upsert simply
waits for the listing to release the ring. The cost sits in the component's
iterator (no slot cache: `rfsRankGreater` re-reads every slot per candidate);
a maintainer-side fix would make full-ring listings fast again.

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
   date/time. A short AUX press opens the per-phase power screen with the
   large centred watts, the volts/amperes row and the centre-zero bar (the
   encoder cycles Fase 1 → Fase 2 → Fase 3 → Totaal), a
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
