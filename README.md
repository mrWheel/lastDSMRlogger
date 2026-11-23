# lastDSMRlogger
<!--
fileVersion: [54] - 23-11-2026 - 22:35
-->
ESP-IDF application for the ESP32-S3 on the TFT LCD Display EC11 Piggyback
Board v3. It reads P1 telegrams from a smart meter through parserDSMR and
shows recent telegrams in a single-page web application with nine views:
Settings, Telegrams, Phase Load (a web replica of the on-device power
panel), Show RingFile, Hours Graph, Days Graph, Months Graph, Actual
Graph and Tariffs. Configuration is stored in NVS and survives a power cycle; the recent
telegram history lives in RAM.

## 1. Project layout

|Path                              |Role                                                                                   |
|----------------------------------|---------------------------------------------------------------------------------------|
|`main/`                           |Application entry, NVS init, Wi-Fi provisioner task, P1 interface Kconfig              |
|`components/logger_settings/`     |Versioned NVS settings service, validation against parser profiles, data source and interval rules |
|`components/logger_receiver/`     |Receive task ownership of UART adapter + parser, diagnostics, RAM telegram history     |
|`components/logger_web/`          |esp_http_server: SPA assets and JSON API                                               |
|`components/logger_tariffs/`      |Dynamic electricity tariffs: JSON validation, atomic apply, LittleFS persistence, aggregation |
|`components/logger_board/`        |On-device TFT debug lines through the esp32_s3_piggyback component                     |
|`components/esp32_s3_piggyback/`  |Vendored board component from mrWheel (pinned, see section 3)                          |
|`managed_components/mrwheel__parserdsmr/` |Parser dependency from the ESP Component Registry (see section 3)                |

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
|parserDSMR                |ESP Component Registry `mrwheel/parserdsmr`, published from `https://github.com/mrWheel/parserDSMR`           |`^0.9.2` — added with `idf.py add-dependency "mrwheel/parserDSMR^0.9.2"` (registry names are lowercase; the `REQUIRES` name and the status console report the lowercase `parserdsmr`) |
|esp32_s3_piggyback        |`https://github.com/mrWheel/esp32-s3-Piggyback`, component directory `components/esp32_s3_piggyback`       |commit `912fa25b86fdb62d8f1005ae57e227c5803db373` (v1.3.0), vendored under `components/` |
|esp-idf-wifi-provisioner  |ESP Component Registry `michmich/esp-idf-wifi-provisioner`                                                  |version `0.4.0` (upstream commit `2c7aaa6aa0e319a429186ee470e137b8579f632f`) |
|ring_file_store           |ESP Component Registry `mrwheel/ring_file_store`                                                            |`^0.1.3` |
|littlefs                  |ESP Component Registry `joltwallet/littlefs`                                                                |`^1.23.1` |
|smb_server                |ESP Component Registry `mrwheel/smb_server`                                                                 |`^0.3.0` (GPL-3.0, guest-only SMB2/SMB3 file server) |
|cjson                     |ESP Component Registry `espressif/cjson`                                                                    |`^1.7.19` (tariff document parsing) |
|mdns                      |ESP Component Registry `espressif/mdns`                                                                     |`^1.15.1` (hostname and `_http._tcp` discovery on the local network) |

parserDSMR is consumed as the ESP Component Registry release
`mrwheel/parserdsmr`, resolved into `managed_components/` by the component
manager at configure time. Add or bump it with:

```
idf.py add-dependency "mrwheel/parserDSMR^<version>"
```

The registry component name is lowercase: `idf.py add-dependency`
normalises `parserDSMR` to `parserdsmr`, `REQUIRES` must name the component
`parserdsmr`, and the status console reports it under that name. No sibling
checkout is involved any more.

