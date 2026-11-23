#!/usr/bin/env python3
"""Convert a legacy lastDSMRlogger RING CSV using interactive prompts."""

import csv
import math
import re
import readline
from datetime import datetime
from decimal import Decimal
from pathlib import Path


COUNTER_COLUMNS = (
    ("EDT1", "1-0:1.8.1"),
    ("EDT2", "1-0:1.8.2"),
    ("ERT1", "1-0:2.8.1"),
    ("ERT2", "1-0:2.8.2"),
    ("GDT", "0-1:24.1.0", "0-1:24.2.1", "0-1:24.2.3"),
)
TARIFF_COLUMNS = ("ti", "te", "tg")
KEY_NAMES = ("YYMM", "YYMMDD", "YYMMDDHH")
DECIMAL_PATTERN = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)")


def parseNumber(text):
    if not DECIMAL_PATTERN.fullmatch(text):
        raise ValueError(f"invalid decimal value {text!r}")
    number = Decimal(text)
    if not math.isfinite(float(number)) or abs(number) > Decimal("3.402823466e38"):
        raise ValueError(f"value outside the logger's numeric range: {text!r}")
    return number


def parseKey(key):
    if len(key) not in (4, 6, 8) or not re.fullmatch(r"[0-9]+", key):
        raise ValueError(f"invalid period key {key!r}")
    year = 2000 + int(key[:2])
    month = int(key[2:4])
    day = int(key[4:6]) if len(key) >= 6 else 1
    hour = int(key[6:]) if len(key) == 8 else 0
    return datetime(year, month, day, hour)


def readRing(path):
    with path.open(encoding="utf-8-sig", newline="") as source:
        reader = csv.reader(source, delimiter=";", strict=True)
        header = next((row for row in reader if any(cell.strip() for cell in row)), [])
        if not header or header[0].strip().upper() not in KEY_NAMES:
            raise ValueError("expected a header starting with YYMMDDHH, YYMMDD or YYMM")

        names = [cell.strip().split("*", 1)[0].lower() for cell in header]
        counterIndexes = []
        for aliases in COUNTER_COLUMNS:
            matches = [i for i, name in enumerate(names) if name in {a.lower() for a in aliases}]
            if len(matches) != 1:
                raise ValueError(f"expected exactly one column for {aliases[0]}")
            counterIndexes.append(matches[0])

        tariffIndexes = []
        for name in TARIFF_COLUMNS:
            if names.count(name) > 1:
                raise ValueError(f"duplicate header column {name!r}")
            tariffIndexes.append(names.index(name) if name in names else None)

        knownIndexes = {0, *counterIndexes, *tariffIndexes}
        records = []
        seen = set()
        for row in reader:
            if not any(cell.strip() for cell in row):
                continue
            row = [cell.strip() for cell in row]
            try:
                timestamp = parseKey(row[0])
                if timestamp in seen:
                    raise ValueError(f"duplicate period key {row[0]!r}")
                if len(row) <= max(counterIndexes):
                    raise ValueError("missing meter reading columns")
                fields = [row[i] for i in counterIndexes]
                readings = [parseNumber(field) for field in fields]
                if any(value < 0 for value in readings):
                    raise ValueError("negative cumulative meter reading")
                if any(value and i not in knownIndexes for i, value in enumerate(row)):
                    raise ValueError("unexpected data in an unnamed or unsupported column")
                tariffs = [row[i] if i is not None and i < len(row) else "" for i in tariffIndexes]
                for value in filter(None, tariffs):
                    parseNumber(value)
            except ValueError as error:
                raise ValueError(f"line {reader.line_num}: {error}") from error
            seen.add(timestamp)
            records.append((timestamp, row[0], fields, readings, tariffs))

    if not records:
        raise ValueError("no data records found")
    records.sort(key=lambda record: record[0])
    return header[0].strip().upper(), records


def askValue(name, label):
    while True:
        value = input(f"{name} ({label}): ").strip().replace(",", ".")
        try:
            parseNumber(value)
        except ValueError as error:
            print(f"Enter a decimal number. {error}.")
        else:
            return value


def matchingPaths(text):
    if text == "~":
        return ["~/"]

    prefix, _, partial = text.rpartition("/")
    directory = Path(prefix + "/").expanduser() if "/" in text else Path(".")
    start = text[:len(text) - len(partial)]
    try:
        matches = []
        for entry in directory.iterdir():
            if not entry.name.startswith(partial):
                continue
            if entry.name.startswith(".") and not partial.startswith("."):
                continue
            suffix = "/" if entry.is_dir() else ""
            matches.append(start + entry.name + suffix)
        return sorted(matches)
    except (OSError, RuntimeError, ValueError):
        return []


def askPath(prompt):
    matches = []

    def completePath(text, state):
        nonlocal matches
        if state == 0:
            end = readline.get_endidx()
            prefix = readline.get_line_buffer()[:end - len(text)]
            matches = [path[len(prefix):] for path in matchingPaths(prefix + text)]
        return matches[state] if state < len(matches) else None

    previousCompleter = readline.get_completer()
    previousDelimiters = readline.get_completer_delims()
    readline.set_completer(completePath)
    readline.set_completer_delims("")
    try:
        while True:
            name = input(prompt).strip()
            if name:
                return Path(name).expanduser()
            print("Enter a file name.")
    finally:
        readline.set_completer(previousCompleter)
        readline.set_completer_delims(previousDelimiters)


def main():
    if "libedit" in (readline.__doc__ or "").lower():
        readline.parse_and_bind("bind ^I rl_complete")
    else:
        readline.parse_and_bind("tab: complete")
    source = askPath("Old RING file: ")
    destination = askPath("New RING file: ")
    if source.resolve() == destination.resolve():
        raise ValueError("choose a different name for the new file")
    if destination.exists():
        raise FileExistsError(f"output already exists: {destination}")

    keyName, records = readRing(source)
    hasDecrease = False
    for previous, current in zip(records, records[1:]):
        for aliases, before, after in zip(COUNTER_COLUMNS, previous[3], current[3]):
            if after < before:
                hasDecrease = True
                print(f"Warning: {aliases[0]} decreases from {before} to {after} "
                      f"between {previous[1]} and {current[1]}.")
    if hasDecrease and input("Continue with these readings unchanged? [y/N] ").strip().lower() != "y":
        print("Cancelled. No output file was created.")
        return 1

    labels = ("electricity import", "electricity export", "gas")
    if any(not value for record in records for value in record[4]):
        print("Enter values for missing fields. The logger stores these fields as euro amounts.")
    defaults = [
        askValue(name, label) if any(not record[4][i] for record in records) else ""
        for i, (name, label) in enumerate(zip(TARIFF_COLUMNS, labels))
    ]

    with destination.open("x", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, delimiter=";")
        writer.writerow([keyName, *(aliases[1] for aliases in COUNTER_COLUMNS), *TARIFF_COLUMNS])
        for _, key, fields, _, tariffs in records:
            writer.writerow([key, *fields, *(value or default for value, default in zip(tariffs, defaults))])
    print(f"Wrote {len(records)} records to {destination}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, csv.Error) as error:
        raise SystemExit(f"Error: {error}")
    except (EOFError, KeyboardInterrupt):
        raise SystemExit("\nCancelled.")
