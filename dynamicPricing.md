# Dynamic pricing with lastDSMRlogger

fileVersion: [4] - 23-11-2026 - 22:35
Buildnumber: 4 - Last update-date: 23-11-2026

Who is this for: users who want the logger to show real energy costs (and the
current kWh price) instead of fixed manual prices, and who are comfortable
running a Python script or a `curl` command on their network.

The logger never fetches prices itself. You obtain prices from your supplier
(see [how2GetTariffs.md](how2GetTariffs.md) per supplier) and deliver them to
the device as small JSON documents, over **MQTT** or over the **HTTP API**.
The device validates each document as a whole, stores the schedule on its
flash volume and uses it to price the energy your meter reports.

```
 supplier / EnergyZero / Home Assistant / Node-RED / your own script
        │
        │  one JSON document per schedule (today + tomorrow)
        ├──► MQTT broker ──► logger (subscribes QoS 1, retained)
        └──► HTTP POST /api/v3/newTariffs
                     │
                     ▼
        tariff store (validated, persisted across reboots)
                     │
                     ├──► GET /api/v3/tariffs            current price + queries
                     └──► meter store: prices every minute of consumption
                          → Tariff €/months Costs columns in the SPA
```

## 1. What you get once prices are flowing

- `GET /api/v3/tariffs` answers the interval that is active right now
  (import, export and gas price), or says it is unavailable.
- The ring stores its tariff amounts **per direction of the energy**: the
  import amount (what the grid delivered to the home — the used energy, a
  cost), the export amount (what the home delivered back — returned energy,
  a credit) and the gas amount. Every record also stores the fixed monthly
  **Network & transport costs (month)** amount from Settings, prorated to
  the ring's period (the days ring a thirtieth of the month, the hours ring
  a thirtieth of a day).
- With the **Dynamic Tariffs** switch on (Settings view, **Tariffs** group),
  the meter store values the stored ring records with the table instead of
  the manual prices: each minute of used energy is priced with the import
  price the table holds for that instant, each minute of delivered-back
  energy with its export price, gas with the `gasPricePerM3` for
  that instant. Minutes the table does not cover fall back to the manual
  **Tariff 1 Delivered (kWh)**, **Tariff 2 Delivered (kWh)**, **Tariff 1
  Received (kWh)**, **Tariff 2 Received (kWh)** and **Tariff Gas Delivered
  (m3)** prices
  (Settings, same group). The meter's tariff indicator (OBIS `0-0:96.14.0`)
  names which of the two tariff prices of a direction values a minute; a
  meter that names no active tariff prices like tariff 1. A dynamic tariff
  is **one price that always
  applies per direction** — there is no normal/off-peak distinction.
- The amounts become visible in the **Show RingFile** view
  (columns **Import €**, **Export €**, **Gas €**, **Netw. €**), in the
  **Hours** and **Days** views under **Presentation: Table** (the row's
  own **Used (€)**, **Returned (€)**, **Gas (€)** and **Total (€)**
  columns beside the quantities — every figure per row, these tables
  carry no summed rows) and in the
  **Months** view under **Presentation: Costs** (Electra, Export, Gas,
  Netw., Total
  and the advance amount). Every money column shares one formula:
  total = used kWh × import price − returned kWh × export price + gas
  m³ × gas price + the record's prorated network amount (the Total
  subtracts the Export credit, and the Netw. column prints the network
  amount stored with the record — the fixed
  monthly **Network & transport costs** setting as it was when the record
  was written). All prices — the manual Settings fields and the published
  feed prices alike — are positive amounts (per kWh, gas per m³); the
  export price is the net compensation received per delivered-back kWh,
  which the tables net as a credit. A `populate` import whose file carries no `ti`, `te`, `tg`
  or `nc` column (or leaves those values blank) stores them anyway: the
  tariff amounts as the average tariffs of the manual **Tariff 1** prices
  and the network costs prorated from the Settings month amount.

The **Tariffs** view lists the stored schedule (it reads
`GET /api/v3/tariffs?from=&to=&resolution=raw`); the interval that is
active right now is answered by plain `GET /api/v3/tariffs` (the **API**
view can open it too, or use `curl`).

