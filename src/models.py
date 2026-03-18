"""Shared payload models and routing constants."""

from pydantic import BaseModel

SENSORS_AND_GPS = "sensors_and_gps"
EFM = "efm"

INGEST_PATHS = {
    SENSORS_AND_GPS: "/ingest/sensors-and-gps",
    EFM: "/ingest/efm",
}


class SensorsAndGpsPayload(BaseModel):
    """Structured slow telemetry payload."""

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


class EfmPayload(BaseModel):
    """Structured EFM telemetry payload."""

    time: str
    adc1_ch1: float | None = None
    adc1_ch2: float | None = None
    adc1_ch3: float | None = None
    adc1_ch4: float | None = None
    adc2_ch1: float | None = None
    adc2_ch2: float | None = None
    adc2_ch3: float | None = None
    adc2_ch4: float | None = None
