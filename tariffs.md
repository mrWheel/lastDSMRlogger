# Implement dynamic electricity tariffs in lastDSMRlogger

Extend lastDSMRlogger to accept dynamic electricity tariffs through MQTT and an HTTP API. Users obtain prices from their energy supplier and publish them through Home Assistant, Node-RED, or a custom application. Support any supplier that can produce the documented JSON format.

Inspect the project first. Reuse its MQTT client, HTTP routing, configuration, time handling, and history storage where suitable. Follow the project’s ESP-IDF and C conventions. Write all new code comments, API messages, UI text, and documentation in English.

Make a SPA [Tariffs] button to list the tariffs.

## Shared tariff format

Use the same versioned JSON format for MQTT and HTTP POST.
```
{
  "schemaVersion": 1,
  "currency": "EUR",
  "intervals": [
    {
      "start": "2026-11-22T12:00:00Z",
      "end": "2026-11-22T12:15:00Z",
      "importPricePerKwh": 0.2474,
      "exportPricePerKwh": 0.0812,
      "gasPricePerM3": 0.129
    },
    {
      "start": "2026-11-22T12:15:00Z",
      "end": "2026-11-22T12:30:00Z",
      "importPricePerKwh": 0.2216,
      "gasPricePerM3": 0.234
    }
  ]
}
```
- Use ISO 8601 UTC timestamps. Include the start and exclude the end. Accept any positive duration, including quarter-hour and hourly schedules.
- Require importPricePerKwh, the final variable price per kWh including applicable supplier charges, energy tax, and VAT. Accept optional exportPricePerKwh, the net variable compensation per exported kWh. Fixed charges and adjustments settled over longer periods are outside the scope of these fields.
- Accept zero and negative prices. Treat a missing export price as unknown.
- Validate the entire batch before storing it. Reject unsupported schema versions, invalid timestamps, non-finite prices, empty lists, and overlapping intervals within a batch. Allow gaps.
- Apply valid batches atomically through shared ingestion logic. Duplicate delivery must be harmless. An interval can replace a stored interval with the same start and end. Reject other overlaps with stored intervals and preserve existing data. Keep previously accepted intervals outside the submitted batch for historical queries.

## MQTT input

Reuse the existing MQTT client. Use <hostname>/tariffs/electricity as the default topic and adapt the prefix to existing configuration where applicable. Expose the effective topic and preserve compatibility with the existing telegram publication topic.

Recommend one retained QoS 1 message containing the available schedule, typically today and tomorrow when available. Reassemble fragmented MQTT payloads. Accept at least two full local days of quarter-hour prices, including a 25-hour day. Log the reason for invalid messages and preserve the stored schedule.

## HTTP API

Use the existing routing and authentication conventions. Implement these operations under /api/v3/tariffs/electricity or the equivalent project API prefix. Document the exact route.

|Request                                                    |Behavior                                                                                                                 |
|-----------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------|
|`POST /api/v3/tariffs`                            |Accept the shared JSON format and return success once stored. Return a clear client error for invalid input or conflicts.|
|`GET /api/v3/tariffs`                             |Return the currently active interval, or explicitly report that the tariff is unavailable.                               |
|`GET /api/v3/tariffs?from=…&to=…&resolution=raw`  |Return original intervals overlapping the requested half-open range.                                                     |
|`GET /api/v3/tariffs?from=…&to=…&resolution=hour` |Return average prices per local calendar hour.                                                                           |
|`GET /api/v3/tariffs?from=…&to=…&resolution=day`  |Return average prices per local calendar day.                                                                            |
|`GET /api/v3/tariffs?from=…&to=…&resolution=month`|Return average prices per local calendar month.                                                                          |

Use UTC for query timestamps and response boundaries. Reject invalid ranges and unsupported resolutions. Construct calendar buckets using the project’s configured time zone. On the autumn daylight saving transition, return the two occurrences of the repeated hour as separate buckets with different UTC boundaries.

## Hourly, daily, and monthly averages

Calculate each tariff field as a time-weighted average. Clip tariff intervals to the intersection of the request range and the calendar bucket. For each field, sum the price multiplied by the covered seconds and divide by that field’s total covered seconds.

Calculate import and export separately. A missing export price adds no export coverage. Report the covered seconds for each field and the total seconds in the requested portion of the bucket. Return null for averages with zero coverage. For example, an hour with only 15 minutes of export prices reports 900 seconds of export coverage.

Fifteen minutes at €0.20/kWh and 45 minutes at €0.28/kWh average €0.26/kWh. Compute daily and monthly averages from the original intervals or carry forward weighted totals and coverage. For actual energy costs, if supported, use the energy consumed or exported at each applicable tariff.

Persist accepted intervals across reboots using existing storage where suitable. Retain at least the current and previous calendar month. Bound storage and request sizes and document those limits. If the existing hour, day, and month history responses expose tariff fields, apply the same aggregation logic and preserve API compatibility.

## README and verification

Add a Dynamic electricity tariffs section to README.md. Explain where users obtain prices, how to find the MQTT topic, how often to publish, and how retained messages work. Include a complete example payload, a copyable mosquitto_pub command, and copyable curl commands for POST, current GET, raw GET, and each aggregation resolution. Document units, price inclusions, UTC timestamps, local calendar buckets, time-weighted averages, missing or partial coverage, retention, payload limits, and how MQTT and HTTP submissions interact.

Build the project and verify both ingestion paths, duplicate retained delivery, rejection without partial updates, recovery after reboot, and hourly, daily, and monthly averages. Include negative prices, missing export prices, intervals crossing bucket boundaries, and both daylight saving transitions. In the handoff, list the changed files, final endpoints, MQTT topic, and checks performed.