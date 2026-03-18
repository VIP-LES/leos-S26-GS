"""Entry point for Pi-mode forwarding and lab-mode ingest."""

import os
import threading
import logging

from dotenv import load_dotenv

load_dotenv()

import uvicorn  # noqa: E402 — must import after load_dotenv

from src import db  # noqa: E402
from src.fake_radio import run_fake_radio  # noqa: E402
from src.forwarder import run_forwarder  # noqa: E402
from src.ingest_api import app  # noqa: E402
from src.queue_writer import init_queue  # noqa: E402

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


def main():
    mode = os.environ.get("GROUNDSTATION_MODE", "lab").strip().lower()
    if mode == "lab":
        run_lab_mode()
        return
    if mode == "pi":
        run_pi_mode()
        return
    raise ValueError(f"Unsupported GROUNDSTATION_MODE: {mode}")


def run_lab_mode() -> None:
    port = int(os.environ.get("INGEST_PORT", "4000"))
    db.connect()
    db.ensure_schema()
    try:
        uvicorn.run(app, host="0.0.0.0", port=port, log_level="info")
    finally:
        db.close()


def run_pi_mode() -> None:
    queue_path = os.environ.get("SPOOL_DB_PATH", "./spool.db")
    ingest_url = os.environ.get("INGEST_URL", "http://localhost:4000")
    fake_input_path = os.environ.get("FAKE_RADIO_INPUT_PATH")
    fake_repeat = os.environ.get("FAKE_RADIO_REPEAT", "false").lower() == "true"
    fake_interval_s = float(os.environ.get("FAKE_RADIO_INTERVAL_S", "0"))

    init_queue(queue_path)

    if fake_input_path:
        fake_thread = threading.Thread(
            target=run_fake_radio,
            kwargs={
                "queue_path": queue_path,
                "input_path": fake_input_path,
                "repeat": fake_repeat,
                "interval_s": fake_interval_s,
            },
            daemon=True,
        )
        fake_thread.start()
        logger.info("Fake radio replay started from %s", fake_input_path)
    else:
        logger.info("Pi mode started without fake radio input; forwarder will idle")

    run_forwarder(queue_path, ingest_url)

if __name__ == "__main__":
    main()
