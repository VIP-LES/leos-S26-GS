"""File watcher that reads new telemetry lines and POSTs them to the Ingest API."""

import os
import time
import logging

import requests

from src.parser import parse_line

logger = logging.getLogger(__name__)


def start_watching(api_url: str, file_path: str) -> None:
    """Block forever, watching file_path and POSTing parsed lines to api_url.

    Uses a simple polling loop with os.stat() to detect new data.
    Handles file truncation (e.g., when the file is overwritten).
    Buffers partial lines across reads.
    """
    # Ensure the file exists.
    if not os.path.exists(file_path):
        with open(file_path, "w") as f:
            pass

    last_size = os.path.getsize(file_path)
    line_buffer = ""

    # Process any data already in the file on startup.
    if last_size > 0:
        logger.info("Processing %d bytes of existing data...", last_size)
        with open(file_path, "r", encoding="utf-8") as f:
            chunk = f.read()
        line_buffer += chunk
        lines = line_buffer.split("\n")
        line_buffer = lines.pop()  # keep incomplete last line in buffer
        for line in lines:
            _process_line(line, api_url)

    logger.info("Watching %s for new launch data lines...", file_path)

    while True:
        try:
            current_size = os.path.getsize(file_path)

            # File was truncated — reset.
            if current_size < last_size:
                logger.info("File truncated, resetting position")
                last_size = 0
                line_buffer = ""

            # No new data.
            if current_size == last_size:
                time.sleep(0.1)
                continue

            # Read only the new bytes.
            with open(file_path, "r", encoding="utf-8") as f:
                f.seek(last_size)
                chunk = f.read(current_size - last_size)

            line_buffer += chunk
            lines = line_buffer.split("\n")
            line_buffer = lines.pop()  # keep incomplete last line in buffer

            for line in lines:
                _process_line(line, api_url)

            last_size = current_size

        except KeyboardInterrupt:
            raise
        except Exception as e:
            logger.error("Watcher error: %s", e)
            time.sleep(1)


def _process_line(line: str, api_url: str) -> None:
    """Parse a line and POST it to the ingest API if valid."""
    parsed = parse_line(line)
    if not parsed:
        return

    logger.info("New entry detected: %s", line.strip())
    try:
        resp = requests.post(f"{api_url}/ingest", json=parsed, timeout=5)
        if resp.status_code in (200, 201):
            logger.info(
                "Ingested row at time %s (%d)", parsed["time"], resp.status_code
            )
        else:
            logger.warning("Ingest API returned %d: %s", resp.status_code, resp.text)
    except requests.RequestException as e:
        logger.error("POST to ingest API failed: %s", e)
