"""FastAPI Ingest API server for telemetry data."""

import logging
from datetime import datetime

from fastapi import FastAPI
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from src import db

logger = logging.getLogger(__name__)


class TelemetryPayload(BaseModel):
    """Pydantic model for incoming telemetry data."""

    time: str
    temperature: float | None = None
    pressure: float | None = None
    pm10_env: float | None = None
    pm25_env: float | None = None
    pm100_env: float | None = None
    aqi_pm25_us: float | None = None
    aqi_pm100_us: float | None = None
    uvi: float | None = None
    light_lux: float | None = None
    humidity: float | None = None


app = FastAPI(title="LEOS Ingest API")


@app.get("/health")
def health():
    """Health check endpoint."""
    return {"status": "ok"}


@app.post("/ingest")
def ingest(payload: TelemetryPayload):
    """Ingest a single telemetry data point.

    Returns:
        201 - Row inserted successfully.
        200 - Duplicate timestamp, row skipped.
        400 - Invalid time format.
        500 - Database error.
    """
    # Validate that the time string is parseable.
    try:
        datetime.fromisoformat(payload.time)
    except ValueError:
        return JSONResponse(
            status_code=400,
            content={"error": f"Invalid time format: {payload.time}"},
        )

    try:
        inserted = db.insert_row(payload.model_dump())
        if inserted:
            return JSONResponse(
                status_code=201,
                content={"status": "inserted", "time": payload.time},
            )
        else:
            return JSONResponse(
                status_code=200,
                content={"status": "duplicate, skipped", "time": payload.time},
            )
    except Exception as e:
        logger.error("Database insert failed: %s", e)
        return JSONResponse(
            status_code=500,
            content={"error": str(e)},
        )
