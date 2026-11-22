#!/usr/bin/env python3
"""
Haalt EPEX-stroomprijzen en TTF-gasprijzen (vandaag + morgen) op via het nieuwe
publieke EnergyZero REST-endpoint en stuurt deze als JSON naar een MQTT-broker.
Leest standaard 'configEnergy.cfg' uit dezelfde map als dit script.
Volledig self-contained: gebruikt uitsluitend de Python Standard Library.
"""

import argparse
import configparser
import json
import math
import secrets
import socket
import ssl
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo


# =====================================================================
# Ingebouwde minimale MQTT v3.1.1 Client (geen externe libraries nodig)
# =====================================================================
class MinimalMQTTClient:
    def __init__(self, client_id: str, username: str = "", password: str = ""):
        self.client_id = client_id.encode("utf-8")
        self.username = username.encode("utf-8") if username else None
        self.password = password.encode("utf-8") if password else None
        self.sock = None

    @staticmethod
    def _encode_str(data: bytes) -> bytes:
        return struct.pack("!H", len(data)) + data

    @staticmethod
    def _encode_remaining_length(length: int) -> bytes:
        encoded = bytearray()
        while True:
            digit = length % 128
            length //= 128
            if length > 0:
                digit |= 0x80
            encoded.append(digit)
            if length == 0:
                break
        return bytes(encoded)

    def connect(self, host: str, port: int, use_tls: bool = False, ca_file: str = "", timeout: int = 10):
        raw_sock = socket.create_connection((host, port), timeout=timeout)
        if use_tls:
            context = ssl.create_default_context()
            try:
                if ca_file:
                    context.load_verify_locations(cafile=ca_file)
                self.sock = context.wrap_socket(raw_sock, server_hostname=host)
            except Exception:
                raw_sock.close()
                raise
        else:
            self.sock = raw_sock

        # CONNECT packet opbouwen (MQTT 3.1.1)
        var_header = bytearray(b"\x00\x04MQTT\x04")
        connect_flags = 0x02  # Clean session
        if self.username is not None:
            connect_flags |= 0x80
        if self.password is not None:
            connect_flags |= 0x40
        var_header.append(connect_flags)
        var_header.extend(struct.pack("!H", 60))  # Keepalive 60s

        payload = bytearray(self._encode_str(self.client_id))
        if self.username is not None:
            payload.extend(self._encode_str(self.username))
        if self.password is not None:
            payload.extend(self._encode_str(self.password))

        remaining = bytes(var_header) + bytes(payload)
        packet = b"\x10" + self._encode_remaining_length(len(remaining)) + remaining
        self.sock.sendall(packet)

        # Wacht op CONNACK (4 bytes: 0x20, 0x02, flags, return_code)
        connack = self._recv_exact(4)
        if connack[:3] != b"\x20\x02\x00":
            raise ConnectionError("Ongeldig MQTT CONNACK ontvangen.")
        if connack[3] != 0:
            raise ConnectionError(f"MQTT verbinding geweigerd (code {connack[3]}).")

    def publish(self, topic: str, message: str, retain: bool = True):
        topic_bytes = self._encode_str(topic.encode("utf-8"))
        msg_bytes = message.encode("utf-8")
        packet_id = b"\x00\x01"
        remaining = topic_bytes + packet_id + msg_bytes
        header_byte = 0x33 if retain else 0x32  # QoS 1
        packet = bytes([header_byte]) + self._encode_remaining_length(len(remaining)) + remaining
        self.sock.sendall(packet)
        if self._recv_exact(4) != b"\x40\x02" + packet_id:
            raise ConnectionError("Ongeldig MQTT PUBACK ontvangen.")

    def disconnect(self):
        if self.sock:
            try:
                self.sock.sendall(b"\xe0\x00")
            except OSError:
                pass
            finally:
                self.sock.close()
                self.sock = None

    def _recv_exact(self, num_bytes: int) -> bytes:
        buf = bytearray()
        while len(buf) < num_bytes:
            chunk = self.sock.recv(num_bytes - len(buf))
            if not chunk:
                raise ConnectionError("MQTT verbinding onverwacht verbroken")
            buf.extend(chunk)
        return bytes(buf)