## 2. Five-minute test with synthetic prices

`tools/randomEnergyTariffs.py` publishes 48 hours of synthetic quarter-hour
prices (import 0.18–0.35, export 0.03–0.16, gas 0.90–1.60 EUR) as one
retained QoS 1 MQTT message, so you can prove the whole chain before wiring
a real supplier feed. It uses the same config file as the real fetcher, and
needs only the Python standard library — no extra packages.

1. In the SPA **Settings** view, **MQTT** group, fill in at least **Broker IP
   or URL** and **Broker port** (the **Broadcast interval** may stay `0`:
   telegram publication is off, but the tariff subscription runs whenever a
   broker is named and the device is on Wi-Fi).
2. Copy `tools/configEnergy.cfg.org` to `tools/configEnergy.cfg` and fill in
   your broker. The `[logger]` entries must reproduce the device's **tariff
   topic** (see section 5): `[logger] hostname` is the **Top topic** segment
   and `[logger] topic` is the **Sub topic** segment.
3. Preview the document without touching the broker:

   ```sh
   python3 tools/randomEnergyTariffs.py --dry-run | head -20
   ```

4. Publish it:

   ```sh
   python3 tools/randomEnergyTariffs.py
   Published 192 test intervals to lastlogger/newTariffs.
   UTC coverage: 2026-11-22T16:15:00Z to 2026-11-24T16:15:00Z.
   ```

5. Verify on the device (replace `lastlogger.local` with your device's
   address or IP):

   ```sh
   curl -s http://lastlogger.local/api/v3/tariffs
   ```

   `clockSynced` must be `true` (the device sets its clock via SNTP after
   every Wi-Fi join) and `available` must be `true` with `start`, `end` and
   prices filled in.

The first published schedule is stored. A **second** run of the script
produces a schedule that no longer starts on the same quarter hour (the
start always is "now, minus 2 hours"), so the device rightly refuses it with
*"the batch overlaps a stored interval"* and keeps the first schedule. That
is the designed conflict rule, not a bug.

## 3. Real prices: `tools/getPrices.py`

`tools/getPrices.py` fetches today (and tomorrow, as soon as published) from
the public EnergyZero endpoint
`https://public.api.energyzero.nl/public/v1/prices` — quarter-hour
electricity and daily gas, both `base_with_vat` — adds your contract
surcharges and publishes the same document to your broker. Again standard
library only.

`configEnergy.cfg`:

```ini
[mqtt]
host = YOUR_MQTT_BROKER
port = 1883
username =
password =
client_id =
tls = false
ca_file =

[logger]
# Top topic segment of the device tariff topic (a .local suffix is accepted).
hostname = lastlogger
# Sub topic segment (everything after the first / of the effective topic).
topic = newTariffs

[tariffs]
# EUR, VAT included; added to the market price incl. VAT. Zero is valid.
import_surcharge_kwh = 0
energy_tax_kwh = 0
gas_surcharge_m3 = 0
energy_tax_m3 = 0
# Signed adjustment on the export price; REQUIRED, use 0 for the plain
# VAT-included market price as export price.
export_surcharge_kwh = 0
```

The produced interval prices are:

```
importPricePerKwh = base_with_vat + import_surcharge_kwh + energy_tax_kwh
exportPricePerKwh = base_with_vat + export_surcharge_kwh
gasPricePerM3     = gas base_with_vat + gas_surcharge_m3 + energy_tax_m3
```

Compare the result once against your supplier app before trusting it:
whether a fee belongs in the feed or is billed separately is a contract
question, and the logger prices with exactly the numbers you send.

Run it after your supplier publishes (for day-ahead feeds around 14:00
Dutch time), for example from cron:

```sh
# crontab -e  (machine that runs the script)
10 14 * * * cd /home/you/lastDSMRlogger/tools && python3 getPrices.py >> tariffs.log 2>&1
```

Script behaviour worth knowing (all visible in the code):