The raw byte view on `dsmrTelegramResult` (`rawBytes`/`rawLength`, the exact
accepted frame including every original line ending and trailer byte), the
UART overflow/framing error reporting in the adapter and the corrected phase
tables are part of the 0.9.2 release. The same release exposes
`dsmrParserVersion()`; the application logs that version at boot and shows
it on the display's boot debug lines.

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
The HTTP API is versioned: the only surface is `/api/v3/` (see API below),
there are no unversioned `/api/` paths, so a future v4 can introduce its
own syntax without an unversioned layer beside it. The SPA opens with the
**Telegrams** view. Embedded
assets are served with `Cache-Control: no-store` so a browser reload never
runs a stale `app.js` after a firmware update. The web server task runs on
an 8 KB stack: WebSocket command handling builds the telegram detail JSON
(string builders plus `vsnprintf`) on that stack and the 4 KB default
overflows, which previously killed the server and blanked the detail panes.

The interface uses an iOS/macOS look and feel (system fonts, translucent
header, grouped cards, an iOS blue accent, automatic light/dark mode). The
tab bar is an iOS-style segmented control built by `app.js` from the `VIEWS`
registry: adding a feature button takes one registry entry in `app.js`
(including its `group`), a matching `<section>` in `index.html`, and an
answer to the view's command on the device. The registry's `group` property
assigns every view to one of the two bars: the **logger bar** shows the
feature views (Telegrams, Phase Load, Hours, Days, Months, Actual) and ends
with a **System** button, the **system bar** shows the settings views
(Settings, Show RingFile, Tariffs, API) and ends with a **Back** button.
Pressing System swaps to the system bar and opens its first view; pressing
Back swaps to the logger bar and restores the logger screen last shown. Each
bar is named by a macOS-style leading caption left of the pills: **Logger:**
before the feature buttons and **System:** before the settings buttons. The
Phase Load view is the default screen at every start and reload of the page.

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
  broker port, user, password, top topic, **Client ID** (the device segment
  of the tariff topic and the id the broker sees; empty leaves the segment
  out) and **Sub topic** of the tariff topic (empty selects `newTariffs`)
  and the broadcast interval (the stored password is never shown by the
  device: the password field starts empty
  and a save only sends it when the operator typed a new one)
  (0 = no broadcast); a **Network** group enters the hostname and the fixed
  IP (only the last part — empty or 0 means plain DHCP); a **Meter store**
  group sets the **rfs Sync Interval (minutes)** (1–60, default 10) — the
  store task commits its staged ring records at least this often, and a
  span behind the field shows a rough LittleFS wear estimate for the
  entered value (device-published volume block count, estimated block-sized
  writes per synchronisation and flash cycle rating, uniform wear assumed);
  a **Tariffs** group carries the **Dynamic Tariffs** switch, the manual
  prices **Tariff 1 Delivered (kWh)**, **Tariff 2 Delivered (kWh)**,
  **Tariff 1 Received (kWh)**, **Tariff 2 Received (kWh)** and **Tariff Gas
  Delivered (m3)** in EUR (see *Dynamic electricity tariffs*) and a fixed
  monthly **Network &
  transport costs (month)** amount in EUR — every ring record stores this
  amount prorated to its period (the days ring a thirtieth of the month,
  the hours ring a thirtieth of a day) and the *Netw.* column of the
  Months Graph *Costs* presentation prints the figure stored with the
  month — the Hours and Days tables' *Total (€)* column adds the same
  stored figure (a more advanced treatment of the network costs may
  follow later). The
  manual prices follow the direction of the energy: *Delivered* is what the
  grid delivers to the home (the used energy, a cost), *Received* is what
  the home delivers back to the grid (returned energy, a credit). The
  meter's tariff indicator (OBIS `0-0:96.14.0`) names which of the two
  tariff prices of a direction values a reading; a meter that names no
  active tariff prices like tariff 1. Gas is only ever delivered to the
  home, so it needs no Received price. Save validates and
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
  100 accepted measurements kept by the device (one sample per accepted
  telegram, so the history survives page reloads, reconnects and view
  switches): one centre-zero bar per telegram with a brighter cap at the
  value end, drawn over very thin scale division lines every quarter step
  and a zero line. The view refreshes
  at least every five seconds, and every read interval when the interval
  is shorter: the device pushes the newest power frame the moment a
  telegram is accepted and the SPA polls `cmd=power` on its own timer as
  well. `GET /api/v3/power` serves the same JSON without WebSocket support.
