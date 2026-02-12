"""Parse a single line of telemetry text into a dict."""

import re
from datetime import datetime


def parse_line(raw: str) -> dict | None:
    """Parse a telemetry text line into a dict, or None if invalid.

    Expected format (comma-separated, first field is a timestamp):
        Wed Feb 12 14:30:00 2026, T:24.56, P:99401.91, UV:2, ...
    """
    if not raw or not raw.strip():
        return None

    # Split on the first comma to separate timestamp from key:value fields.
    first_comma = raw.find(",")
    if first_comma == -1:
        ts_string = raw.strip()
        tail = ""
    else:
        ts_string = raw[:first_comma].strip()
        tail = raw[first_comma + 1 :]

    # Parse the timestamp as local time (matches JS `new Date()` behavior).
    # The timestamp from the radio decoder has no timezone info, so we treat
    # it as the system's local time — same as the old Node.js script did.
    try:
        time = datetime.strptime(ts_string, "%a %b %d %H:%M:%S %Y")
        time = time.astimezone()  # attach the system's local timezone
    except ValueError:
        # Fallback: let Python try to parse it as ISO or other format.
        try:
            time = datetime.fromisoformat(ts_string)
            if time.tzinfo is None:
                time = time.astimezone()
        except ValueError:
            print(f"Skipping line with bad timestamp: {raw}")
            return None

    def take_number(pattern: str) -> float | None:
        m = re.search(pattern, tail, re.IGNORECASE)
        return float(m.group(1)) if m else None

    temp = take_number(r"T:\s*([-+\d.]+)")
    pressure = take_number(r"P:\s*([-+\d.]+)")
    uv_raw = take_number(r"UV:\s*(\d+)")
    aqi_pm100_us = take_number(r"aqi_pm100_us:\s*([-+\d.]+)")
    aqi_pm25_us = take_number(r"aqi_pm25_us:\s*([-+\d.]+)")
    pm100_env = take_number(r"pm100_env:\s*([-+\d.]+)")
    pm25_env = take_number(r"pm25_env:\s*([-+\d.]+)")
    pm10_env = take_number(r"pm10_env:\s*([-+\d.]+)")

    # Legacy fallback: A:(x, y) maps to aqi_pm100_us / aqi_pm25_us.
    if aqi_pm100_us is None or aqi_pm25_us is None:
        legacy = re.search(
            r"A:\s*\(\s*([-+\d.]+)\s*,\s*([-+\d.]+)\s*\)", tail, re.IGNORECASE
        )
        if legacy:
            if aqi_pm100_us is None:
                aqi_pm100_us = float(legacy.group(1))
            if aqi_pm25_us is None:
                aqi_pm25_us = float(legacy.group(2))

    uv = int(uv_raw) if uv_raw is not None else None

    return {
        "time": time.isoformat(),
        "temp": temp,
        "pressure": pressure,
        "aqi_pm100_us": aqi_pm100_us,
        "aqi_pm25_us": aqi_pm25_us,
        "pm100_env": pm100_env,
        "pm25_env": pm25_env,
        "pm10_env": pm10_env,
        "uv": uv,
    }
