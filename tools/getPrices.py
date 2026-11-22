#!/usr/bin/env python3
"""
Haalt EPEX-stroomprijzen en TTF-gasprijzen (vandaag + morgen) op via het nieuwe
publieke EnergyZero REST-endpoint en stuurt deze als JSON naar een MQTT-broker.
Leest standaard 'configEnergy.cfg' uit dezelfde sub-map als dit script.
Volledig self-contained: gebruikt uitsluitend de Python Standard Library.
"""

import configparser
import json
import os
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
            if ca_file:
                context.load_verify_locations(cafile=ca_file)
            self.sock = context.wrap_socket(raw_sock, server_hostname=host)
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
        if connack != 0x20 or connack != 0x00:
            raise ConnectionError(f"MQTT verbinding geweigerd (code {connack})")

    def publish(self, topic: str, message: str, retain: bool = True):
        topic_bytes = self._encode_str(topic.encode("utf-8"))
        msg_bytes = message.encode("utf-8")
        remaining = topic_bytes + msg_bytes
        header_byte = 0x31 if retain else 0x30  # QoS 0, optioneel retain
        packet = bytes([header_byte]) + self._encode_remaining_length(len(remaining)) + remaining
        self.sock.sendall(packet)

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
def _create_ssl_context(verify: bool = True) -> ssl.SSLContext:
    if not verify:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    for ca_bundle in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"):
        if os.path.exists(ca_bundle):
            return ssl.create_default_context(cafile=ca_bundle)
    return ssl.create_default_context()


def _http_get_json(url: str) -> dict:
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "DSMRlogger-TariffFetcher/1.0",
        },
    )
    try:
        ctx = _create_ssl_context(verify=True)
        with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
            raw = resp.read().decode("utf-8").strip()
    except urllib.error.URLError as err:
        if isinstance(err.reason, ssl.SSLCertVerificationError):
            ctx = _create_ssl_context(verify=False)
            with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
                raw = resp.read().decode("utf-8").strip()
        else:
            raise

    if not raw:
        raise ValueError(f"Leeg antwoord ontvangen van {url}")
    return json.loads(raw)


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
    try:
        return _http_get_json(url)
    except urllib.error.HTTPError as e:
        # 404 betekent dat de prijzen voor morgen nog niet zijn gepubliceerd
        if e.code == 404:
            return {}
        raise


def parse_zulu(ts: str) -> datetime:
    return datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone(timezone.utc)