- **Show RingFile**: the stored records of one on-device meter ring (hours,
  days or months; a radio group picks the ring, a Refresh button re-requests
  it), newest period first: one table row per stored period with the five
  meter counters (kWh and m³) and the three tariff amounts (EUR) per
  direction of the energy: *Import* (what the used energy costs), *Export*
  (what the delivered-back energy earns) and *Gas* — fictional billing cents
  while no tariff calculator runs.
  `cmd=ringfile&ring=<period>` on the WebSocket, `GET /api/v3/ringfile` with
  the optional `?ring=` query serve the same JSON.
- **Hours Graph / Days Graph / Months Graph**: the usage overview of one
  meter ring, styled after the `assets/hoursGraph.png`,
  `assets/daysGraph.png` and `assets/monthsGraph.png` mockups: two stacked
  panels on one shared period axis (oldest period left, newest right). The
  upper panel carries bars for the used (red) and returned (green) kWh on a
  zero line with a centred legend; the lower panel a blue line of the gas
  m³ per period with its own legend. On the months view every period is
  paired with the same month of the year before — YY-MM against
  (YY-1)-MM, so each pair compares the month against last year (like the
  monthsGraph mockup): the used pair — red this period, orange previous —
  stands side by side above the zero line, the returned pair — dark green
  this period, light green previous — below it, and the gas panel draws
  both lines (blue this period, light blue previous); a month whose
  year-ago counterpart the ring no longer retains draws its pair alone.
  Each panel has grid lines with y labels, a unit label and rotated period
  labels (hours `19-11 16:00`, days `Thursday 22-10`, months `November`,
  January carrying the year); the label band is sized to the slanted text,
  so nothing is cut off and no dead space survives between the panels.
  Hovering pops up separate chips: one for the used kWh and one for the
  returned kWh riding along their bar on the energy panel, one for the gas
  m³ over the gas panel — except on the months view, where the energy
  panel shows exactly one chip: the one for the bar the cursor sits on
  (this period or previous year, used or returned); the
  state line names the API the records come from
  (`/api/v3/ringfile?ring=hours|days|months`). The device turns the retained
  cumulative counters into per-period differences
  (`cmd=graph&ring=<period>`, or `GET /api/v3/graph?ring=<period>`;
  `cmd=hoursgraph` remains as the hours spelling on the WebSocket):
  the oldest record has no predecessor and is left out, a counter reset is
  clamped to zero, a period that does not directly follow its retained
  predecessor (imported history ending in the past, periods aged out of the
  ring) is left out so the gap shows as a hole instead of one giant bar,
  and a Refresh button re-requests the series (switching to the view
  requests it too). Note the P1 telegram publishes energy counters only as
  totals — the days and months graphs therefore aggregate all three phases
  (the per-phase mockup bars are not derivable from stored data).
  A *Presentation:* radio group in each of the three views switches the
  same cached frame between presentations without asking the device again:
  **Graph** draws the panels described above, **Table** lists the same
  per-period amounts newest first. The Hours and Days tables show one row
  per retained period (period, used kWh, returned kWh, gas m³) plus the
  row's own money columns *Used (€)*, *Returned (€)*, *Gas (€)* and
  *Total (€)* — every figure stands on its own row, these tables never
  carry a summed row. The months
  Table is the two-year comparison of the old DSMRlogger32 history page
  (`assets/oldMonthsTabel.png`): the drawn twelve months, newest on top,
  each row showing used, returned and gas twice — this month and the same
  month one year back (a year the ring no longer retains shows dashes) —
  closing with a **Total** row that sums the drawn rows column by column
  and a **Balance** row that nets used against returned per year. The
  months view has a third option **Costs** (`assets/oldMonthsCosts.png`):
  per drawn month the *Electra* amount (the used kWh times the import
  tariff the ring stores, what the used energy costs), the *Export* amount
  (the returned kWh times the export tariff, the positive compensation the
  delivered-back energy earns), the *Gas* amount (the gas m³ times the
  gas tariff), *Netw.* (the network and transport amount stored with the
  record: the Settings month amount as saved when the record was written)
  and their
  *Total* (Electra minus Export plus Gas plus Netw. — the very rule the
  Hours and Days *Total (€)* columns share), each month paired
  with the same month one year back, and an
  **Advance** row printing each half's advance payment: that column's
  total sum divided by twelve (the old page's *Voorschot Bedrag*).
- **Actual Graph**: the realtime per-phase power overview, styled after the
  `assets/actualGraph.png` mockup. The device keeps an internal rolling
  table of the newest ~100 accepted telegrams in RAM: per sample the
  unsigned delivered and returned watts of each phase (OBIS
  `1-0:21/41/61.7.0` and `1-0:22/42/62.7.0`), the gas m³/h rate of the last
  observed gas-counter increase (held between gas arrivals) and the sample
  time. `cmd=actualgraph` (or `GET /api/v3/actualgraph`) hands the table over
  oldest first. The upper panel stacks the used watts upwards (light red
  L1, red L2, dark red L3) and the returned watts downwards (light green
  L1, green L2, dark green L3), every phase keeping its own colour and its
  own slab with a white hairline on each phase boundary; six checkboxes
  switch each phase series on and off and the watt scale follows the
  visible series only. The lower panel carries the blue gas line. The
  rotated time labels hang under the plotted bars, not under the empty
  left of the plot. The newest sample always sits at the
  right edge of the 100-slot plot: the graph fills from the right and
  shifts one slot left with every accepted telegram once the table is
  full. The view polls on the Phase Load timer; hovering a slot pops up a
  chip with the sample time, the per-phase kW and the gas m³/h.
- **Tariffs**: the stored dynamic tariff schedule as a plain table — one
  row per interval that is **in use now or starts in the future**, oldest
  first, with the columns *Date/Time from*, *Date/Time to*, *Tariff
  Delivered*, *Tariff Returned* and *Tariff Gas* (prices per kWh and per m³ in
  the document currency; an unknown price shows a dash and the row
  covering the present moment is highlighted). Expired intervals are not
  shown. The table reads `GET /api/v3/tariffs?from=&to=&resolution=raw`
  over plain HTTP when the view opens and on **Refresh** (like the API
  view the tariff table needs no WebSocket command), so it always shows
  what the tariff store holds right now.

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

### API — standard data calls under /api/v3/

Everything the telnet console and the SPA can request and show is
retrievable as a plain GET under `/api/v3/`, each endpoint under a short
descriptive English name. The JSON frames answer exactly what the console
prints and what the SPA receives (they carry the same `type` tag); the
index at `/api/v3` lists the surface live from the device. The two SPA
actions (save settings, report the viewed screen) are POSTs in the same
namespace; a populate import stays on the console. There is no unversioned
`/api/` surface: all endpoints are versioned so a future v4 can define its
own syntax.

The MQTT password never leaves the device: no settings frame (HTTP or
WebSocket) carries `mqttPassword`, the SPA's password field starts empty,
and a save that omits the `mqttPassword` field keeps the stored password
(a present empty value clears it explicitly).

|Method |Path                                    |Purpose                                                       |
|-------|----------------------------------------|--------------------------------------------------------------|
|WS     |`/ws`                                   |SPA transport: status/telegram/power pushes + `cmd=config\|save\|status\|telegrams\|telegram\|raw\|power\|ringfile\|graph\|actualgraph\|hoursgraph\|view`, raw bytes as binary frame |
|GET    |`/api/v3`                               |Index: lists all v3 endpoints with descriptions               |
|GET    |`/api/v3/config`                        |NVS settings + active state + valid choices (data sources, interval bounds, countries, profiles); never includes `mqttPassword` |
|POST   |`/api/v3/config`                        |Save form-urlencoded settings incl. `rxGpio`, `rxInverted` (0/1) and `interval` (interval clamped to 2..600; one atomic operation; omitted `mqttPassword` keeps the stored password) |
|GET    |`/api/v3/status`                        |Reception state, counters, last diagnostic, uptime **plus** the console's extra facts: `wifiSsid`, `wifiIp`, `wifiConnected`, `controlWebClaimed`, `controlTelnetClaimed` |
|GET    |`/api/v3/version`                       |Console version line as JSON: firmware, parserDSMR, ESP-IDF and gcc versions, compile stamp **and** the managed component versions |
|GET    |`/api/v3/device`                        |Console state block as JSON: copyright, hostname, versions, chip, heap, PSRAM, image and flash facts |
|GET    |`/api/v3/telegrams`                     |Metadata of retained records (incl. `meterTimeText` from OBIS `0-0:1.0.0`), newest first |
|GET    |`/api/v3/telegram`                      |Decoded fields of the most recently accepted telegram         |
|GET    |`/api/v3/telegram/<seq>`                |Decoded detail of one retained record                         |
|GET    |`/api/v3/raw`                           |Exact bytes of the most recently accepted telegram (`text/plain`) |
|GET    |`/api/v3/raw/<seq>`                     |Exact accepted frame bytes as `text/plain`                    |
|GET    |`/api/v3/power`                         |Realtime power frame of the newest accepted telegram (phases and total watts/amps/volts, meter time, scale ladder step, history of the newest 100 accepted total watts) |
|GET    |`/api/v3/ringfile?ring=hours\|days\|months` |Records of one meter ring (hours by default), newest first |
|GET    |`/api/v3/graph?ring=hours\|days\|months`|Per-period used/returned kWh and gas m³ of one ring (hours by default; differences of consecutive counters), plus the tariff amounts (`ti`/`te`/`tG`) and network costs (`nc`) stored with the period, oldest first |
|GET    |`/api/v3/actualgraph`                   |Rolling table of the newest ~100 accepted telegrams: delivered and returned watts per phase, held gas m³/h and sample time, oldest first |
|GET    |`/api/v3/store`                         |Console `store` command as JSON: every LittleFS volume file with its size, plus capacity, used and free bytes |
|GET    |`/api/v3/csv?ring=hours\|days\|months`  |Console `ring2csv` content as a `text/csv` download: the exact `RING<name>.csv` a ring2csv would write (re-importable through populate), without touching the volume |
|POST   |`/api/v3/view`                          |Reports the active browser screen name to the device display  |

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
as the exact accepted frame bytes. Every burst sends the frame
back at 115200 baud and runs in a dedicated low-priority receiver
task, so the blocking writes never stall reception. The copie go out
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

Polarity follows the two SPA toggles. During a burst the copy leave the
pin at the selected *P1 Tx polarity*, and the RXD inversion of the current
*P1 RX polarity* setting is kept in the mask while it is active. Once TX is
idle again the TX line returns to the polarity of the current P1 Rx
polarity setting (the hardware update is skipped when it is already
correct), and on resume RX is restored to its own setting. Each copy is
confirmed on the wire with `uart_wait_tx_done()` (5 s timeout; a
maximum-sized copy takes ≈1.5 s); a partial write or timeout skips the
remaining copies and reception is still restored safely. A settings change
that arrives mid-burst waits for the burst to finish and applies to the
next operation, so the copy of one telegram always share one polarity.
The console runs on USB-Serial-JTAG and never touches UART0. The console
reports every burst with `forwarded <n> bytes on UART0 TX, tx polarity
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
|`version`        |firmware, parserDSMR and managed component versions              |
|`telegrams`      |the telegrams retained in RAM                                    |
|`telegram [<seq>]`|decoded fields of one retained telegram; without a sequence the most recently accepted telegram |
|`raw [<seq>]`    |exact accepted bytes of one retained telegram; without a sequence the most recently accepted telegram |
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
|User / Password        |broker credentials; both empty for an anonymous broker. The stored password is never sent to the browser: an empty password field keeps it, typing a new value replaces it |
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
comes from the router's lease; empty or 0 returns to plain DHCP). The mDNS
announcement comes from the `espressif/mdns` responder started with the
network identity: the SPA is reachable as `http://<hostname>.local/`
whatever the current lease address is (the default hostname gives
`http://dsmrlogger.local/` when no name is configured), and the responder
also publishes an `_http._tcp` service for service browsers. A save that
renames the device moves the `.local` name along with the DHCP name. Work
that touches the lease itself — re-requesting it for a new name, pinning or
withdrawing a fixed address — is performed by the Wi-Fi task within seconds
after the save replies, because it briefly leaves the station interface and
would otherwise abort the very browser request that saved it.

### Dynamic electricity tariffs

The device keeps **per-kWh electricity prices** for imported and exported
energy so tariff-aware views and automations can ask what a kWh costs (or
earned) at any instant. The logger does not fetch prices itself: an
application on the network publishes them — the supplier feed, Home
Assistant, Node-RED or the example scripts `tools/getPrices.py` (EnergyZero
day-ahead feed) and `tools/randomEnergyTariffs.py` (synthetic test schedule).
End-user guidance lives in `dynamicPricing.md` and `how2GetTariffs.md`.
Both delivery paths share one validator and one store (the
`logger_tariffs` component), so a publisher does not need to know which
path carried its document.

**One document, two paths.** A batch is a single JSON document:

```json
{"schemaVersion":1,"currency":"EUR","intervals":[
  {"start":"2026-12-01T00:00:00Z","end":"2026-12-01T00:15:00Z",
   "importPricePerKwh":0.235,"exportPricePerKwh":0.08,"gasPricePerM3":1.09},
  {"start":"2026-12-01T00:15:00Z","end":"2026-12-01T00:30:00Z",
   "importPricePerKwh":-0.042,"gasPricePerM3":1.09}
]}
```

- `schemaVersion` must be `1`; `currency` is a three-letter code.
- Timestamps are exactly `YYYY-MM-DDTHH:MM:SSZ` in **UTC**. `start` is
  inclusive, `end` exclusive; any positive duration is allowed (quarter
  hours, hours, a whole day — whatever the feed delivers; a gas day may
  span many electricity quarter-hours).
- Each interval carries at least one of `importPricePerKwh`,
  `exportPricePerKwh` and `gasPricePerM3`; every submitted price is a
  finite number and **zero and negative prices are valid** (negative spot
  prices happen). A missing or `null` price means **unknown, never zero**.
  A dynamic tariff is one price that always applies — there is no
  normal/off-peak distinction.
- Electricity prices are per kWh and the gas price per m³, in the document
  currency, taken as submitted: whether VAT and taxes are included is the
  feed's choice and should be documented by the publisher (the logger only
  echoes the currency).