- `--dry-run` prints the exact JSON without publishing — inspect it first.
- Electricity and gas are requested for "today"; the feed also returns
  tomorrow once published. Coverage spans two local days.
- A quarter-hour only gets a gas price when one gas day **fully covers** it;
  quarter-hours without gas coverage are dropped and the script reports
  *"Kwartieren zonder geldig gastarief zijn overgeslagen"*.
- `export_surcharge_kwh` is mandatory in the config (empty aborts with an
  error); there is no way to omit the export price — set `0` if your feed
  has no real export compensation and you accept the market price as is.
- The publish topic is `hostname` + `/` + `topic` from `[logger]`, always
  those two segments; publish is QoS 1 **retained**.
- If the broker refuses the schedule the MQTT publish is still confirmed;
  rejection reasons from the device land in the device log (see section 9).

## 4. The tariff document (identical on both paths)

Both the MQTT message and the HTTP body carry the same single JSON document:

```json
{
  "schemaVersion":1,
  "currency":"EUR",
  "intervals":
  [
    {
      "start":"2026-12-01T00:00:00Z",
      "end":"2026-12-01T00:15:00Z",
      "importPricePerKwh":0.235,
      "exportPricePerKwh":0.08,
      "gasPricePerM3":1.09
    },
    {
      "start":"2026-12-01T00:15:00Z",
      "end":"2026-12-01T00:30:00Z",
      "importPricePerKwh":-0.042,
      "exportPricePerKwh":0.096,
      "gasPricePerM3":1.09
    }
  ]
}
```

Rules enforced by the device (a document failing **any** check is refused
whole, the stored schedule stays untouched):

| Item                | Requirement                                                                                       |
|---------------------|---------------------------------------------------------------------------------------------------|
| `schemaVersion`     | must be the number `1`                                                                              |
| `currency`          | three-letter code (stored uppercase)                                                                |
| `intervals`         | non-empty array; ≤ 1024 entries per batch                                                           |
| `start` / `end`     | strings, exactly `YYYY-MM-DDTHH:MM:SSZ` in **UTC**; `end` after `start`; start inclusive, end exclusive; any positive duration (15 min, 1 h, a whole gas day) |
| prices              | optional; each of `importPricePerKwh`, `exportPricePerKwh`, `gasPricePerM3` either a finite number or `null`/absent; **at least one per interval**; zero and negative prices are valid |
| inside one batch    | intervals may touch but never overlap                                                               |
| whole document      | ≤ 96 KiB, printable ASCII only (see the one-line rule below), JSON nesting ≤ 6                      |

A missing or `null` price means **unknown, never zero**.

**The document must be one single line.** The validator accepts only bytes
0x20–0x7E, so pretty-printed JSON with newlines or tabs is rejected with
*"the document must be printable ASCII"*. Keep the file compact or minify it
when sending:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  --data-binary "$(jq -c . tariffs.json)" \
  http://lastlogger.local/api/v3/newTariffs
