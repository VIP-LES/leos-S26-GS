import os
import tempfile
import unittest
from unittest import mock

from src.forwarder import run_forwarder
from src.queue_writer import enqueue, fetch_unsent, init_queue


class ForwarderPipelineTests(unittest.TestCase):
    def test_forwarder_posts_and_marks_sent(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            queue_path = os.path.join(tmpdir, "spool.db")
            init_queue(queue_path)
            enqueue(
                queue_path,
                "sensors_and_gps",
                {"time": "2026-03-18T00:00:00+00:00", "temperature": 21.0},
            )

            response = mock.Mock(status_code=201, text="ok")
            with mock.patch("src.forwarder.requests.post", return_value=response) as post_mock:
                with mock.patch("src.forwarder.time.sleep", side_effect=KeyboardInterrupt()):
                    with self.assertRaises(KeyboardInterrupt):
                        run_forwarder(queue_path, "http://example.test", poll_interval_s=0.0)

            called_url = post_mock.call_args_list[0].args[0]
            called_payload = post_mock.call_args_list[0].kwargs["json"]
            self.assertEqual(called_url, "http://example.test/ingest/sensors-and-gps")
            self.assertEqual(
                called_payload,
                {"time": "2026-03-18T00:00:00+00:00", "temperature": 21.0},
            )
            self.assertEqual(fetch_unsent(queue_path), [])


if __name__ == "__main__":
    unittest.main()