**Validation is all-or-nothing.** A batch failing any check is refused
whole, the reply and the log carry the reason, and the stored schedule
stays untouched. An interval whose start **and** end equal a stored
interval replaces it (so re-delivering the same document — what a retained
message does on every new subscription — is harmless); any other overlap
with the stored schedule is a conflict and refuses the batch. Intervals
outside the submitted batch keep standing, so history survives partial
updates.

**MQTT input.** The device subscribes (QoS 1) to
`<top topic>/<client id>/<sub topic>` whenever a broker is configured and
the station has joined; all three segments come from the Settings **MQTT**
group — the top topic (empty selects `lastdsmrlogger`), the **Client ID**
(the device segment; while empty the segment is left out, and the same id
is what the broker sees when the client connects) and the **Sub topic**
(empty selects `newTariffs`). The effective topic is reported by the
current-price endpoint, so it never has to be assembled by hand. Large documents are
reassembled from MQTT fragments before validation; oversized or malformed
payloads are dropped with a log line naming the reason. **Publish
retained** (`-r`) and the device recovers the last schedule on every boot
without waiting for the next update. Publish whenever prices change —
daily for day-ahead feeds, per update for intraday spot feeds; duplicate
or overlapping redelivery of the same document is a no-op.

```
mosquitto_pub -h <broker> -p 1883 -q 1 -r \
  -t 'lastdsmrlogger/newTariffs' \
  -s < batch.json
```

