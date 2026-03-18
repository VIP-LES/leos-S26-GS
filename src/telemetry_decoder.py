"""RF frame encode/decode helpers shared by fake input and future radio RX."""

from __future__ import annotations

import math
import struct
from datetime import datetime, timezone

from src.models import EFM, SENSORS_AND_GPS

SYNC_BYTE = 0xA5
PROTOCOL_VERSION = 1

RF_MESSAGE_SENSOR_GPS = 0x01
RF_MESSAGE_EFM = 0x02
RF_MESSAGE_COMMAND = 0x03

MESSAGE_TYPE_TO_QUEUE = {
    RF_MESSAGE_SENSOR_GPS: SENSORS_AND_GPS,
    RF_MESSAGE_EFM: EFM,
}

QUEUE_TO_MESSAGE_TYPE = {value: key for key, value in MESSAGE_TYPE_TO_QUEUE.items()}

SENSORS_AND_GPS_FIELDS = (
    "temperature",
    "pressure",
    "pm10_env",
    "pm25_env",
    "pm100_env",
    "aqi_pm25_us",
    "aqi_pm100_us",
    "uvi",
    "light_lux",
    "humidity",
)
EFM_FIELDS = (
    "adc1_ch1",
    "adc1_ch2",
    "adc1_ch3",
    "adc1_ch4",
    "adc2_ch1",
    "adc2_ch2",
    "adc2_ch3",
    "adc2_ch4",
)

SENSORS_AND_GPS_STRUCT = struct.Struct("<Q10f")
EFM_STRUCT = struct.Struct("<Q8f")


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _time_to_millis(time_str: str) -> int:
    dt = datetime.fromisoformat(time_str)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1000)


def _millis_to_iso(timestamp_ms: int) -> str:
    dt = datetime.fromtimestamp(timestamp_ms / 1000, tz=timezone.utc)
    return dt.isoformat()


def _value_for_pack(value: float | None) -> float:
    return float("nan") if value is None else float(value)


def _value_for_unpack(value: float) -> float | None:
    return None if math.isnan(value) else value


def _build_payload_bytes(message_type: str, payload: dict) -> bytes:
    if message_type == SENSORS_AND_GPS:
        values = [_time_to_millis(payload["time"])]
        values.extend(_value_for_pack(payload.get(field)) for field in SENSORS_AND_GPS_FIELDS)
        return SENSORS_AND_GPS_STRUCT.pack(*values)
    if message_type == EFM:
        values = [_time_to_millis(payload["time"])]
        values.extend(_value_for_pack(payload.get(field)) for field in EFM_FIELDS)
        return EFM_STRUCT.pack(*values)
    raise ValueError(f"Unsupported message type: {message_type}")


def encode_rf_frame(message_type: str, payload: dict, sequence: int = 0) -> bytes:
    rf_message_type = QUEUE_TO_MESSAGE_TYPE[message_type]
    payload_bytes = _build_payload_bytes(message_type, payload)
    header = bytes(
        (
            SYNC_BYTE,
            PROTOCOL_VERSION,
            rf_message_type,
            sequence & 0xFF,
            len(payload_bytes),
        )
    )
    crc = crc16_ccitt_false(header + payload_bytes)
    return header + payload_bytes + struct.pack(">H", crc)


def _decode_payload(rf_message_type: int, payload_bytes: bytes) -> tuple[str, dict]:
    if rf_message_type == RF_MESSAGE_SENSOR_GPS:
        if len(payload_bytes) != SENSORS_AND_GPS_STRUCT.size:
            raise ValueError("Invalid sensors_and_gps payload length")
        unpacked = SENSORS_AND_GPS_STRUCT.unpack(payload_bytes)
        payload = {"time": _millis_to_iso(unpacked[0])}
        for field, value in zip(SENSORS_AND_GPS_FIELDS, unpacked[1:]):
            payload[field] = _value_for_unpack(value)
        return SENSORS_AND_GPS, payload

    if rf_message_type == RF_MESSAGE_EFM:
        if len(payload_bytes) != EFM_STRUCT.size:
            raise ValueError("Invalid efm payload length")
        unpacked = EFM_STRUCT.unpack(payload_bytes)
        payload = {"time": _millis_to_iso(unpacked[0])}
        for field, value in zip(EFM_FIELDS, unpacked[1:]):
            payload[field] = _value_for_unpack(value)
        return EFM, payload

    raise ValueError(f"Unsupported RF message type: {rf_message_type:#x}")


def decode_rf_frame(frame: bytes) -> tuple[str, dict]:
    if len(frame) < 7:
        raise ValueError("Frame too short")
    if frame[0] != SYNC_BYTE:
        raise ValueError("Invalid sync byte")
    if frame[1] != PROTOCOL_VERSION:
        raise ValueError("Unsupported protocol version")

    payload_length = frame[4]
    expected_len = 5 + payload_length + 2
    if len(frame) != expected_len:
        raise ValueError("Frame length does not match payload_length")

    frame_without_crc = frame[:-2]
    received_crc = struct.unpack(">H", frame[-2:])[0]
    expected_crc = crc16_ccitt_false(frame_without_crc)
    if received_crc != expected_crc:
        raise ValueError("CRC mismatch")

    return _decode_payload(frame[2], frame[5:-2])


def decode_hex_frame(frame_hex: str) -> tuple[str, dict]:
    return decode_rf_frame(bytes.fromhex(frame_hex.strip()))
