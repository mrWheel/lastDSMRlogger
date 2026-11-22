#!/usr/bin/env python3
"""Generate synthetic electricity and gas tariffs for lastDSMRlogger."""

import argparse
import configparser
import json
import random
import re
import secrets
import socket
import ssl
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path


QUARTER = timedelta(minutes=15)
DEFAULT_CONFIG = Path(__file__).resolve().with_name("configEnergy.cfg")


def loadConfig(path):
    config = configparser.ConfigParser(interpolation=None)
    with path.open(encoding="utf-8") as handle:
        config.read_file(handle)

    broker = config["mqtt"]
    logger = config["logger"]
    hostname = logger["hostname"].strip().lower()
    if hostname.endswith(".local"):
        hostname = hostname[:-6]
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", hostname):
        raise ValueError("Set [logger] hostname to a hostname such as lastdsmrlogger.")

    topicSuffix = logger["topic"].strip().strip("/")
    if not topicSuffix:
        raise ValueError("Set [logger] topic to a topic suffix such as tariffs.")
    topic = f"{hostname}/{topicSuffix}"
    if any(character in topic for character in ("+", "#", "\0")):
        raise ValueError("The MQTT publish topic cannot contain wildcards or NUL.")

    port = broker.getint("port", fallback=1883)
    if not 1 <= port <= 65535:
        raise ValueError("The MQTT port must be between 1 and 65535.")

    return topic, broker, port


def utcTimestamp(value):
    return value.isoformat(timespec="seconds").replace("+00:00", "Z")


def makeSchedule(now):
    # Align to quarter-hour boundaries so repeated runs share interval keys.
    start = now.replace(minute=now.minute // 15 * 15, second=0, microsecond=0)
    start -= timedelta(hours=2)
    target = start + timedelta(hours=48)

    intervals = []
    priceRanges = (("importPricePerKwh", 1800, 3500),
                   ("exportPricePerKwh", 300, 1600),
                   ("gasPricePerM3", 9000, 16000))
    previous = {}
    while start < target:
        end = start + QUARTER
        interval = {"start": utcTimestamp(start), "end": utcTimestamp(end)}
        for field, low, high in priceRanges:
            price = random.randint(low, high)
            while price == previous.get(field):
                price = random.randint(low, high)
            interval[field] = price / 10000
            previous[field] = price
        intervals.append(interval)
        start = end

    return {"schemaVersion": 1, "currency": "EUR", "intervals": intervals}


def mqttField(value):
    data = value.encode("utf-8")
    return len(data).to_bytes(2, "big") + data


def sendPacket(connection, header, body=b""):
    remaining = len(body)
    length = bytearray()
    while remaining >= 128:
        length.append((remaining % 128) | 0x80)
        remaining //= 128
    length.append(remaining)
    connection.sendall(bytes([header]) + length + body)


def readExactly(connection, length):
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise OSError("The broker closed the connection.")
        data.extend(chunk)
    return bytes(data)


def readAcknowledgment(connection, expectedHeader):
    # MQTT 3.1.1 CONNACK and PUBACK both have a two-byte body.
    if readExactly(connection, 2) != bytes([expectedHeader, 2]):
        raise OSError("The broker returned an unexpected MQTT packet.")
    return readExactly(connection, 2)


def publishTariffs(host, port, username, password, tlsContext, topic, payload):
    with socket.create_connection((host, port), timeout=15) as tcp:
        if tlsContext is None:
            publishOnConnection(tcp, username, password, topic, payload)
        else:
            with tlsContext.wrap_socket(tcp, server_hostname=host) as connection:
                publishOnConnection(connection, username, password, topic, payload)


def publishOnConnection(connection, username, password, topic, payload):
    # MQTT 3.1.1 CONNECT with Clean Session and a 30-second keepalive.
    flags = 0x02
    fields = mqttField("tariffs" + secrets.token_hex(6))
    if username:
        flags |= 0xC0
        fields += mqttField(username) + mqttField(password)
    sendPacket(connection, 0x10,
               mqttField("MQTT") + bytes([4, flags, 0, 30]) + fields)

    acknowledgment = readAcknowledgment(connection, 0x20)
    if acknowledgment != bytes([0, 0]):
        raise OSError(f"The broker refused the MQTT connection (code {acknowledgment[1]}).")

    # PUBLISH with QoS 1 and RETAIN set. Only one packet is in flight.
    packetId = bytes([0, 1])
    sendPacket(connection, 0x33, mqttField(topic) + packetId + payload.encode("utf-8"))
    if readAcknowledgment(connection, 0x40) != packetId:
        raise OSError("The MQTT acknowledgement has an unexpected packet ID.")
    sendPacket(connection, 0xE0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the JSON without contacting the broker.")
    args = parser.parse_args()

    try:
        topic, broker, port = loadConfig(args.config)
        schedule = makeSchedule(datetime.now(timezone.utc))
        if args.dry_run:
            print(json.dumps(schedule, indent=2))
            return 0

        brokerHost = broker["host"].strip()
        if not brokerHost or brokerHost == "YOUR_MQTT_BROKER":
            raise ValueError("Set [mqtt] host to the address of your MQTT broker.")

        username = broker.get("username", "")
        password = broker.get("password", "")
        if password and not username:
            raise ValueError("Set [mqtt] username when using a password.")

        tlsContext = None
        if broker.getboolean("tls", fallback=False):
            caFile = broker.get("ca_file", "").strip()
            caPath = str(args.config.resolve().parent / caFile) if caFile else None
            tlsContext = ssl.create_default_context(cafile=caPath)

        payload = json.dumps(schedule, separators=(",", ":"))
        publishTariffs(brokerHost, port, username, password, tlsContext, topic, payload)

        intervals = schedule["intervals"]
        print(f"Published {len(intervals)} test intervals to {topic}.")
        print(f"UTC coverage: {intervals[0]['start']} to {intervals[-1]['end']}.")
        return 0
    except (OSError, ValueError, KeyError, configparser.Error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