**HTTP API** (same document on the `POST`, route `/api/v3/newTariffs`):

```
curl -s -X POST -H 'Content-Type: application/json' \
  --data-binary @batch.json http://<device>/api/v3/newTariffs
# -> {"ok":true,"storedIntervals":192,"currency":"EUR"}
#    400 (or 500 for store problems) with {"ok":false,"error":"…"} on refusal

curl -s http://<device>/api/v3/tariffs
# -> current: mqttTopic, clockSynced, currency, storedIntervals and, when
#    available, the interval active now with its start/end/prices

curl -s 'http://<device>/api/v3/tariffs?from=2026-12-01T00:00:00Z&to=2026-12-02T00:00:00Z&resolution=raw'
curl -s 'http://<device>/api/v3/tariffs?from=2026-12-01T00:00:00Z&to=2026-12-02T00:00:00Z&resolution=hour'
curl -s '…&resolution=day'
curl -s '…&resolution=month'
```

`from` and `to` use the same UTC form, `to` is exclusive; `resolution`
defaults to `raw`.

**Calendar buckets and averages.** Hour, day and month buckets follow the
**local** calendar of the device time zone (CET/CEST), while their reported
`start`/`end` stay authoritative UTC instants. On the autumn transition the
repeated local hour returns as **two buckets with the same label and
different UTC boundaries**; the spring-forward skipped hour produces no
bucket at all. Prices aggregate as **time-weighted averages**: every bucket
carries the seconds actually covered by an import (or export) price next to
the bucket's total seconds, so partial coverage is distinguishable from
full coverage; a bucket with no coverage reports `null` prices and 0
covered seconds — unknown stays unknown.

