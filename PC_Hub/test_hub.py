import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = Path(__file__).with_name("Monitoring_Hub.py")
spec = importlib.util.spec_from_file_location("monitoring_hub_tested", MODULE_PATH)
hub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hub)


class HubTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        hub.DATA_DIR = root
        hub.PHOTO_DIR = root / "photos"
        hub.BACKUP_DIR = root / "backups"
        hub.DB_PATH = root / "monitoring.sqlite"
        hub.API_KEY_FILE = root / "openai_key.txt"
        hub.ensure_dirs()
        hub.initialize_database()

    def tearDown(self):
        self.temp.cleanup()

    def test_defaults_and_exact_pass_schema(self):
        self.assertEqual(hub.get_setting("model"), "gpt-5.6-sol")
        self.assertEqual(hub.get_setting("effort"), "low")
        self.assertEqual(hub.get_setting("passes"), "1")
        for passes in (1, 2, 3):
            counts = hub.analysis_schema(passes)["properties"]["tiller_check_counts"]
            self.assertEqual(counts["minItems"], passes)
            self.assertEqual(counts["maxItems"], passes)

    def test_photo_uses_nearest_same_station_sensor(self):
        stamp = hub.now_iso()
        with hub.db_lock, hub.db() as con:
            con.execute(
                "INSERT INTO sensor_readings(station_id,date,reading_time,role,soil_percent) VALUES(?,?,?,?,?)",
                ("plant-1", "2026-08-30", "120000", "soil", 40.0),
            )
            con.execute(
                "INSERT INTO sensor_readings(station_id,date,reading_time,role,soil_percent) VALUES(?,?,?,?,?)",
                ("plant-1", "2026-08-30", "122000", "soil", 55.0),
            )
            con.execute(
                "INSERT INTO photos(station_id,date,filename,state,created_at,updated_at) VALUES(?,?,?,?,?,?)",
                (
                    "plant-1",
                    "2026-08-30",
                    "2026-08-30__12-18-00-000.jpg",
                    "downloaded",
                    stamp,
                    stamp,
                ),
            )
        hub.associate_sensor_readings("plant-1", "2026-08-30")
        with hub.db_lock, hub.db() as con:
            row = con.execute("""SELECT sr.soil_percent FROM photos p JOIN sensor_readings sr
              ON sr.id=p.sensor_reading_id WHERE p.station_id='plant-1'""").fetchone()
        self.assertEqual(row[0], 55.0)

    def test_sidecar_rebuilds_analysis(self):
        folder = hub.PHOTO_DIR / "plant-2" / "2026-08-30"
        folder.mkdir(parents=True)
        image = folder / "2026-08-30__10-00-00-000.jpg"
        image.write_bytes(b"\xff\xd8" + b"x" * 1200 + b"\xff\xd9")
        result = {
            "water_level_cm": None,
            "final_tiller_count": 4,
            "tiller_check_counts": [4],
            "growth_stage": "active tillering",
            "water_confidence": 0,
            "tiller_confidence": 0.8,
            "growth_confidence": 0.9,
            "notes": "visible basal shoots",
        }
        record = {
            "model": "gpt-5.6-sol",
            "reasoning_effort": "low",
            "pass_count": 1,
            "response_id": "resp_test",
            "completed_at": hub.now_iso(),
            "result": result,
            "usage": {},
        }
        image.with_suffix(image.suffix + ".analysis.json").write_text(
            json.dumps(record), encoding="utf-8"
        )
        hub.rescan_local_files()
        with hub.db_lock, hub.db() as con:
            photo = con.execute("SELECT state FROM photos WHERE station_id='plant-2'").fetchone()
            analysis = con.execute(
                "SELECT result_json FROM analyses WHERE station_id='plant-2'"
            ).fetchone()
        self.assertEqual(photo[0], "completed")
        self.assertEqual(json.loads(analysis[0])["growth_stage"], "active tillering")

    def test_manual_photo_uses_discovered_station_and_queues_sync(self):
        with hub.db_lock, hub.db() as con:
            con.execute(
                "UPDATE stations SET ip='192.168.1.211',online=1 WHERE station_id='plant-2'"
            )
        hub.sync_event.clear()
        with mock.patch.object(
            hub, "url_json", return_value={"ok": True, "filename": "test.jpg", "count": 8}
        ) as request:
            result = hub.take_station_photo({"station_id": "plant-2"})
        request.assert_called_once_with("http://192.168.1.211/snap", 20)
        self.assertEqual(result["filename"], "test.jpg")
        self.assertTrue(hub.sync_event.is_set())

    def test_truncated_photo_is_not_accepted(self):
        image = hub.PHOTO_DIR / "truncated.jpg"
        image.write_bytes(b"\xff\xd8" + b"x" * 1200)
        self.assertFalse(hub.valid_jpeg(image))
        image.write_bytes(b"\xff\xd8" + b"x" * 1200 + b"\xff\xd9")
        self.assertTrue(hub.valid_jpeg(image))

    def test_second_launch_does_not_start_workers_or_touch_data(self):
        with (
            mock.patch.object(hub, "HubHTTPServer", side_effect=OSError("in use")),
            mock.patch.object(hub, "url_json", return_value={"stations": []}),
            mock.patch.object(hub.webbrowser, "open") as browser,
            mock.patch.object(hub, "ensure_dirs") as initialize,
            mock.patch.object(hub.threading, "Thread") as worker,
        ):
            hub.run()
        browser.assert_called_once_with("http://localhost:8765")
        initialize.assert_not_called()
        worker.assert_not_called()


if __name__ == "__main__":
    unittest.main()