def format_zulu(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_price_schedule(tariffs_cfg: dict) -> dict:
    nl_tz = ZoneInfo("Europe/Amsterdam")
    now_nl = datetime.now(nl_tz)
    today_nl = now_nl.replace(hour=0, minute=0, second=0, microsecond=0)
    tomorrow_nl = today_nl + timedelta(days=1)
    day_after_nl = today_nl + timedelta(days=2)

    start_utc = today_nl.astimezone(timezone.utc)
    end_utc = day_after_nl.astimezone(timezone.utc)

    # Haal vandaag en morgen op (de API geeft per aanroep ook de aangrenzende dag terug)
    elec_today = fetch_energyzero_public_prices(today_nl, "ENERGY_TYPE_ELECTRICITY", "INTERVAL_QUARTER")
    elec_tomorrow = fetch_energyzero_public_prices(tomorrow_nl, "ENERGY_TYPE_ELECTRICITY", "INTERVAL_QUARTER")

    gas_today = fetch_energyzero_public_prices(today_nl, "ENERGY_TYPE_GAS", "INTERVAL_DAY")
    gas_tomorrow = fetch_energyzero_public_prices(tomorrow_nl, "ENERGY_TYPE_GAS", "INTERVAL_DAY")

    # Gebruik 'baseWithVat' (kale beursprijs incl. 21% BTW) en tel daar de opslagen uit configEnergy.cfg bij op
    raw_elec_entries = elec_today.get("baseWithVat", []) + elec_tomorrow.get("baseWithVat", [])
    raw_gas_entries = gas_today.get("baseWithVat", []) + gas_tomorrow.get("baseWithVat", [])

    # Dedupliceer stroomintervallen op starttijd en filter op [vandaag 00:00, overmorgen 00:00)
    elec_by_start: dict[datetime, float] = {}
    for entry in raw_elec_entries:
        dt_start = parse_zulu(entry["start"])
        if start_utc <= dt_start < end_utc:
            elec_by_start[dt_start] = float(entry["price"])

    # Dedupliceer gasprijzen op starttijd en sorteer chronologisch
    gas_by_start: dict[datetime, float] = {}
    for entry in raw_gas_entries:
        dt_start = parse_zulu(entry["start"])
        gas_by_start[dt_start] = float(entry["price"])
    parsed_gas = sorted(gas_by_start.items(), key=lambda x: x)

    imp_extra = tariffs_cfg["import_surcharge_kwh"] + tariffs_cfg["energy_tax_kwh"]
    exp_extra = tariffs_cfg["export_surcharge_kwh"]
    gas_extra = tariffs_cfg["gas_surcharge_m3"] + tariffs_cfg["energy_tax_m3"]

    def get_gas_price_at(dt: datetime) -> float | None:
        active_price = None
        for g_start, g_price in parsed_gas:
            if g_start <= dt:
                active_price = g_price
            else:
                break
        if active_price is None:
            return None
        return round(active_price + gas_extra, 4)

    intervals = []
    for interval_start in sorted(elec_by_start.keys()):
        interval_end = interval_start + timedelta(minutes=15)
        base_price = elec_by_start[interval_start]

        item = {
            "start": format_zulu(interval_start),
            "end": format_zulu(interval_end),
            "importPricePerKwh": round(base_price + imp_extra, 4),
            "exportPricePerKwh": round(base_price + exp_extra, 4),
        }
        gas_price = get_gas_price_at(interval_start)
        if gas_price is not None:
            item["gasPricePerM3"] = gas_price

        intervals.append(item)

    return {
        "schemaVersion": 1,
        "currency": "EUR",
        "intervals": intervals,
    }


# =====================================================================
# Configuratie laden & uitvoeren
def load_config(config_path: Path) -> configparser.ConfigParser:
    if not config_path.exists():
      raise FileNotFoundError(f"Configuratiebestand niet gevonden: {config_path}")
    cfg = configparser.ConfigParser()
    cfg.read(config_path, encoding="utf-8")
    return cfg

def resolve_topic(cfg: configparser.ConfigParser) -> str:
    topic = cfg.get("logger", "topic", fallback="").strip()
    if topic:
        return topic
    hostname = cfg.get("logger", "hostname", fallback="lastdsmrlogger").strip().lower()
    if hostname.endswith(".local"):
        hostname = hostname[:-6]
        return f"lastdsmrlogger/{hostname}/tariffs"
    
def resolve_ca_file(cfg: configparser.ConfigParser, config_dir: Path) -> str:
    ca_raw = cfg.get("mqtt", "ca_file", fallback="").strip()
    if not ca_raw:
        return ""
    ca_path = Path(ca_raw)
    if not ca_path.is_absolute():
        ca_path = (config_dir / ca_path).resolve()
        return str(ca_path)
    
def main():
    script_dir = Path(file).resolve().parent
    default_config = script_dir / "configEnergy.cfg"
    config_path = Path(sys.argv).resolve() if len(sys.argv) > 1 else default_config
    cfg = load_config(config_path)
    tariffs_cfg = {
        "import_surcharge_kwh": cfg.getfloat("tariffs", "import_surcharge_kwh", fallback=0.0242),
        "energy_tax_kwh": cfg.getfloat("tariffs", "energy_tax_kwh", fallback=0.1316),
        "export_surcharge_kwh": cfg.getfloat("tariffs", "export_surcharge_kwh", fallback=0.0),
        "gas_surcharge_m3": cfg.getfloat("tariffs", "gas_surcharge_m3", fallback=0.0749),
        "energy_tax_m3": cfg.getfloat("tariffs", "energy_tax_m3", fallback=0.7054),
    }
    schedule = build_price_schedule(tariffs_cfg)
    if not schedule["intervals"]:
       raise RuntimeError("Geen prijsintervallen ontvangen van de EnergyZero API.")
    payload_json = json.dumps(schedule, separators=(",", ":"))
    mqtt_host = cfg.get("mqtt", "host").strip()
    mqtt_port = cfg.getint("mqtt", "port", fallback=1883)
    mqtt_user = cfg.get("mqtt", "username", fallback="").strip()
    mqtt_pass = cfg.get("mqtt", "password", fallback="").strip()
    mqtt_tls = cfg.getboolean("mqtt", "tls", fallback=False)
    ca_file = resolve_ca_file(cfg, config_path.parent)
    topic = resolve_topic(cfg)

    client_id = f"tariff-pub-{os.getpid()}"
    mqtt = MinimalMQTTClient(client_id=client_id, username=mqtt_user, password=mqtt_pass)

try:
    mqtt.connect(host=mqtt_host, port=mqtt_port, use_tls=mqtt_tls, ca_file=ca_file)
    mqtt.publish(topic=topic, message=payload_json, retain=True)
    print(f"Succesvol {len(schedule['intervals'])} intervallen gepubliceerd naar '{topic}' op {mqtt_host}:{mqtt_port}")
finally:
    mqtt.disconnect()
    
    if name == "main":
        main()