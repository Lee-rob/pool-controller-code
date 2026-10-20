"""Unattended-capture regression tests, including real local HTTP/TCP streams."""
import copy
import ctypes
import importlib.util
import json
from pathlib import Path
import queue
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

spec = importlib.util.spec_from_file_location("watch", Path(__file__).resolve().parents[1]/"scripts/Watch-Chlorinator.py")
watch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watch)


def status(value=650, age=0, active=True, uptime=1000000):
    return {"current_ms": uptime, "channels": [{"id": 8, "type": "Filter", "active": active}],
            "chlorinator": {"orp_reading": value, "orp_reading_age_ms": age}}


class DetectorTests(unittest.TestCase):
    def detector(self):
        return watch.Detector(flat=900, stale=300, warmup=180, recent=1800)

    def test_fresh_identical_values_trigger_only_flatline_once(self):
        d = self.detector()
        events = []
        for second in range(0, 1201, 15):
            events += d.observe(status(), second)
        self.assertEqual([e["code"] for e in events], ["orp_flatline"])
        self.assertTrue(events[0]["details"]["fresh_messages_continue"])
        self.assertFalse(events[0]["confirmed_g4"])
        self.assertEqual(d.observe(status(value=651), 1215), [])
        again = []
        for second in range(1230, 2131, 15):
            again += d.observe(status(value=651), second)
        self.assertEqual([e["code"] for e in again], ["orp_flatline"])

    def test_frozen_measurement_age_is_distinct_from_flat_value(self):
        d = self.detector()
        events = []
        for second in range(0, 601, 15):
            events += d.observe(status(age=second*1000), second)
        self.assertEqual([e["code"] for e in events], ["orp_data_stale"])
        self.assertFalse(events[0]["confirmed_g4"])

    def test_inactive_overnight_does_not_trigger_chemistry_incidents(self):
        d = self.detector()
        events = []
        for second in range(0, 12*3600, 15):
            events += d.observe(status(age=second*1000, active=False), second)
        self.assertEqual(events, [])

    def test_pump_stopping_at_fault_does_not_disarm_immediately(self):
        d = self.detector()
        events = []
        for second in range(0, 601, 15):
            events += d.observe(status(age=max(0, second-240)*1000, active=second<240), second)
        self.assertEqual([e["code"] for e in events], ["orp_data_stale"])
        self.assertEqual(events[0]["details"]["pump_context"], "recently reported running")

    def test_missing_chemistry_sample_after_running_is_captured(self):
        d = self.detector()
        events = []
        for second in range(0, 601, 15):
            events += d.observe(status(value=None, age=None), second)
        self.assertEqual([e["code"] for e in events], ["orp_data_stale"])

    def test_reboot_resets_chemistry_timers(self):
        d = self.detector()
        for second in range(0, 601, 15):
            d.observe(status(uptime=1000000+second*1000), second)
        events = d.observe(status(value=None, age=None, uptime=100), 615)
        self.assertEqual([e["code"] for e in events], ["gateway_restarted"])
        self.assertFalse(d.armed)

    def test_uptime_wrap_does_not_imply_reboot(self):
        d = self.detector()
        d.observe(status(uptime=0xFFFFFFFF-10), 0)
        self.assertEqual(d.observe(status(uptime=10), 15), [])

    def test_network_loss_is_not_labelled_g4_and_resets_flatline_interval(self):
        d = self.detector()
        d.observe(status(), 0)
        self.assertEqual(d.failure(15), [])
        events = d.failure(90)
        self.assertEqual([e["code"] for e in events], ["gateway_unreachable"])
        self.assertFalse(events[0]["confirmed_g4"])
        self.assertEqual(d.failure(120), [])
        self.assertEqual(d.observe(status(), 1000), [])
        self.assertEqual(d.changed, 1000)