```

**Merging into the store** (same logic for both paths):

- An interval with the **same start and end** as a stored one **replaces**
  it — re-delivering the same document is harmless.
- Any other overlap with a stored interval is a **conflict**: the whole
  batch is refused (HTTP 400 `"the batch overlaps a stored interval"`, MQTT
  drop + warning in the device log).
- Intervals outside the submitted batch keep standing, so history survives
  partial updates.
- Everything that ended before the start of the **previous local calendar
  month** is pruned at ingest; the accepted schedule is persisted to
  `tariffs.bin` on the device's LittleFS volume and reloaded after every
  reboot.

So a normal supplier flow (publish today+tomorrow every day) replaces the
intervals it published before only when their boundaries are identical;
publishing shifted boundaries is refused. Keep your timestamps aligned
(e.g. always on quarter-hour instants).

## 5. MQTT path

The device subscribes (QoS 1) to one tariff topic as soon as a **broker** is
configured and the station has joined Wi-Fi. All segments come from the
**MQTT** group in the Settings view:

| Setting (Settings → MQTT) | Role in the tariff topic                                       | Default when empty      |
|---------------------------|----------------------------------------------------------------|-------------------------|
| **Top topic**             | first segment                                                   | `lastdsmrlogger`        |
| **Client ID**             | optional middle segment (also announced to the broker)          | segment left out        |
| **Sub topic**             | last segment                                                    | `newTariffs`            |

Effective topic: `<top topic>/<client id>/<sub topic>` (two segments when
the client id is empty). You never have to assemble it by hand — ask the
device:

```sh
curl -s http://lastlogger.local/api/v3/tariffs | jq .mqttTopic
"lastlogger/newTariffs"
```

Make your publisher (and the `[logger]` entries of `configEnergy.cfg`) use
exactly that topic.

Publishing recommendations:

- **One retained QoS 1 message** holding the full available schedule
  (typically today + tomorrow). Retention lets the device recover the last
  schedule the moment it (re)connects, even before the next publisher run.
- Publish whenever prices change: once a day after the day-ahead release,
  per update for intraday feeds. Duplicate redelivery (retained message to a
  rebooting device) is a no-op.
- Fragmented MQTT payloads are reassembled before validation; oversized or
  malformed messages are dropped with a reason in the device log — the
  stored schedule never breaks.
- The MQTT client also carries the telegram broadcast (if the interval is
  above 0); the tariff subscription runs either way.

Manual check with mosquitto:

```sh
mosquitto_pub -h mqtt.example.net -p 1883 -q 1 -r \
  -t lastlogger/newTariffs \
  -m "$(jq -c . tariffs.json)"
```

## 6. HTTP path

No authentication is required on the device's local API.

**Submit a schedule:**

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  --data-binary "$(jq -c . tariffs.json)" \
  http://lastlogger.local/api/v3/newTariffs
# accepted:  {"ok":true,"storedIntervals":192,"currency":"EUR"}
# refused:   HTTP 400 {"ok":false,"error":"the batch overlaps a stored interval"}
# store/mem: HTTP 500 {"ok":false,"error":"the schedule could not be persisted"}
```

**Current price** (this is also how you verify the MQTT topic and the clock):

```sh
curl -s http://lastlogger.local/api/v3/tariffs
```

```json
{"type":"tariff","mqttTopic":"lastlogger/newTariffs","clockSynced":true,
 "currency":"EUR","storedIntervals":192,"available":true,
 "start":"2026-11-22T18:15:00Z","end":"2026-11-22T18:30:00Z",
 "importPricePerKwh":0.210400,"exportPricePerKwh":0.081200,
 "gasPricePerM3":1.234000}
```

`available:false` (with the price fields omitted) when the clock is not
synchronised yet or no stored interval covers "now".

**Query stored intervals** — `from`, `to` and `resolution` always appear
together; timestamps use the same UTC form, `to` is exclusive:

```sh
curl -s 'http://lastlogger.local/api/v3/tariffs?from=2026-12-01T00:00:00Z&to=2026-12-02T00:00:00Z&resolution=raw'
curl -s 'http://lastlogger.local/api/v3/tariffs?from=2026-12-01T00:00:00Z&to=2026-12-02T00:00:00Z&resolution=hour'
curl -s '…&resolution=day'
curl -s '…&resolution=month'
```

`raw` returns the original intervals (absent prices as `null`):

```json
{"type":"tariffQuery","resolution":"raw","from":"2026-12-01T00:00:00Z",
 "to":"2026-12-02T00:00:00Z","currency":"EUR","intervals":[
   {"start":"2026-12-01T00:00:00Z","end":"2026-12-01T00:15:00Z",
    "importPricePerKwh":0.235000,"exportPricePerKwh":null,
    "gasPricePerM3":1.090000}],
 "count":1}
```

`hour`, `day` and `month` return **time-weighted averages** per *local*
calendar bucket (the device clock runs on `CET-1CEST`):

