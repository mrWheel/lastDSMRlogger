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

## Format the payload and send it

- Use UTC timestamps ending in `Z`, with an inclusive start and exclusive end. Convert each boundary with its actual offset, including during daylight saving changes.
- Send final variable prices as JSON numbers in EUR per kWh for electricity and EUR per m³ for gas. If a source reports EUR per MWh for electricity, divide by 1,000. Check which costs are included before adding tax, VAT, or supplier charges. Keep fixed monthly charges out of the payload.
- Include at least one price per interval. Omit unknown prices. An actual zero or negative price is valid. Gas can have a separate daily interval that overlaps electricity intervals.
- If tomorrow's prices are unavailable, send the valid schedule you have and publish an updated schedule once the remaining prices arrive.

Save the following example as `tariffs.json`. Replace the sample dates and prices with the intervals produced by your integration.

```json
{
  "schemaVersion": 1,
  "currency": "EUR",
  "intervals": [
    {
      "start": "2026-11-22T12:00:00Z",
      "end": "2026-11-22T12:15:00Z",
      "importPricePerKwh": 0.2474,
      "exportPricePerKwh": 0.0812
    },
    {
      "start": "2026-11-22T05:00:00Z",
      "end": "2026-11-23T05:00:00Z",
      "gasPricePerM3": 1.234
    }
  ]
}
```

### MQTT

Publish the complete available schedule as a retained QoS 1 message to the topic configured on your logger. With hostname `lastlogger` and topic suffix `tariffs`, use `lastlogger/tariffs`. Replace the broker address and apply its authentication or TLS options as required.

```sh
mosquitto_pub -h mqtt.example.net -q 1 -r \
  -t lastlogger/tariffs -f tariffs.json
```

### HTTP API

Use the tariff POST endpoint documented for your firmware. The example assumes `/api/v3/tariffs` and the hostname `lastlogger.local`. Add the logger's authentication options if required.

```sh
curl --fail-with-body -X POST \
  -H 'Content-Type: application/json' \
  --data-binary @tariffs.json \
  http://lastlogger.local/api/v3/tariffs
```