**Stored tariff amounts.** The ring stores three tariff amounts per period,
named for the direction of the energy they value: the **import** amount
(what the grid delivered to the home — the used energy, a cost), the
**export** amount (what the home delivered back to the grid — returned
energy, a credit) and the **gas** amount. With the **Dynamic Tariffs**
switch on (Settings view, **Tariffs** group) the meter store values every
ring record from the table instead of the fictional demo amounts: each
minute of used energy is priced with the single `importPricePerKwh` the
table holds for that instant and each minute of delivered-back energy with
the `exportPricePerKwh` it holds (a dynamic tariff has no normal/off-peak
distinction); minutes the table does not cover — or prices it does not
carry — fall back to the manual prices. Gas is priced with the
**gasPricePerM3** the table holds for that instant when it carries one, and
with the manual **Tariff Gas Delivered (m3)** price otherwise. With the
switch off all three amounts use the manual prices.

The manual prices come in pairs per direction: **Tariff 1 Delivered (kWh)**
and **Tariff 2 Delivered (kWh)** for the used energy, **Tariff 1 Received
(kWh)** and **Tariff 2 Received (kWh)** for the delivered-back energy. The
meter's tariff indicator (OBIS `0-0:96.14.0`, published with every reading)
names the tariff that is active at that moment and picks which price of a
direction values that minute's energy; a meter that names no active tariff
prices like tariff 1. Both registers of a direction (the tariff 1 and
tariff 2 counters) grow into the one amount of their direction, so meters
that publish everything in a single register per direction price correctly
too. The
amounts accumulate inside the current period and restart when a new period
opens (or after a reboot, when the staged record is rewritten anyway);
counter jumps (a meter replacement, or a meter that never published a
telegram) move the baseline instead of producing a giant amount.