class RecorderTests(unittest.TestCase):
    def test_cleanup_access_denied_keeps_zip_and_recording_continues(self):
        with tempfile.TemporaryDirectory() as folder:
            r = watch.Recorder(folder)
            r.record("bus", "before cleanup error")
            r.start({"code": "test"})
            with patch.object(watch.shutil, "rmtree", side_effect=PermissionError("Access denied")):
                first = r.finish()
            self.assertTrue(first.exists())
            self.assertTrue((Path(folder)/"incident-01"/"records.jsonl").exists())
            with zipfile.ZipFile(first) as z:
                self.assertIsNone(z.testzip())
                self.assertIn(b"before cleanup error", z.read("bus-timeline.txt"))
            r.record("bus", "after cleanup error")
            r.start({"code": "second"})
            second = r.finish()
            self.assertNotEqual(first, second)
            with zipfile.ZipFile(second) as z:
                self.assertIn(b"after cleanup error", z.read("bus-timeline.txt"))
            r.close()

    def test_failed_zip_save_preserves_raw_incident_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            r = watch.Recorder(folder)
            r.record("bus", "keep this evidence")
            r.start({"code": "test"})
            with patch.object(watch.os, "replace", side_effect=PermissionError("Access denied")):
                with self.assertRaises(PermissionError):
                    r.finish()
            self.assertIn("keep this evidence", (Path(folder)/"incident-01"/"records.jsonl").read_text())
            self.assertFalse((Path(folder)/"incident-01.zip").exists())
            r.close()

    def test_precursor_and_aftermath_are_saved_without_manual_markers(self):
        with tempfile.TemporaryDirectory() as folder:
            clock = [0]
            r = watch.Recorder(folder, pre=30, post=10, clock=lambda: clock[0])
            r.record("bus", "too old")
            clock[0] = 40
            r.record("bus", "02 00 84 FF FF 80 00 1D 0F 30 01 4B 00 4C 03")
            r.record("status", status(age=300000))
            clock[0] = 50
            r.start({"code": "orp_data_stale", "confirmed_g4": False})
            clock[0] = 59
            r.record("bus", "aftermath")
            self.assertFalse(r.due())
            clock[0] = 60
            self.assertTrue(r.due())
            path = r.finish()
            with zipfile.ZipFile(path) as z:
                bus = z.read("bus-timeline.txt").decode()
                self.assertNotIn("too old", bus)
                self.assertIn("02 00 84", bus)
                self.assertIn("aftermath", bus)
                self.assertIn("300000", z.read("status-samples.jsonl").decode())
                self.assertFalse(json.loads(z.read("incident.json"))["confirmed_g4"])
            self.assertFalse((Path(folder)/"incident-01").exists())
            r.close()

    def test_new_trigger_during_capture_is_merged_not_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            r = watch.Recorder(folder)
            r.start({"code": "orp_data_stale"})
            r.start({"code": "gateway_unreachable"})
            with zipfile.ZipFile(r.finish(stopped=True)) as z:
                meta = json.loads(z.read("incident.json"))
                self.assertEqual(len(meta["triggers"]), 2)
                self.assertTrue(meta["watcher_stopped"])
            self.assertEqual(r.serial, 1)
            r.close()

    def test_ring_and_incident_size_limits_are_enforced(self):
        with tempfile.TemporaryDirectory() as folder:
            r = watch.Recorder(folder, ring_limit=1000, incident_limit=800)
            for _ in range(30):
                r.record("bus", "x"*200)
            self.assertLessEqual(r.current.stat().st_size, 1000)
            self.assertLessEqual(r.previous.stat().st_size, 1000)
            r.start({"code": "test"})
            for _ in range(10):
                r.record("bus", "y"*200)
            self.assertLessEqual(r.active_bytes, 800)
            self.assertTrue(r.due())
            with zipfile.ZipFile(r.finish()) as z:
                meta = json.loads(z.read("incident.json"))
                self.assertTrue(meta["size_limit_reached"])
                self.assertTrue(meta["prehistory_size_limited"])
            r.close()


