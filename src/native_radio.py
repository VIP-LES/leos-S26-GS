"""Native Pi-side radio source backed by the SX126x receiver process."""

from __future__ import annotations

import logging
import os
import socket
import subprocess
import time
from dataclasses import dataclass

from src import queue_writer
from src.telemetry_decoder import decode_rf_frame_to_ingest_payload

logger = logging.getLogger(__name__)

IPC_MSG_RX_PACKET = 0x01
IPC_MSG_BUTTON_EVENT = 0x02
IPC_MSG_TX_PACKET = 0x03
IPC_MSG_TX_RESULT = 0x04


@dataclass
class RadioReceiverClient:
    sock: socket.socket
    process: subprocess.Popen[str] | None = None

    def close(self) -> None:
        try:
            self.sock.close()
        finally:
            if self.process is not None:
                self.process.terminate()
                self.process.wait(timeout=2)

    def send_tx_packet(self, radio_id: int, frame: bytes) -> None:
        self.sock.send(bytes((IPC_MSG_TX_PACKET, radio_id)) + frame)


def _connect_seqpacket(socket_path: str, timeout_s: float = 5.0) -> socket.socket:
    deadline = time.monotonic() + timeout_s

    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            sock.connect(socket_path)
            return sock
        except OSError:
            sock.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def start_receiver_client(
    socket_path: str,
    *,
    receiver_bin: str | None = None,
    autostart: bool = True,
) -> RadioReceiverClient:
    process = None

    if autostart:
        if receiver_bin is None:
            raise ValueError("receiver_bin is required when autostart is enabled")

        process = subprocess.Popen(
            [receiver_bin],
            stdout=subprocess.DEVNULL,
            stderr=None,
            env=os.environ.copy(),
        )

    sock = _connect_seqpacket(socket_path)
    return RadioReceiverClient(sock=sock, process=process)


def run_native_radio(
    queue_path: str,
    socket_path: str,
    *,
    receiver_bin: str | None = None,
    autostart: bool = True,
) -> None:
    client = start_receiver_client(
        socket_path,
        receiver_bin=receiver_bin,
        autostart=autostart,
    )

    try:
        while True:
            message = client.sock.recv(512)
            if not message:
                raise RuntimeError("radio receiver socket closed")

            if len(message) < 2:
                logger.warning("Ignoring short IPC message of %d bytes", len(message))
                continue

            message_kind = message[0]
            radio_id = message[1]

            if message_kind == IPC_MSG_RX_PACKET:
                frame = message[2:]
                message_type, payload = decode_rf_frame_to_ingest_payload(frame)
                queue_writer.enqueue(queue_path, message_type, payload)
                logger.info("Queued radio %d %s payload at %s", radio_id, message_type, payload["time"])
                continue

            if message_kind == IPC_MSG_BUTTON_EVENT:
                logger.info("Ignoring button event %d", radio_id)
                continue

            if message_kind == IPC_MSG_TX_RESULT:
                logger.info("TX result from radio %d status=%d", radio_id, message[2] if len(message) > 2 else -1)
                continue

            logger.warning("Ignoring unknown IPC message kind 0x%02x", message_kind)
    finally:
        client.close()
