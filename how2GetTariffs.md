fileVersion: [2] - 22-11-2026 - 19:55

# How to get dynamic energy tariffs for lastDSMRlogger

Run a script, Node-RED flow, or Home Assistant automation that fetches your supplier's prices, converts them to the format below, and submits them to lastDSMRlogger. Keep supplier credentials and contract-specific calculations in that integration. Compare a few converted prices against your supplier's app or tariff sheet before publishing them.

## Vandebron Dynamisch

Vandebron displays contract tariffs in its app. No publicly documented endpoint for those tariffs was identified. Its January 2026 terms identify EPEX SPOT Day-Ahead for Dutch electricity and LEBA TTF Day-Ahead for gas. Electricity market prices change every quarter, while Vandebron bills by the hour. The gas day runs from 06:00 to 06:00 Dutch local time.

If you construct your own estimate, verify the hourly pricing rule against your contract before converting quarter-hour prices. Add the applicable contract charges and taxes. For gas, verify that your data source matches the specified index and gas-day boundaries. Derive export compensation separately from your contract. Label estimates in your integration so users can distinguish them from prices obtained directly from the supplier.

Sources: [Vandebron Dynamisch](https://vandebron.nl/energie/energiecontract/dynamisch), [January 2026 supply terms](https://assets.ctfassets.net/l0vbdd13d5ww/4Jv8YoEcCIqpuw4n8dGNov/2aac04e1ba10a38ccc06fb8707b13933/Leveringsvoorwaarden_Dynamisch__geldig_vanaf_06-01-2026_.pdf).

## Tibber

Use Tibber's GraphQL endpoint at `https://api.tibber.com/v1-beta/gql` with your Tibber access token as an HTTP Bearer token. Set a `User-Agent` header. Select the correct home if the account contains several. The following query is a starting point for the documented hourly feed.

```graphql
{
  viewer {
    homes {
      id
      currentSubscription {
        priceInfo {
          today { startsAt total currency }
          tomorrow { startsAt total currency }
        }
      }
    }
  }
}
```

For a quarter-hour contract, consult the current schema for the `QUARTER_HOURLY` price range and use the correct resolution for your contract. Confirm which Dutch taxes and fees the returned `total` includes before mapping it to `importPricePerKwh`. Check the displayed app price before adding any adjustment. Leave export and gas fields absent unless you have a verified source for them. Cache the prices and check for tomorrow's schedule when it becomes available.

Sources: [Calling the Tibber API](https://developer.tibber.com/docs/guides/calling-api), [GraphQL reference](https://developer.tibber.com/docs/reference).

## Frank Energie

Community integrations use the GraphQL endpoint at `https://graphql.frankenergie.nl/` for Dutch market prices. The quarter-hour integration linked below documents the `marketPrices(date: ..., resolution: PT15M)` query. Electricity slots include `from`, `till`, and separate price components. Preserve the time boundaries returned by the source. The price-history project also documents a gas query.

Choose the resolution your contract uses. Verify how the returned market price, VAT, sourcing markup, and energy tax map to the price shown in your app. Check the current query and response shape before implementing the integration. Include export compensation only if your contract or data source provides it.

Sources: [Community quarter-hour integration and query](https://github.com/Bennie-JC/ha-frank-quarter-prices/blob/main/README_EN.md), [Community price history and gas field reference](https://indyjonesnl.github.io/frank-energie-price-history/api/).

## EnergyZero

The public EnergyZero API documents a GET prices endpoint with a date in `dd-mm-yyyy` format. Request electricity with `INTERVAL_QUARTER` or `INTERVAL_HOUR`, and gas with `INTERVAL_DAY`. Use the request parameters in the current API reference. The response can include the previous day and the following day when available. Match slots by their timestamps and keep gas-day boundaries as supplied.

Check whether the price field you select already includes tax and VAT. Apply any additional fees required by your contract. If you use EnergyZero as a market price source for a different supplier, verify the market index and billing period before estimating the contract tariff.

Source: [EnergyZero GetPrices API reference](https://docs.api.energyzero.nl/docs/api/swagger/public/energy-market-service-get-prices/).

## Vattenfall

Vattenfall publishes FlexPrijs tariffs on its customer-service page, in Mijn Vattenfall, and in the Energie app. No documented public FlexPrijs tariff API was identified. Use the displayed rates to check any estimate built from wholesale data and your contract terms. If you have TijdPrijs, build intervals from the fixed time blocks and rates in your contract. TijdPrijs has a separate pricing model.

Sources: [Vattenfall dynamic tariffs](https://www.vattenfall.nl/klantenservice/alles-over-je-dynamische-contract/), [TijdPrijs](https://www.vattenfall.nl/energie/tijdprijs/).

## Greenchoice

The separate *Dynamisch verbruik Greenchoice* app, published by EnergyZero, shows electricity and gas prices. The public EnergyZero feed described above can supply market data. Compare the interval duration and all-in rates with the Greenchoice app and apply the Greenchoice contract charges where applicable. A Greenchoice-specific public contract-rate API was not verified.

Source: [Dynamisch verbruik Greenchoice](https://play.google.com/store/apps/details?id=nl.energyzero.greenchoice).

## Essent

Home Assistant's Essent integration reads unauthenticated electricity and gas prices from Essent's public API. Use that integration or inspect the current client for request and response details. Read the available interval data, preserve its boundaries, and check the reported price against your Essent contract before forwarding it. Availability of the next day's prices can vary.

Sources: [Home Assistant Essent integration](https://www.home-assistant.io/integrations/essent/), [Python client](https://pypi.org/project/essent-dynamic-pricing/).

## Eneco and Oxxio

Eneco publishes dynamic electricity and gas tariffs on its tariff page. Oxxio displays current electricity tariffs on its own dynamic contract page. No supported public contract-rate API was verified for either brand. When building a price from market data, use the billing period and fees for your own Eneco or Oxxio contract. Verify electricity, gas and export separately before publishing them.

Sources: [Eneco dynamic tariff page](https://www.eneco.nl/duurzame-energie/dynamisch-energiecontract/dynamische-tarieven/), [Oxxio dynamic contract and rates](https://www.oxxio.nl/energiecontract-afsluiten/dynamisch-energiecontract/).

## ENGIE Netherlands

A community Python client exists for the private consumer app API. Check its current documentation for the data it exposes before relying on it for tariff schedules. No supported public residential tariff endpoint was verified. ENGIE also advertises business APIs. Those have a separate onboarding process. If you use market data to estimate residential prices, compare the results with your ENGIE contract or app.

Sources: [Community ENGIE Netherlands client](https://github.com/AboveColin/engie-nl), [ENGIE business APIs](https://www.engie.nl/zakelijk/product-advies/api).

## ANWB Energie

ANWB publishes dynamic energy rates on its website. Home Assistant identifies ANWB Energie as an EnergyZero reseller, so the EnergyZero integration or public feed can provide the market-price input. Verify the billing intervals and ANWB-specific fees against the ANWB tariff display and your contract before publishing the final prices.

Sources: [ANWB current energy tariffs](https://www.anwb.nl/energie/actuele-tarieven), [Home Assistant EnergyZero integration](https://www.home-assistant.io/integrations/energyzero/).

## Format the payload and send it

- Use UTC timestamps ending in `Z`, with an inclusive start and exclusive end. Convert each boundary with its actual offset, including during daylight saving changes.
- Send final variable prices as JSON numbers in EUR per kWh for electricity and EUR per m³ for gas. If a source reports EUR per MWh for electricity, divide by 1,000. Check which costs are included before adding tax, VAT, or supplier charges. Keep fixed monthly charges out of the payload.
- Include at least one price per interval. Omit unknown prices. An actual zero or negative price is valid. Carry the gas price on the intervals it covers; intervals inside one document may not overlap each other, so a gas day is priced inside the electricity intervals rather than as a separate overlapping interval.
- If tomorrow's prices are unavailable, send the valid schedule you have and publish an updated schedule once the remaining prices arrive.
- The device validator accepts **printable ASCII only** (bytes 0x20–0x7E), so the document must be **one single line**: newlines and tabs are refused with `the document must be printable ASCII`. Minify before sending (for example `jq -c . tariffs.json`) or have your integration emit compact JSON.

Save the following example as `tariffs.json`. Replace the sample dates and prices with the intervals produced by your integration. The block is spread over several lines here to read; keep your real file on one single line (or minify when sending, as the commands below show).

```json
{
  "schemaVersion": 1,
  "currency": "EUR",
  "intervals": [
    {
      "start": "2026-11-22T12:00:00Z",
      "end": "2026-11-22T12:15:00Z",
      "importPricePerKwh": 0.2474,
      "exportPricePerKwh": 0.0812,
      "gasPricePerM3": 1.234
    },
    {
      "start": "2026-11-22T12:15:00Z",
      "end": "2026-11-22T12:30:00Z",
      "importPricePerKwh": 0.2470,
      "exportPricePerKwh": 0.0712,
      "gasPricePerM3": 1.234
    }
  ]
}
```

### MQTT

Publish the complete available schedule as a retained QoS 1 message to the topic configured on your logger. The topic is `<top topic>/<client id>/<sub topic>` from the Settings **MQTT** group; with top topic `lastlogger`, no client id and the default sub topic it is `lastlogger/newTariffs`. The current-price endpoint `GET /api/v3/tariffs` reports the effective topic as `mqttTopic`, so it never has to be assembled by hand. Replace the broker address and apply its authentication or TLS options as required.

```sh
mosquitto_pub -h mqtt.example.net -q 1 -r \
  -t lastlogger/newTariffs -m "$(jq -c . tariffs.json)"
```

### HTTP API

Send the JSON to `POST /api/v3/newTariffs`. Replace `lastlogger.local` with your logger's address and apply its authentication options if required.

```sh
curl --fail-with-body -X POST \
  -H 'Content-Type: application/json' \
  --data-binary "$(jq -c . tariffs.json)" \
  http://lastlogger.local/api/v3/newTariffs
```

## Python and C++ examples

These examples submit an already normalized `tariffs.json` to the logger. Use the supplier sections above to build the file. Run either example from the directory containing the file or pass its path as the second argument. The examples assume a logger on the local network with HTTP enabled.

### Python

Save as `post_tariffs.py`. It uses only the Python standard library.

```python
import argparse
import json
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser()
parser.add_argument("logger", help="Logger base URL, e.g. http://lastlogger.local")
parser.add_argument("file", nargs="?", default="tariffs.json", type=Path)
args = parser.parse_args()

try:
    schedule = json.loads(args.file.read_text(encoding="utf-8"))
    data = json.dumps(schedule, separators=(",", ":"), allow_nan=False).encode("ascii")
    request = Request(
        args.logger.rstrip("/") + "/api/v3/newTariffs",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request, timeout=15) as response:
        print(f"HTTP {response.status}")
        print(response.read().decode("utf-8", errors="replace"))
except HTTPError as error:
    print(error.read().decode("utf-8", errors="replace"))
    parser.exit(1, f"HTTP {error.code}\n")
except (OSError, ValueError, URLError) as error:
    parser.exit(1, f"{error}\n")
```

```sh
python3 post_tariffs.py http://lastlogger.local tariffs.json
```

### C++ with libcurl

Save as `post_tariffs.cpp`. This desktop example requires a C++ compiler and libcurl development headers. It posts the file bytes unchanged, so keep `tariffs.json` on one single line (no pretty-printing); the device refuses newlines and tabs.

```cpp
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "Usage: post_tariffs LOGGER_BASE_URL [tariffs.json]\n";
        return 2;
    }

    std::ifstream file(argc == 3 ? argv[2] : "tariffs.json", std::ios::binary);
    if (!file)
    {
        std::cerr << "Could not open the tariff file.\n";
        return 1;
    }
    const std::string payload{std::istreambuf_iterator<char>(file), {}};
    std::string url = argv[1];
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    url += "/api/v3/newTariffs";

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return 1;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (headers == nullptr)
    {
        curl_easy_cleanup(curl);
        return 1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (result != CURLE_OK)
    {
        std::cerr << curl_easy_strerror(result) << '\n';
        return 1;
    }
    std::cerr << "\nHTTP " << status << '\n';
    return status >= 200 && status < 300 ? 0 : 1;
}
```

```sh
c++ -std=c++17 post_tariffs.cpp -lcurl -o post_tariffs
./post_tariffs http://lastlogger.local tariffs.json
```