class IntegrationTests(unittest.TestCase):
    def test_windows_default_uses_local_app_data(self):
        with tempfile.TemporaryDirectory() as local:
            with patch.object(watch.sys, "platform", "win32"), \
                 patch.dict(watch.os.environ, {"LOCALAPPDATA": local}), \
                 patch.object(watch, "desktop", side_effect=AssertionError("Desktop must not be used")):
                self.assertEqual(watch.capture_root(), Path(local)/"PoolControllerCaptures")

    def test_windows_default_without_environment_has_local_fallback(self):
        with tempfile.TemporaryDirectory() as home:
            with patch.object(watch.sys, "platform", "win32"), \
                 patch.dict(watch.os.environ, {}, clear=True), \
                 patch.object(watch.Path, "home", return_value=Path(home)):
                self.assertEqual(watch.capture_root(), Path(home)/"AppData"/"Local"/"PoolControllerCaptures")

    def test_windows_sleep_request_is_restored(self):
        api = MagicMock(return_value=0x80000000)
        fake = MagicMock()
        fake.kernel32.SetThreadExecutionState = api
        with patch.object(watch.os, "name", "nt"), patch.object(ctypes, "windll", fake, create=True):
            restore = watch.request_awake()
            restore()
        self.assertEqual([call.args[0] for call in api.call_args_list], [0x80000001, 0x80000000])

    def test_complete_unattended_program_saves_flatline_before_and_after(self):
        class HTTP(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps(status()).encode())
            def log_message(self, *args):
                pass
        class TCP(socketserver.BaseRequestHandler):
            def handle(self):
                try:
                    for _ in range(300):
                        self.request.sendall(b"02 00 84 FF FF 80 00 1D 0F 30 01 4B 00 4C 03\n")
                        time.sleep(.1)
                except OSError:
                    pass
        http = ThreadingHTTPServer(("127.0.0.1", 0), HTTP)
        tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), TCP)
        tcp.daemon_threads = True
        for server in (http, tcp):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as folder:
                command = [sys.executable, spec.origin, "--controller", "127.0.0.1:%d"%http.server_port,
                           "--debug-port", str(tcp.server_address[1]), "--hours", "0.004",
                           "--poll-seconds", "5", "--warmup-minutes", "0.01",
                           "--flat-minutes", "0.05", "--post-minutes", "0.1", "--output", folder]
                result = subprocess.run(command, capture_output=True, text=True, timeout=25)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                archives = list(Path(folder).glob("incident-*.zip"))
                self.assertEqual(len(archives), 1, result.stdout)
                with zipfile.ZipFile(archives[0]) as archive:
                    meta = json.loads(archive.read("incident.json"))
                    samples = [json.loads(line) for line in archive.read("status-samples.jsonl").splitlines()]
                    self.assertEqual(meta["triggers"][0]["code"], "orp_flatline")
                    self.assertFalse(meta["confirmed_g4"])
                    self.assertFalse(meta["watcher_stopped"])
                    self.assertTrue(any(item["captured_at"] < meta["started_at"] for item in samples))
                    self.assertTrue(any(item["captured_at"] > meta["started_at"] for item in samples))
                    self.assertIn(b"02 00 84", archive.read("bus-timeline.txt"))
        finally:
            for server in (http, tcp):
                server.shutdown()
                server.server_close()

    def test_actual_http_and_tcp_collection_survives_tcp_reconnection(self):
        class HTTP(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps(status()).encode())
            def log_message(self, *args):
                pass
        class TCP(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.sendall(b"Pool debug\n02 00 84 FF FF 80 00 1D 0F 30 01 4B 00 4C 03\n")
        http = ThreadingHTTPServer(("127.0.0.1", 0), HTTP)
        tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), TCP)
        for server in (http, tcp):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        with tempfile.TemporaryDirectory() as folder:
            r = watch.Recorder(folder)
            stop, messages = threading.Event(), queue.Queue()
            counters = {"frames": 0, "connected": False}
            workers = [threading.Thread(target=watch.http_worker, args=("http://127.0.0.1:%d"%http.server_port, .05, stop, r, messages)),
                       threading.Thread(target=watch.tcp_worker, args=("127.0.0.1", tcp.server_address[1], stop, r, counters))]
            for worker in workers:
                worker.start()
            try:
                ok, sample, _ = messages.get(timeout=3)
                self.assertTrue(ok)
                self.assertEqual(sample["chlorinator"]["orp_reading"], 650)
                deadline = time.monotonic()+8
                while counters["frames"] < 2 and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertGreaterEqual(counters["frames"], 2)
                r.start({"code": "integration_test", "confirmed_g4": False})
            finally:
                stop.set()
                for worker in workers:
                    worker.join(timeout=6)
                path = r.finish(stopped=True)
                r.close()
                for server in (http, tcp):
                    server.shutdown()
                    server.server_close()
            with zipfile.ZipFile(path) as z:
                self.assertIn(b"02 00 84", z.read("bus-timeline.txt"))
                self.assertIn(b"orp_reading", z.read("status-samples.jsonl"))
                self.assertIn(b"tcp_error", z.read("capture-health.jsonl"))


if __name__ == "__main__":
    unittest.main()
