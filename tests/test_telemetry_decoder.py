import unittest

from src.telemetry_decoder import (
    crc16_ccitt_false,
    decode_rf_frame,
    encode_rf_frame,
    rf_payload_to_ingest_payload,
)


class TelemetryDecoderTests(unittest.TestCase):
    def test_crc16_known_value(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_round_trip_sensor_payload(self):
        payload = {
            "time": "2026-03-18T00:00:00+00:00",
            "bme688_valid": True,
            "bme688": {
                "humidity": 7.0,
                "pressure": 101325.0,
                "temperature": 24.5,
                "altitude": 123.0,
                "gas_resistance": 456.0,
            },
            "tsl2591_valid": True,
            "tsl2591": {
                "light_lux": 6.0,
                "raw_visible": 601,
                "raw_infrared": 602,
                "raw_full_spectrum": 603,
            },
            "ltr390_valid": False,
            "ltr390": {"uvs": 5000},
            "pmsa003i_valid": True,
            "pmsa003i": {
                "pm10_env": 10,
                "pm25_env": 20,
                "pm100_env": 30,
                "aqi_pm25_us": 40,
                "aqi_pm100_us": 50,
                "particles_03um": 60,
                "particles_05um": 70,
                "particles_10um": 80,
                "particles_25um": 90,
                "particles_50um": 100,
                "particles_100um": 110,
            },
            "gps": {
                "fix_ok": True,
                "lat": 42.1,
                "lon": -83.7,
                "alt_m": 300.0,
                "speed_mps": 12.5,
                "track_deg": 181.0,
                "sats_used": 8,
                "sats_visible": 12,
                "gps_utc": "2026-03-18T00:00:00+00:00",
            },
        }
        frame = encode_rf_frame("sensors_and_gps", payload, sequence=12)
        message_type, decoded = decode_rf_frame(frame)
        ingest_payload = rf_payload_to_ingest_payload(message_type, decoded)
        self.assertEqual(message_type, "sensors_and_gps")
        self.assertAlmostEqual(decoded["bme688"]["temperature"], 24.5, places=4)
        self.assertEqual(decoded["tsl2591"]["raw_visible"], 601)
        self.assertEqual(decoded["tsl2591"]["raw_infrared"], 602)
        self.assertEqual(decoded["tsl2591"]["raw_full_spectrum"], 603)
        self.assertFalse(decoded["ltr390_valid"])
        self.assertEqual(decoded["ltr390"]["uvs"], 5000)
        self.assertEqual(decoded["pmsa003i"]["particles_100um"], 110)
        self.assertEqual(ingest_payload["time"], "2026-03-18T00:00:00+00:00")
        self.assertAlmostEqual(ingest_payload["temperature"], 24.5, places=4)
        self.assertEqual(ingest_payload["raw_visible"], 601)
        self.assertEqual(ingest_payload["raw_full_spectrum"], 603)
        self.assertIsNone(ingest_payload["uvs"])
        self.assertEqual(ingest_payload["particles_25um"], 90.0)
        self.assertEqual(ingest_payload["gps_utc"], "2026-03-18T00:00:00+00:00")

    def test_round_trip_efm_payload(self):
        payload = {
            "time": "2026-03-18T00:00:00+00:00",
            "valid": False,
            "adc1_ch1_diff": 1.0,
            "adc1_ch4_breakbeam": 4.0,
            "adc2_ch1_diff": 5.0,
            "adc2_ch4_breakbeam": 8.0,
        }
        frame = encode_rf_frame("efm", payload, sequence=9)
        message_type, decoded = decode_rf_frame(frame)
        ingest_payload = rf_payload_to_ingest_payload(message_type, decoded)
        self.assertEqual(message_type, "efm")
        self.assertFalse(decoded["valid"])
        self.assertAlmostEqual(decoded["adc2_ch4_breakbeam"], 8.0, places=4)
        self.assertIsNone(ingest_payload["adc2_ch4_breakbeam"])


if __name__ == "__main__":
    unittest.main()