# =====================================================================
# Prijsdata ophalen & verwerken (nieuwe EnergyZero API)
# =====================================================================
def _http_get_json(url: str) -> dict:
    ca_file = "/etc/ssl/cert.pem"
    context = ssl.create_default_context(cafile=ca_file if Path(ca_file).is_file() else None)
    request = urllib.request.Request(url, headers={
        "Accept": "application/json",
        "User-Agent": "DSMRlogger-TariffFetcher/1.0",
    })
    with urllib.request.urlopen(request, timeout=15, context=context) as response:
        result = json.load(response)
    if not isinstance(result, dict):
        raise ValueError("EnergyZero gaf geen JSON-object terug.")
    return result


def fetch_energyzero_public_prices(date_nl: datetime, energy_type: str, interval: str) -> dict:
    """
    Haalt prijzen op via https://public.api.energyzero.nl/public/v1/prices
    date_nl: datum in dd-mm-yyyy formaat
    energy_type: 'ENERGY_TYPE_ELECTRICITY' of 'ENERGY_TYPE_GAS'
    interval: 'INTERVAL_QUARTER' of 'INTERVAL_DAY'
    """
    base_url = "https://public.api.energyzero.nl/public/v1/prices"
    params = {
        "date": date_nl.strftime("%d-%m-%Y"),
        "energyType": energy_type,
        "interval": interval,
    }
    url = f"{base_url}?{urllib.parse.urlencode(params)}"
    return _http_get_json(url)


def parse_zulu(ts: str) -> datetime:
    value = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    if value.utcoffset() is None:
        raise ValueError(f"Tijdzone ontbreekt in EnergyZero-tijdstempel: {ts}")
    return value.astimezone(timezone.utc)


