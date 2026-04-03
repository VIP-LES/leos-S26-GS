"""RF frame encode/decode helpers shared by fake input and future radio RX."""

from __future__ import annotations

import struct
from datetime import datetime, timezone

from src.models import EFM, SENSORS_AND_GPS

SYNC_BYTE = 0xA5
PROTOCOL_VERSION = 1

RF_MESSAGE_SENSOR_GPS = 0x01
RF_MESSAGE_EFM = 0x02
RF_MESSAGE_COMMAND = 0x03

RADIO_SENSOR_GPS_PAYLOAD_SIZE = 131
RADIO_EFM_PAYLOAD_SIZE = 41

MESSAGE_TYPE_TO_QUEUE = {
    RF_MESSAGE_SENSOR_GPS: SENSORS_AND_GPS,
    RF_MESSAGE_EFM: EFM,
}

QUEUE_TO_MESSAGE_TYPE = {value: key for key, value in MESSAGE_TYPE_TO_QUEUE.items()}


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


def _iso_to_us(time_str: str) -> int:
    dt = datetime.fromisoformat(time_str)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1_000_000)


def _us_to_iso(timestamp_us: int) -> str:
    dt = datetime.fromtimestamp(timestamp_us / 1_000_000, tz=timezone.utc)
    return dt.isoformat()


def _optional_us_to_iso(timestamp_us: int) -> str | None:
    if timestamp_us == 0:
        return None
    return _us_to_iso(timestamp_us)


def _payload_time_to_us(payload: dict) -> int:
    if "t_pkt_us" in payload:
        return int(payload["t_pkt_us"])
    if "time" in payload:
        return _iso_to_us(payload["time"])
    raise ValueError("Payload must include t_pkt_us or time")


def _gps_time_to_us(gps_payload: dict) -> int:
    if "gps_utc_us" in gps_payload:
        return int(gps_payload["gps_utc_us"])
    if "gps_utc" in gps_payload and gps_payload["gps_utc"] is not None:
        return _iso_to_us(gps_payload["gps_utc"])
    return 0


def _read_u8(payload: bytes, offset: int) -> tuple[int, int]:
    return payload[offset], offset + 1


def _read_bool(payload: bytes, offset: int) -> tuple[bool, int]:
    value, offset = _read_u8(payload, offset)
    if value not in (0, 1):
        raise ValueError(f"Invalid bool value: {value}")
    return bool(value), offset


