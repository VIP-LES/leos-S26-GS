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
    """Create the telemetry hypertables if they don't exist."""
    conn = _get_conn()
    with conn.cursor() as cur:
        cur.execute("CREATE EXTENSION IF NOT EXISTS timescaledb")
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS sensors_and_gps_data (
                time         timestamptz PRIMARY KEY,
                temperature  double precision,
                pressure     double precision,
                pm10_env     double precision,
                pm25_env     double precision,
                pm100_env    double precision,
                aqi_pm25_us  double precision,
                aqi_pm100_us double precision,
                uvi          double precision,
                light_lux    double precision,
                humidity     double precision
            )
            """
        )
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS efm_data (
                time      timestamptz PRIMARY KEY,
                adc1_ch1  double precision,
                adc1_ch2  double precision,
                adc1_ch3  double precision,
                adc1_ch4  double precision,
                adc2_ch1  double precision,
                adc2_ch2  double precision,
                adc2_ch3  double precision,
                adc2_ch4  double precision
            )
            """
        )
        cur.execute(
            "SELECT create_hypertable('sensors_and_gps_data', 'time', "
            "if_not_exists => TRUE, migrate_data => TRUE)"
        )
        cur.execute(
            "SELECT create_hypertable('efm_data', 'time', "
            "if_not_exists => TRUE, migrate_data => TRUE)"
        )
    logger.info("Schema ensured")


def insert_sensors_and_gps_row(row: dict) -> bool:
    """Insert one slow telemetry row."""
    sql = """
        INSERT INTO sensors_and_gps_data
            (time, temperature, pressure, pm10_env, pm25_env, pm100_env,
             aqi_pm25_us, aqi_pm100_us, uvi, light_lux, humidity)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON CONFLICT (time) DO NOTHING
    """
    values = (
        row["time"],
        row.get("temperature"),
        row.get("pressure"),
        row.get("pm10_env"),
        row.get("pm25_env"),
        row.get("pm100_env"),
        row.get("aqi_pm25_us"),
        row.get("aqi_pm100_us"),
        row.get("uvi"),
        row.get("light_lux"),
        row.get("humidity"),
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


def insert_efm_row(row: dict) -> bool:
    """Insert one EFM row."""
    sql = """
        INSERT INTO efm_data
            (time, adc1_ch1, adc1_ch2, adc1_ch3, adc1_ch4,
             adc2_ch1, adc2_ch2, adc2_ch3, adc2_ch4)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON CONFLICT (time) DO NOTHING
    """
    values = (
        row["time"],
        row.get("adc1_ch1"),
        row.get("adc1_ch2"),
        row.get("adc1_ch3"),
        row.get("adc1_ch4"),
        row.get("adc2_ch1"),
        row.get("adc2_ch2"),
        row.get("adc2_ch3"),
        row.get("adc2_ch4"),
    )
    conn = _get_conn()
    with conn.cursor() as cur:
        cur.execute(sql, values)
        inserted = cur.rowcount > 0

    if inserted:
        logger.info("Inserted EFM row at time %s", row["time"])
    else:
        logger.debug("Duplicate EFM row at time %s, skipped", row["time"])
    return inserted


def insert_row(row: dict) -> bool:
    """Backward-compatible alias for the original single-stream API."""
    return insert_sensors_and_gps_row(row)


def close():
    """Close the database connection."""
    global _conn
    if _conn:
        _conn.close()
        _conn = None
        logger.info("Database connection closed")