**Persistence and retention.** The accepted schedule is stored on the
meter store's LittleFS volume and reloaded after every reboot. At ingest
time everything older than the start of the previous calendar month is
pruned, so at least the current and the previous month always remain
queryable.

**Documented bounds.** Payload ≤ 96 KiB per document (both paths) carrying
printable ASCII only (bytes 0x20–0x7E): the document must be **one single
line**, pretty-printed JSON with newlines or tabs is refused, so minify it
first (for example `jq -c`), ≤ 1024 intervals per batch, ≤ 8192 stored
intervals (over 60 days of quarter-hour prices), ≤ 744 buckets per
aggregation (a full local month at hour resolution). A submission or query
over a bound is refused — payload
and batch problems as 400, storage problems as 500 — and never applies
partially.

**MQTT and HTTP together.** The paths are interchangeable for a single
device: an HTTP `POST` and an MQTT publish validate and apply through the
same code. The broker is not affected by an HTTP submission, so after a
reboot a retained MQTT document is applied again: keep the retained
message current (publish retained on every change) or do not use retained
messages at all, otherwise an old retained document can re-apply prices a
later HTTP `POST` had replaced.

### Importing historical periods (`populate`)

The console command `populate {hours|days|months}` imports historical
periods from a semicolon separated csv file on the LittleFS volume into one
meter ring. After the ring selector the command asks for the input file,
relative to the volume root (typically `RINGmonths.csv`); an empty
line cancels.

