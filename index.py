"""Entry point: starts the Ingest API server and file watcher."""

import os
import signal
import threading
import logging

from dotenv import load_dotenv

load_dotenv()

import uvicorn  # noqa: E402 — must import after load_dotenv

from src import db  # noqa: E402
from src.server import app  # noqa: E402
from src.watcher import start_watching  # noqa: E402

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


def main():
    port = int(os.environ.get("INGEST_PORT", "4000"))
    api_url = os.environ.get("INGEST_URL", f"http://localhost:{port}")
    file_path = os.environ.get("LAUNCH_DATA_FILE", "./launch_data.txt")

    # Connect to database and ensure schema exists.
    db.connect()
    db.ensure_schema()

    # Start FastAPI/uvicorn in a background daemon thread.
    config = uvicorn.Config(app, host="0.0.0.0", port=port, log_level="info")
    server = uvicorn.Server(config)
    api_thread = threading.Thread(target=server.run, daemon=True)
    api_thread.start()
    logger.info("Ingest API starting on port %d", port)

    # Start file watcher on the main thread (blocks forever).
    try:
        start_watching(api_url, file_path)
    except KeyboardInterrupt:
        pass
    finally:
        logger.info("Shutting down...")
        server.should_exit = True
        api_thread.join(timeout=5)
        db.close()


def _shutdown_handler(signum, frame):
    raise KeyboardInterrupt


signal.signal(signal.SIGINT, _shutdown_handler)
signal.signal(signal.SIGTERM, _shutdown_handler)

if __name__ == "__main__":
    main()
