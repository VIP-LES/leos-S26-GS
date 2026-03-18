"""SQLite-backed queue helpers for Pi-side durability."""

from __future__ import annotations

import json
import os
import sqlite3
from datetime import datetime, timezone


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _connect(queue_path: str) -> sqlite3.Connection:
    os.makedirs(os.path.dirname(queue_path) or ".", exist_ok=True)
    conn = sqlite3.connect(queue_path)
    conn.row_factory = sqlite3.Row
    return conn


def init_queue(queue_path: str) -> None:
    with _connect(queue_path) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS outgoing_queue (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                message_type TEXT NOT NULL,
                payload TEXT NOT NULL,
                created_at TEXT NOT NULL,
                sent_at TEXT
            )
            """
        )
        conn.commit()


def enqueue(queue_path: str, message_type: str, payload: dict) -> int:
    with _connect(queue_path) as conn:
        cur = conn.execute(
            """
            INSERT INTO outgoing_queue (message_type, payload, created_at)
            VALUES (?, ?, ?)
            """,
            (message_type, json.dumps(payload), _utc_now()),
        )
        conn.commit()
        return int(cur.lastrowid)


def fetch_unsent(queue_path: str, limit: int = 100) -> list[dict]:
    with _connect(queue_path) as conn:
        rows = conn.execute(
            """
            SELECT id, message_type, payload, created_at, sent_at
            FROM outgoing_queue
            WHERE sent_at IS NULL
            ORDER BY id ASC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [dict(row) for row in rows]


def mark_sent(queue_path: str, message_id: int) -> None:
    with _connect(queue_path) as conn:
        conn.execute(
            "UPDATE outgoing_queue SET sent_at = ? WHERE id = ?",
            (_utc_now(), message_id),
        )
        conn.commit()