def format_zulu(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def price_slots(response, start, end, label):
    entries = response.get("base_with_vat")
    if not isinstance(entries, list):
        raise ValueError(f"EnergyZero {label}: veld base_with_vat ontbreekt of is ongeldig.")

    slots = {}
    for entry in entries:
        slot_start = parse_zulu(entry["start"])
        slot_end = parse_zulu(entry["end"])
        if slot_end <= slot_start:
            raise ValueError(f"EnergyZero {label}: ongeldig prijsinterval.")
        if slot_start >= end or slot_end <= start:
            continue
        price = float(entry["price"]["value"])
        if not math.isfinite(price):
            raise ValueError(f"EnergyZero {label}: ongeldige prijs.")
        key = (slot_start, slot_end)
        if key in slots and slots[key] != price:
            raise ValueError(f"EnergyZero {label}: tegenstrijdige prijzen voor hetzelfde interval.")
        slots[key] = price

    result = []
    for (slot_start, slot_end), price in sorted(slots.items()):
        if result and slot_start < result[-1][1]:
            raise ValueError(f"EnergyZero {label}: overlappende prijsintervallen.")
        result.append((slot_start, slot_end, price))
    return result


def build_price_schedule(tariffs_cfg, now=None):
    now = datetime.now(timezone.utc) if now is None else now
    local_now = now.astimezone(ZoneInfo("Europe/Amsterdam"))
    today = local_now.replace(hour=0, minute=0, second=0, microsecond=0)
    start = today.astimezone(timezone.utc)
    end = (today + timedelta(days=2)).astimezone(timezone.utc)

    # Een aanvraag voor vandaag levert ook morgen, zodra gepubliceerd.
    electricity = fetch_energyzero_public_prices(today, "ENERGY_TYPE_ELECTRICITY",
                                                 "INTERVAL_QUARTER")
    gas = fetch_energyzero_public_prices(today, "ENERGY_TYPE_GAS", "INTERVAL_DAY")
    electric_slots = price_slots(electricity, start, end, "stroom")
    gas_slots = price_slots(gas, start, end, "gas")
    if not any(a <= now < b for a, b, _ in electric_slots):
        raise ValueError("Geen actueel stroomtarief ontvangen van EnergyZero.")
    if not any(a <= now < b for a, b, _ in gas_slots):
        raise ValueError("Geen actueel gastarief ontvangen van EnergyZero.")

    imp_extra = tariffs_cfg["import_surcharge_kwh"] + tariffs_cfg["energy_tax_kwh"]
    gas_extra = tariffs_cfg["gas_surcharge_m3"] + tariffs_cfg["energy_tax_m3"]
    export_extra = tariffs_cfg["export_surcharge_kwh"]
    intervals = []
    for a, b, price in electric_slots:
        # Een kwartier krijgt alleen een prijs als de gasdag het volledig dekt.
        gas_price = next((value for start, end, value in gas_slots
                          if start <= a and b <= end), None)
        if gas_price is None:
            continue
        intervals.append({
            "start": format_zulu(a),
            "end": format_zulu(b),
            "importPricePerKwh": round(price + imp_extra, 4),
            "exportPricePerKwh": round(price + export_extra, 4),
            "gasPricePerM3": round(gas_price + gas_extra, 4),
        })
    if not any(parse_zulu(item["start"]) <= now < parse_zulu(item["end"]) for item in intervals):
        raise ValueError("Geen volledig actueel kwartier met stroom- en gastarief ontvangen.")
    if len(intervals) < len(electric_slots):
        print("Kwartieren zonder geldig gastarief zijn overgeslagen.", file=sys.stderr)
    return {"schemaVersion": 1, "currency": "EUR", "intervals": intervals}


def load_config(config_path):
    cfg = configparser.ConfigParser(interpolation=None)
    with config_path.open(encoding="utf-8") as source:
        cfg.read_file(source)
    return cfg


def resolve_topic(cfg):
    hostname = cfg.get("logger", "hostname").strip().lower()
    if hostname.endswith(".local"):
        hostname = hostname[:-6]
    suffix = cfg.get("logger", "topic").strip().strip("/")
    topic = f"{hostname}/{suffix}"
    if not hostname or "/" in hostname or not suffix or any(c in topic for c in "+#\x00"):
        raise ValueError("Ongeldige hostname of topic in [logger].")
    return topic


def resolve_ca_file(cfg, config_dir):
    raw = cfg.get("mqtt", "ca_file", fallback="").strip()
    return str((config_dir / raw).resolve()) if raw else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", nargs="?", type=Path,
                        default=Path(__file__).resolve().with_name("configEnergy.cfg"))
    parser.add_argument("--dry-run", action="store_true",
                        help="Haal tarieven op en toon JSON, zonder MQTT-publicatie.")
    args = parser.parse_args()

    try:
        cfg = load_config(args.config)
        topic = resolve_topic(cfg)
        keys = ("import_surcharge_kwh", "energy_tax_kwh", "gas_surcharge_m3", "energy_tax_m3")
        tariffs_cfg = {key: cfg.getfloat("tariffs", key) for key in keys}
        export = cfg.get("tariffs", "export_surcharge_kwh", fallback="").strip()
        if not export:
            raise ValueError("Vul [tariffs] export_surcharge_kwh in. Gebruik 0 voor de beursprijs inclusief btw.")
        tariffs_cfg["export_surcharge_kwh"] = float(export)
        if not all(math.isfinite(price) for price in tariffs_cfg.values()):
            raise ValueError("Ongeldig getal in [tariffs].")

        if not args.dry_run:
            mqtt_host = cfg.get("mqtt", "host").strip()
            mqtt_port = cfg.getint("mqtt", "port", fallback=1883)
            mqtt_user = cfg.get("mqtt", "username", fallback="")
            mqtt_client_id = cfg.get("mqtt", "client_id", fallback="").strip()
            if not mqtt_client_id:
                mqtt_client_id = "tariff-pub-" + secrets.token_hex(6)
            if "\x00" in mqtt_client_id or len(mqtt_client_id.encode("utf-8")) > 65535:
                raise ValueError("Ongeldige MQTT client_id in [mqtt].")
            mqtt_password = cfg.get("mqtt", "password", fallback="")
            mqtt_tls = cfg.getboolean("mqtt", "tls", fallback=False)
            ca_file = resolve_ca_file(cfg, args.config.resolve().parent)
            if not mqtt_host or mqtt_host == "YOUR_MQTT_BROKER" or not 1 <= mqtt_port <= 65535:
                raise ValueError("Vul een geldig brokeradres en poort in bij [mqtt].")
            if mqtt_password and not mqtt_user:
                raise ValueError("Vul ook [mqtt] username in wanneer je een wachtwoord gebruikt.")

        schedule = build_price_schedule(tariffs_cfg)
        payload = json.dumps(schedule, separators=(",", ":"), allow_nan=False)
        if args.dry_run:
            print(json.dumps(schedule, indent=2))
            return 0

        mqtt = MinimalMQTTClient(mqtt_client_id, mqtt_user, mqtt_password)
        try:
            mqtt.connect(mqtt_host, mqtt_port, mqtt_tls, ca_file)
            mqtt.publish(topic, payload, retain=True)
        finally:
            mqtt.disconnect()
        print(f"{len(schedule['intervals'])} intervallen bevestigd door de broker op {topic}.")
        return 0
    except urllib.error.HTTPError as error:
        print(f"EnergyZero HTTP {error.code}: {error.reason}", file=sys.stderr)
    except (OSError, ValueError, TypeError, KeyError, configparser.Error) as error:
        print(f"Fout: {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
