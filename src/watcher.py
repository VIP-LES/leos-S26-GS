"""File watcher that reads new telemetry lines and POSTs them to the Ingest API.

Implements offline resilience via a persistent cursor file that tracks the byte
offset of the last successfully sent line.  On network/server failure the watcher
retries with exponential back-off (capped at 30 s).  On restart after a power
loss the cursor is read from disk and processing resumes from that point; the
server's ON CONFLICT DO NOTHING guarantees idempotent writes for any duplicates.
"""

import os
import time
import logging
import tempfile

import requests

from src.parser import parse_line

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Retry / back-off constants
# ---------------------------------------------------------------------------
_INITIAL_BACKOFF_S = 1
_MAX_BACKOFF_S = 30
_BACKOFF_FACTOR = 2

# ---------------------------------------------------------------------------
# Cursor helpers
# ---------------------------------------------------------------------------


def _read_cursor(cursor_path: str) -> int:
    """Return the byte offset stored in *cursor_path*, or 0 if missing/corrupt."""
    try:
        with open(cursor_path, "r") as f:
            return int(f.read().strip())
    except (FileNotFoundError, ValueError, OSError):
        return 0


def _write_cursor(cursor_path: str, offset: int) -> None:
    """Atomically persist *offset* to the cursor file.

    Writes to a temporary file in the same directory then uses ``os.replace``
    so the cursor is never left in a half-written state (safe across power loss
    on most filesystems).
    """
    dir_name = os.path.dirname(cursor_path) or "."
    try:
        fd, tmp_path = tempfile.mkstemp(dir=dir_name, prefix=".cursor_")
        try:
            os.write(fd, str(offset).encode())
        finally:
            os.close(fd)
        os.replace(tmp_path, cursor_path)
    except OSError as e:
        logger.error("Failed to write cursor file %s: %s", cursor_path, e)


# ---------------------------------------------------------------------------
# Sending helpers
# ---------------------------------------------------------------------------


def _send_line(parsed: dict, api_url: str) -> str:
    """POST *parsed* to the ingest API.

    Returns
    -------
    ``"ok"``    – 2xx response, line was accepted (inserted or duplicate).
    ``"skip"``  – 4xx response, line is malformed and will never succeed.
    ``"retry"`` – 5xx / network error, should be retried later.
    """
    try:
        resp = requests.post(f"{api_url}/ingest", json=parsed, timeout=5)
        if resp.status_code in (200, 201):
            logger.info(
                "Ingested row at time %s (%d)", parsed["time"], resp.status_code
            )
            return "ok"
        elif 400 <= resp.status_code < 500:
            logger.warning(
                "Ingest API returned %d (skipping): %s",
                resp.status_code,
                resp.text,
            )
            return "skip"
        else:
            logger.warning(
                "Ingest API returned %d (will retry): %s",
                resp.status_code,
                resp.text,
            )
            return "retry"
    except requests.RequestException as e:
        logger.error("POST to ingest API failed (will retry): %s", e)
        return "retry"


def _send_with_retry(parsed: dict, api_url: str) -> None:
    """Send *parsed* to the ingest API, retrying indefinitely on transient errors.

    Uses exponential back-off: 1 s → 2 s → 4 s → … → 30 s (cap).
    Returns once the line is accepted or permanently skipped (4xx).
    """
    result = _send_line(parsed, api_url)
    if result != "retry":
        return

    backoff = _INITIAL_BACKOFF_S
    attempt = 1
    while True:
        logger.info("Retry #%d for time %s in %ds …", attempt, parsed["time"], backoff)
        time.sleep(backoff)
        result = _send_line(parsed, api_url)
        if result != "retry":
            if attempt > 0:
                logger.info(
                    "Succeeded after %d retries for time %s", attempt, parsed["time"]
                )
            return
        backoff = min(backoff * _BACKOFF_FACTOR, _MAX_BACKOFF_S)
        attempt += 1


# ---------------------------------------------------------------------------
# Main watcher loop
# ---------------------------------------------------------------------------


def start_watching(api_url: str, file_path: str, cursor_path: str) -> None:
    """Block forever, watching *file_path* and POSTing parsed lines to *api_url*.

    Maintains a cursor file at *cursor_path* that records the byte offset of
    the last successfully sent line.  On restart the watcher resumes from that
    offset so no data is lost across power / network outages.
    """
    # Ensure the data file exists.
    if not os.path.exists(file_path):
        with open(file_path, "w") as f:
            pass

    confirmed_offset = _read_cursor(cursor_path)
    file_size = os.path.getsize(file_path)

    # If the cursor is beyond the current file size the file was likely
    # recreated — reset and reprocess from the beginning.
    if confirmed_offset > file_size:
        logger.info(
            "Cursor (%d) is past file size (%d); resetting to 0",
            confirmed_offset,
            file_size,
        )
        confirmed_offset = 0
        _write_cursor(cursor_path, 0)

    # Process any un-sent data that is already in the file.
    if confirmed_offset < file_size:
        logger.info(
            "Catching up: processing bytes %d → %d …",
            confirmed_offset,
            file_size,
        )
        confirmed_offset = _process_chunk(
            file_path, confirmed_offset, file_size, api_url, cursor_path
        )

    logger.info("Watching %s for new launch data lines …", file_path)

    last_size = file_size
    line_buffer = ""

    while True:
        try:
            current_size = os.path.getsize(file_path)

            # File was truncated / recreated — reset.
            if current_size < last_size:
                logger.info("File truncated, resetting cursor to 0")
                confirmed_offset = 0
                _write_cursor(cursor_path, 0)
                line_buffer = ""

            if current_size == confirmed_offset and line_buffer == "":
                # Nothing new.
                last_size = current_size
                time.sleep(0.1)
                continue

            # Read new bytes from the confirmed offset (to handle any
            # partial buffer left over).
            read_from = confirmed_offset + len(line_buffer.encode("utf-8"))
            if current_size > read_from:
                with open(file_path, "r", encoding="utf-8") as f:
                    f.seek(read_from)
                    chunk = f.read(current_size - read_from)
                line_buffer += chunk

            # Split into complete lines.
            lines = line_buffer.split("\n")
            line_buffer = lines.pop()  # keep incomplete trailing data

            for line in lines:
                line_byte_len = len(line.encode("utf-8")) + 1  # +1 for \n
                parsed = parse_line(line)
                if parsed:
                    _send_with_retry(parsed, api_url)
                # Advance cursor past this line whether parsed or not
                # (blank / comment lines should not block progress).
                confirmed_offset += line_byte_len
                _write_cursor(cursor_path, confirmed_offset)

            last_size = current_size

        except KeyboardInterrupt:
            raise
        except Exception as e:
            logger.error("Watcher error: %s", e)
            time.sleep(1)


def _process_chunk(
    file_path: str,
    start_offset: int,
    end_offset: int,
    api_url: str,
    cursor_path: str,
) -> int:
    """Read bytes [start_offset, end_offset) from *file_path*, parse and send
    each complete line, and return the new confirmed offset."""
    with open(file_path, "r", encoding="utf-8") as f:
        f.seek(start_offset)
        data = f.read(end_offset - start_offset)

    lines = data.split("\n")
    # The last element is either an empty string (file ends with \n) or an
    # incomplete line — either way we leave it for the live-tail loop.
    remainder = lines.pop()

    offset = start_offset
    for line in lines:
        line_byte_len = len(line.encode("utf-8")) + 1  # +1 for \n
        parsed = parse_line(line)
        if parsed:
            _send_with_retry(parsed, api_url)
        offset += line_byte_len
        _write_cursor(cursor_path, offset)

    return offset
