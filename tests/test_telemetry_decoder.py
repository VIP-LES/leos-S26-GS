import unittest

from src.telemetry_decoder import crc16_ccitt_false, decode_rf_frame, encode_rf_frame


class TelemetryDecoderTests(unittest.TestCase):
    def test_crc16_known_value(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_round_trip_sensor_payload(self):
        payload = {
            "time": "2026-03-18T00:00:00+00:00",
            "temperature": 24.5,
            "pressure": 101325.0,
            "pm10_env": None,
            "pm25_env": 1.0,
            "pm100_env": 2.0,
            "aqi_pm25_us": 3.0,
            "aqi_pm100_us": 4.0,
            "uvi": 5.0,
            "light_lux": 6.0,
            "humidity": 7.0,
        }
        frame = encode_rf_frame("sensors_and_gps", payload, sequence=12)
        message_type, decoded = decode_rf_frame(frame)
        self.assertEqual(message_type, "sensors_and_gps")
        self.assertEqual(decoded["time"], "2026-03-18T00:00:00+00:00")
        self.assertAlmostEqual(decoded["temperature"], 24.5, places=4)
        self.assertIsNone(decoded["pm10_env"])

    def test_round_trip_efm_payload(self):
        payload = {
            "time": "2026-03-18T00:00:00+00:00",
            "adc1_ch1": 1.0,
            "adc1_ch2": 2.0,
            "adc1_ch3": 3.0,
            "adc1_ch4": 4.0,
            "adc2_ch1": 5.0,
            "adc2_ch2": 6.0,
            "adc2_ch3": 7.0,
            "adc2_ch4": 8.0,
        }
        frame = encode_rf_frame("efm", payload, sequence=9)
        message_type, decoded = decode_rf_frame(frame)
        self.assertEqual(message_type, "efm")
        self.assertAlmostEqual(decoded["adc2_ch4"], 8.0, places=4)


if __name__ == "__main__":
    unittest.main()