The ring files carry a record layout marker on the volume (the tariff
amounts moved from one per tariff — `t1`, `t2` — to one per direction of the
energy — `ti`, `te`, `tg` — so the old amounts would read back under the
wrong heading, and every record later grew the prorated `nc` network costs
field). A volume whose marker names another layout rebuilds its
three rings empty once, on the model of a ring whose configured capacity
changed, and the log asks for a repopulate; a retained file whose header
disagrees with the current record size rebuilds the same way. The tariff
schedule and the import csv files on the volume are untouched. Export the
rings with `ring2csv` before flashing a firmware that bumps the layout if the
retained periods matter.

The file starts with a header line. Its first column names the period key
(`YYMMDDHH`, `YYMMDD` or `YYMM`); the value columns are named by OBIS code
(with or without the `*255` method suffix) or, for the three tariff amounts
and the network costs that have no code of their own, by the rubrics of the
ring print (`ti`, `te`
and `tg`, named for the direction of the energy they value, and `nc`):

|Column                             |Ring field                |Unit|
|-----------------------------------|--------------------------|----|
|`1-0:1.8.1`                        |energy delivered tariff 1 |kWh |
|`1-0:1.8.2`                        |energy delivered tariff 2 |kWh |
|`1-0:2.8.1`                        |energy returned tariff 1  |kWh |
|`1-0:2.8.2`                        |energy returned tariff 2  |kWh |
|`0-1:24.1.0` (or `0-1:24.2.1`/`.3`)|gas delivered             |m³  |
|`ti`                               |import amount (used)      |€   |
|`te`                               |export amount (returned)  |€   |
|`tg`                               |gas amount                |€   |
|`nc`                               |network costs             |€   |

Unknown columns — including the `#` and the empty trailing columns
handwritten files may carry — are skipped. Every data line starts with the
period key (8, 6 or 4 digits) and carries its values in header order; **a
blank or missing counter value becomes 0**. A blank or missing `ti`, `te`,
`tg` or `nc` is stored anyway: the tariff amounts are filled with the
average tariffs of the manual **Tariff 1** prices (an imported line names no
tariff indicator, so it prices like tariff 1) and `nc` with the Settings
**Network & transport costs (month)** amount prorated to the ring's period
(days a thirtieth of the month, hours a thirtieth of a day). Several lines
of one period update the
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
`YYMMDDHH;1-0:1.8.1*255;1-0:1.8.2*255;1-0:2.8.1*255;1-0:2.8.2*255;0-1:24.1.0*255;ti;te;tg;nc`
followed by one line per retained record, oldest period first, the key as
eight `YYMMDDHH` digits, the five counters at three decimals and the three
tariff amounts and the network costs at two. Every field the ring file holds
is exported, so the
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
|Telegram history                        |1 record × (12 KB raw + up to 96 field snapshots) ≈ 50 KB     |
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