def _read_u16_le(payload: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<H", payload, offset)[0], offset + 2


def _read_u32_le(payload: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<I", payload, offset)[0], offset + 4


def _read_u64_le(payload: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<Q", payload, offset)[0], offset + 8


def _read_float_le(payload: bytes, offset: int) -> tuple[float, int]:
    return struct.unpack_from("<f", payload, offset)[0], offset + 4


def _read_double_le(payload: bytes, offset: int) -> tuple[float, int]:
    return struct.unpack_from("<d", payload, offset)[0], offset + 8


def _write_u8(buf: bytearray, value: int) -> None:
    buf.extend(struct.pack("<B", value))


def _write_bool(buf: bytearray, value: bool) -> None:
    _write_u8(buf, 1 if value else 0)


def _write_u16_le(buf: bytearray, value: int) -> None:
    buf.extend(struct.pack("<H", value))


def _write_u32_le(buf: bytearray, value: int) -> None:
    buf.extend(struct.pack("<I", value))


def _write_u64_le(buf: bytearray, value: int) -> None:
    buf.extend(struct.pack("<Q", value))


def _write_float_le(buf: bytearray, value: float) -> None:
    buf.extend(struct.pack("<f", value))


def _write_double_le(buf: bytearray, value: float) -> None:
    buf.extend(struct.pack("<d", value))


def _decode_sensor_gps_payload(payload_bytes: bytes) -> dict:
    if len(payload_bytes) != RADIO_SENSOR_GPS_PAYLOAD_SIZE:
        raise ValueError("Invalid sensors_and_gps payload length")

    offset = 0
    payload: dict = {}

    payload["t_pkt_us"], offset = _read_u64_le(payload_bytes, offset)

    bme688 = {}
    bme688["humidity"], offset = _read_float_le(payload_bytes, offset)
    bme688["pressure"], offset = _read_float_le(payload_bytes, offset)
    bme688["temperature"], offset = _read_float_le(payload_bytes, offset)
    bme688["altitude"], offset = _read_float_le(payload_bytes, offset)
    bme688["gas_resistance"], offset = _read_float_le(payload_bytes, offset)
    payload["bme688_valid"], offset = _read_bool(payload_bytes, offset)
    payload["bme688"] = bme688

    tsl2591 = {}
    tsl2591["light_lux"], offset = _read_float_le(payload_bytes, offset)
    tsl2591["raw_visible"], offset = _read_u16_le(payload_bytes, offset)
    tsl2591["raw_infrared"], offset = _read_u16_le(payload_bytes, offset)
    tsl2591["raw_full_spectrum"], offset = _read_u32_le(payload_bytes, offset)
    payload["tsl2591_valid"], offset = _read_bool(payload_bytes, offset)
    payload["tsl2591"] = tsl2591

    ltr390 = {}
    ltr390["uvs"], offset = _read_u32_le(payload_bytes, offset)
    payload["ltr390_valid"], offset = _read_bool(payload_bytes, offset)
    payload["ltr390"] = ltr390

    pmsa003i = {}
    pmsa003i["pm10_env"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["pm25_env"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["pm100_env"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["aqi_pm25_us"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["aqi_pm100_us"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_03um"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_05um"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_10um"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_25um"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_50um"], offset = _read_u32_le(payload_bytes, offset)
    pmsa003i["particles_100um"], offset = _read_u32_le(payload_bytes, offset)
    payload["pmsa003i_valid"], offset = _read_bool(payload_bytes, offset)
    payload["pmsa003i"] = pmsa003i

    gps = {}
    gps["fix_ok"], offset = _read_bool(payload_bytes, offset)
    gps["lat"], offset = _read_double_le(payload_bytes, offset)
    gps["lon"], offset = _read_double_le(payload_bytes, offset)
    gps["alt_m"], offset = _read_float_le(payload_bytes, offset)
    gps["speed_mps"], offset = _read_float_le(payload_bytes, offset)
    gps["track_deg"], offset = _read_float_le(payload_bytes, offset)
    gps["sats_used"], offset = _read_u8(payload_bytes, offset)
    gps["sats_visible"], offset = _read_u8(payload_bytes, offset)
    gps["gps_utc_us"], offset = _read_u64_le(payload_bytes, offset)
    payload["gps"] = gps

    if offset != RADIO_SENSOR_GPS_PAYLOAD_SIZE:
        raise ValueError("Sensors_and_gps payload did not decode cleanly")

    return payload


def _decode_efm_payload(payload_bytes: bytes) -> dict:
    if len(payload_bytes) != RADIO_EFM_PAYLOAD_SIZE:
        raise ValueError("Invalid efm payload length")

    offset = 0
    payload: dict = {}

    payload["t_pkt_us"], offset = _read_u64_le(payload_bytes, offset)
    payload["valid"], offset = _read_bool(payload_bytes, offset)
    payload["adc1_ch1_diff"], offset = _read_float_le(payload_bytes, offset)
    payload["adc1_ch2_sensing"], offset = _read_float_le(payload_bytes, offset)
    payload["adc1_ch3_reference"], offset = _read_float_le(payload_bytes, offset)
    payload["adc1_ch4_breakbeam"], offset = _read_float_le(payload_bytes, offset)
    payload["adc2_ch1_diff"], offset = _read_float_le(payload_bytes, offset)
    payload["adc2_ch2_sensing"], offset = _read_float_le(payload_bytes, offset)
    payload["adc2_ch3_reference"], offset = _read_float_le(payload_bytes, offset)
    payload["adc2_ch4_breakbeam"], offset = _read_float_le(payload_bytes, offset)

    if offset != RADIO_EFM_PAYLOAD_SIZE:
        raise ValueError("EFM payload did not decode cleanly")

    return payload


def _encode_sensor_gps_payload(payload: dict) -> bytes:
    buf = bytearray()

    _write_u64_le(buf, _payload_time_to_us(payload))

    bme688 = payload["bme688"]
    _write_float_le(buf, bme688["humidity"])
    _write_float_le(buf, bme688["pressure"])
    _write_float_le(buf, bme688["temperature"])
    _write_float_le(buf, bme688["altitude"])
    _write_float_le(buf, bme688["gas_resistance"])
    _write_bool(buf, bool(payload.get("bme688_valid", bme688.get("valid", True))))

    tsl2591 = payload["tsl2591"]
    _write_float_le(buf, tsl2591["light_lux"])
    _write_u16_le(buf, tsl2591["raw_visible"])
    _write_u16_le(buf, tsl2591["raw_infrared"])
    _write_u32_le(buf, tsl2591["raw_full_spectrum"])
    _write_bool(buf, bool(payload.get("tsl2591_valid", tsl2591.get("valid", True))))

    ltr390 = payload["ltr390"]
    _write_u32_le(buf, ltr390["uvs"])
    _write_bool(buf, bool(payload.get("ltr390_valid", ltr390.get("valid", True))))

    pmsa003i = payload["pmsa003i"]
    _write_u32_le(buf, pmsa003i["pm10_env"])
    _write_u32_le(buf, pmsa003i["pm25_env"])
    _write_u32_le(buf, pmsa003i["pm100_env"])
    _write_u32_le(buf, pmsa003i["aqi_pm25_us"])
    _write_u32_le(buf, pmsa003i["aqi_pm100_us"])
    _write_u32_le(buf, pmsa003i["particles_03um"])
    _write_u32_le(buf, pmsa003i["particles_05um"])
    _write_u32_le(buf, pmsa003i["particles_10um"])
    _write_u32_le(buf, pmsa003i["particles_25um"])
    _write_u32_le(buf, pmsa003i["particles_50um"])
    _write_u32_le(buf, pmsa003i["particles_100um"])
    _write_bool(buf, bool(payload.get("pmsa003i_valid", pmsa003i.get("valid", True))))

    gps = payload["gps"]
    _write_bool(buf, gps["fix_ok"])
    _write_double_le(buf, gps["lat"])
    _write_double_le(buf, gps["lon"])
    _write_float_le(buf, gps["alt_m"])
    _write_float_le(buf, gps["speed_mps"])
    _write_float_le(buf, gps["track_deg"])
    _write_u8(buf, gps["sats_used"])
    _write_u8(buf, gps["sats_visible"])
    _write_u64_le(buf, _gps_time_to_us(gps))

    if len(buf) != RADIO_SENSOR_GPS_PAYLOAD_SIZE:
        raise ValueError("Sensors_and_gps payload encoded to unexpected size")

    return bytes(buf)


def _encode_efm_payload(payload: dict) -> bytes:
    buf = bytearray()

    _write_u64_le(buf, _payload_time_to_us(payload))
    _write_bool(buf, bool(payload.get("valid", True)))
    _write_float_le(buf, payload["adc1_ch1_diff"])
    _write_float_le(buf, payload["adc1_ch2_sensing"])
    _write_float_le(buf, payload["adc1_ch3_reference"])
    _write_float_le(buf, payload["adc1_ch4_breakbeam"])
    _write_float_le(buf, payload["adc2_ch1_diff"])
    _write_float_le(buf, payload["adc2_ch2_sensing"])
    _write_float_le(buf, payload["adc2_ch3_reference"])
    _write_float_le(buf, payload["adc2_ch4_breakbeam"])

    if len(buf) != RADIO_EFM_PAYLOAD_SIZE:
        raise ValueError("EFM payload encoded to unexpected size")

    return bytes(buf)


def _build_payload_bytes(message_type: str, payload: dict) -> bytes:
    if message_type == SENSORS_AND_GPS:
        return _encode_sensor_gps_payload(payload)
    if message_type == EFM:
        return _encode_efm_payload(payload)
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
    return header + payload_bytes + struct.pack("<H", crc)


def _decode_payload(rf_message_type: int, payload_bytes: bytes) -> tuple[str, dict]:
    if rf_message_type == RF_MESSAGE_SENSOR_GPS:
        return SENSORS_AND_GPS, _decode_sensor_gps_payload(payload_bytes)

    if rf_message_type == RF_MESSAGE_EFM:
        return EFM, _decode_efm_payload(payload_bytes)

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
    received_crc = struct.unpack("<H", frame[-2:])[0]
    expected_crc = crc16_ccitt_false(frame_without_crc)
    if received_crc != expected_crc:
        raise ValueError("CRC mismatch")

    return _decode_payload(frame[2], frame[5:-2])


def decode_hex_frame(frame_hex: str) -> tuple[str, dict]:
    return decode_rf_frame(bytes.fromhex(frame_hex.strip()))


def rf_payload_to_ingest_payload(message_type: str, payload: dict) -> dict:
    if message_type == SENSORS_AND_GPS:
        gps = payload["gps"]
        bme688_valid = payload["bme688_valid"]
        tsl2591_valid = payload["tsl2591_valid"]
        ltr390_valid = payload["ltr390_valid"]
        pmsa003i_valid = payload["pmsa003i_valid"]

        return {
            "time": _us_to_iso(payload["t_pkt_us"]),
            "gps_utc": _optional_us_to_iso(gps["gps_utc_us"]),
            "temperature": payload["bme688"]["temperature"] if bme688_valid else None,
            "pressure": payload["bme688"]["pressure"] if bme688_valid else None,
            "humidity": payload["bme688"]["humidity"] if bme688_valid else None,
            "altitude": payload["bme688"]["altitude"] if bme688_valid else None,
            "gas_resistance": payload["bme688"]["gas_resistance"] if bme688_valid else None,
            "light_lux": payload["tsl2591"]["light_lux"] if tsl2591_valid else None,
            "raw_visible": payload["tsl2591"]["raw_visible"] if tsl2591_valid else None,
            "raw_infrared": payload["tsl2591"]["raw_infrared"] if tsl2591_valid else None,
            "raw_full_spectrum": payload["tsl2591"]["raw_full_spectrum"] if tsl2591_valid else None,
            "uvs": float(payload["ltr390"]["uvs"]) if ltr390_valid else None,
            "pm10_env": float(payload["pmsa003i"]["pm10_env"]) if pmsa003i_valid else None,
            "pm25_env": float(payload["pmsa003i"]["pm25_env"]) if pmsa003i_valid else None,
            "pm100_env": float(payload["pmsa003i"]["pm100_env"]) if pmsa003i_valid else None,
            "aqi_pm25_us": float(payload["pmsa003i"]["aqi_pm25_us"]) if pmsa003i_valid else None,
            "aqi_pm100_us": float(payload["pmsa003i"]["aqi_pm100_us"]) if pmsa003i_valid else None,
            "particles_03um": float(payload["pmsa003i"]["particles_03um"]) if pmsa003i_valid else None,
            "particles_05um": float(payload["pmsa003i"]["particles_05um"]) if pmsa003i_valid else None,
            "particles_10um": float(payload["pmsa003i"]["particles_10um"]) if pmsa003i_valid else None,
            "particles_25um": float(payload["pmsa003i"]["particles_25um"]) if pmsa003i_valid else None,
            "particles_50um": float(payload["pmsa003i"]["particles_50um"]) if pmsa003i_valid else None,
            "particles_100um": float(payload["pmsa003i"]["particles_100um"]) if pmsa003i_valid else None,
            "gps_fix_ok": gps["fix_ok"],
            "gps_lat": gps["lat"],
            "gps_lon": gps["lon"],
            "gps_alt_m": gps["alt_m"],
            "gps_speed_mps": gps["speed_mps"],
            "gps_track_deg": gps["track_deg"],
            "gps_sats_used": gps["sats_used"],
            "gps_sats_visible": gps["sats_visible"],
        }

    if message_type == EFM:
        valid = payload["valid"]
        return {
            "time": _us_to_iso(payload["t_pkt_us"]),
            "adc1_ch1_diff": payload["adc1_ch1_diff"] if valid else None,
            "adc1_ch2_sensing": payload["adc1_ch2_sensing"] if valid else None,
            "adc1_ch3_reference": payload["adc1_ch3_reference"] if valid else None,
            "adc1_ch4_breakbeam": payload["adc1_ch4_breakbeam"] if valid else None,
            "adc2_ch1_diff": payload["adc2_ch1_diff"] if valid else None,
            "adc2_ch2_sensing": payload["adc2_ch2_sensing"] if valid else None,
            "adc2_ch3_reference": payload["adc2_ch3_reference"] if valid else None,
            "adc2_ch4_breakbeam": payload["adc2_ch4_breakbeam"] if valid else None,
        }

    raise ValueError(f"Unsupported message type: {message_type}")


def decode_rf_frame_to_ingest_payload(frame: bytes) -> tuple[str, dict]:
    message_type, payload = decode_rf_frame(frame)
    return message_type, rf_payload_to_ingest_payload(message_type, payload)


def decode_hex_frame_to_ingest_payload(frame_hex: str) -> tuple[str, dict]:
    return decode_rf_frame_to_ingest_payload(bytes.fromhex(frame_hex.strip()))
