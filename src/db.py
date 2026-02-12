"""Database connection and operations for TimescaleDB."""

import os
import logging

import psycopg2

logger = logging.getLogger(__name__)

_conn: psycopg2.extensions.connection | None = None


def _get_conn() -> psycopg2.extensions.connection:
    """Return the active connection or raise if not connected."""
    if _conn is None:
        raise RuntimeError("Database not connected. Call connect() first.")
    return _conn


def connect():
    """Create a connection to TimescaleDB using environment variables.

    Required env vars: PGHOST, PGPORT, PGUSER, PGPASSWORD, PGDATABASE.
    Optional: PGSSL (set to "true" to enable SSL).
    """
    global _conn

    _conn = psycopg2.connect(
        host=os.environ["PGHOST"],
        port=int(os.environ["PGPORT"]),
        user=os.environ["PGUSER"],
        password=os.environ["PGPASSWORD"],
        database=os.environ["PGDATABASE"],
        sslmode="require" if os.environ.get("PGSSL") == "true" else "prefer",
    )
    _conn.autocommit = True
    logger.info(
        "Connected to TimescaleDB at %s:%s", os.environ["PGHOST"], os.environ["PGPORT"]
    )


def ensure_schema():
    """Create the launch_data table and TimescaleDB hypertable if they don't exist."""
    conn = _get_conn()
    with conn.cursor() as cur:
        cur.execute("CREATE EXTENSION IF NOT EXISTS timescaledb")
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS launch_data (
                time         timestamptz PRIMARY KEY,
                temp         double precision,
                pressure     double precision,
                aqi_pm100_us double precision,
                aqi_pm25_us  double precision,
                pm100_env    double precision,
                pm25_env     double precision,
                pm10_env     double precision,
                uv           integer
            )
            """
        )
        cur.execute(
            "SELECT create_hypertable('launch_data', 'time', "
            "if_not_exists => TRUE, migrate_data => TRUE)"
        )
    logger.info("Schema ensured")


def insert_row(row: dict) -> bool:
    """Insert a telemetry row. Returns True if inserted, False if duplicate (skipped).

    Uses ON CONFLICT (time) DO NOTHING for idempotent writes.
    """
    sql = """
        INSERT INTO launch_data
            (time, temp, pressure, aqi_pm100_us, aqi_pm25_us,
             pm100_env, pm25_env, pm10_env, uv)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON CONFLICT (time) DO NOTHING
    """
    values = (
        row["time"],
        row.get("temp"),
        row.get("pressure"),
        row.get("aqi_pm100_us"),
        row.get("aqi_pm25_us"),
        row.get("pm100_env"),
        row.get("pm25_env"),
        row.get("pm10_env"),
        row.get("uv"),
    )
    conn = _get_conn()
    with conn.cursor() as cur:
        cur.execute(sql, values)
        inserted = cur.rowcount > 0

    if inserted:
        logger.info("Inserted row at time %s", row["time"])
    else:
        logger.debug("Duplicate row at time %s, skipped", row["time"])

    return inserted


def close():
    """Close the database connection."""
    global _conn
    if _conn:
        _conn.close()
        _conn = None
        logger.info("Database connection closed")
