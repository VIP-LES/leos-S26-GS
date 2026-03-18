"""Fake/replay telemetry source for Pi-mode testing without hardware."""

from __future__ import annotations

import json
import logging
import time

from src import queue_writer
from src.telemetry_decoder import decode_hex_frame, decode_rf_frame, encode_rf_frame

logger = logging.getLogger(__name__)


def _iter_records(input_path: str):
    with open(input_path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            yield line


def _decode_record(line: str) -> tuple[str, dict]:
    if line.startswith("{"):
        record = json.loads(line)
        if "frame_hex" in record:
            return decode_hex_frame(record["frame_hex"])

        message_type = record["message_type"]
        payload = record["payload"]
        frame = encode_rf_frame(message_type, payload, sequence=record.get("sequence", 0))
        return decode_rf_frame(frame)

    return decode_hex_frame(line)


def run_fake_radio(
    queue_path: str,
    input_path: str,
    *,
    repeat: bool = False,
    interval_s: float = 0.0,
) -> None:
    while True:
        for line in _iter_records(input_path):
            message_type, payload = _decode_record(line)
            queue_writer.enqueue(queue_path, message_type, payload)
            logger.info("Queued fake %s payload at %s", message_type, payload["time"])
            if interval_s > 0:
                time.sleep(interval_s)
        if not repeat:
            return