```json
{"type":"tariffQuery","resolution":"hour","from":"2026-12-01T00:00:00Z",
 "to":"2026-12-01T01:00:00Z","currency":"EUR","buckets":[
   {"start":"2026-11-30T23:00:00Z","end":"2026-12-01T00:00:00Z",
    "localLabel":"2026-12-01 00:00","importPricePerKwh":0.247400,
    "importCoveredSeconds":3600,"exportPricePerKwh":null,
    "exportCoveredSeconds":0,"totalSeconds":3600}],
 "count":1}
```

- An average is only meaningful while its `…CoveredSeconds` is above 0; a
  bucket with no coverage of a price reports `null` (unknown stays unknown).
  15 minutes at 0.20 and 45 minutes at 0.28 average 0.26.
- Gas is **not** averaged into buckets — read gas from `resolution=raw`.
- On the autumn DST change the repeated local hour comes back as **two
  buckets with the same `localLabel` and different UTC boundaries**; the
  spring-forward hour produces no bucket at all. The `start`/`end` UTC
  instants are always authoritative.

## 7. Clock, time zone and daylight saving

- All document timestamps are **UTC**. Convert your supplier's local times
  with their real offset (01:00 in December is `00:00Z`, in July `02:00Z` —
  mind summer/winter time per date, not per month).
- The device synchronises its clock via SNTP (`pool.ntp.org`) after every
  Wi-Fi join and runs on `CET-1CEST`. Before the first synchronisation
  `clockSynced` is `false`, the current price reports `available:false`, and
  retention pruning waits — prices you submit are still stored and become
  usable as soon as the clock is set.

## 8. Reference: documented bounds

| Bound                             | Limit                                  | On exceeding              |
|-----------------------------------|----------------------------------------|---------------------------|
| JSON document (both paths)        | 96 KiB, printable ASCII, nesting ≤ 6   | 400 / MQTT drop           |
| Intervals per batch               | 1024                                   | 400 / MQTT drop           |
| Stored intervals                  | 8192 (over 60 days of quarter-hours)   | 400 / MQTT drop           |
| Raw answer per query              | 2048 intervals                         | 400 "narrow the range"    |
| Buckets per aggregation           | 744 (one full local month at hour)     | 400 "range refused (too wide)" |

A refused submission never applies partially.

## 9. Troubleshooting

| Symptom                                          | Cause and fix                                                                                          |
|--------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `available:false` while intervals are stored     | Clock not synchronised (`clockSynced:false`) — check Wi-Fi/SNTP; or "now" falls outside every stored interval (stale schedule) |
| MQTT publishes succeed but nothing changes       | Topic mismatch — compare the publisher topic with `mqttTopic` from `GET /api/v3/tariffs`; watch the device log for `tariff message via MQTT rejected: …` |
| Device log: `could not subscribe to the tariff topic … (retrying in 10 s)` | Broker not ready/reachable at that moment; it retries automatically every 10 s |
| `the document must be printable ASCII`           | Newlines/tabs in the body — send compact JSON (`jq -c`)                                                 |
| `the batch overlaps a stored interval`           | New boundaries intersect stored ones; align timestamps, or this is the expected refusal of a re-run with shifted starts |
| `currency must be a three-letter code`           | `currency` missing or not exactly three letters                                                          |
| `every interval must carry at least one price`   | Interval without any of the three price fields                                                           |
| Prices never change the € columns                | **Dynamic Tariffs** switch off in Settings → Tariffs, or table covers nothing for that period (manual fallback prices used) |
| MQTT re-applies old prices after a reboot        | A stale retained message: always publish retained on every change, or stop using retained — do not mix retained MQTT with one-off HTTP POSTs |

## 10. Where to go next

- [how2GetTariffs.md](how2GetTariffs.md) — per-supplier guidance (Tibber,
  EnergyZero, Frank Energie, Vandebron, …) and ready-made Python/C++ POST
  examples for building `tariffs.json`.
- `README.md`, section *Dynamic electricity tariffs* — the developer-facing
  reference for the same endpoints and bounds.
- Home Assistant / Node-RED: subscribe to your supplier feed and either
  publish retained MQTT to the device tariff topic or POST the same JSON to
  `/api/v3/newTariffs`; both paths share one validator and one store.
