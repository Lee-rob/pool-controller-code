"""Checks evidence extraction cannot turn corrupt/unknown bytes into states."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("capture", Path(__file__).resolve().parents[1] / "scripts/analyze_chlorinator.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def frame(command, payload, source=0x84):
    message = bytearray([2, 0, source, 255, 255, 128, 0, command, len(payload) + 12])
    message.append(sum(message) & 255)
    message.extend(payload)
    message.extend([sum(payload) & 255, 3])
    return message.hex(" ").upper()


class CaptureTests(unittest.TestCase):
    def test_targets_are_not_measurements_or_production(self):
        report = capture.analyse("2026-10-18T13:07:06+11:00 " + frame(0x1D, [1, 75, 0]))
        self.assertEqual(report["chlorinator_timeline"][0]["interpretation"], {"field": "pH target", "value": 7.5})
        self.assertIsNone(report["production_active"])
        self.assertIsNone(report["g4_fault"])

    def test_ble_fault_and_cell_bytes_have_no_assumed_bus_meaning(self):
        report = capture.analyse("2026-10-18T13:07:06+11:00 " + frame(0x13, [7, 0x20]))
        self.assertEqual(report["chlorinator_timeline"][0]["interpretation"]["field"], "unverified raw payload")
        self.assertIsNone(report["manual_off"])

    def test_corrupt_frame_is_excluded(self):
        damaged = frame(0x1F, [1, 75, 0]).replace("4B 00 4C", "4C 00 4C")
        report = capture.analyse("2026-10-18T13:07:06+11:00 " + damaged)
        self.assertEqual(report["valid_raw_frames"], 0)
        self.assertEqual(len(report["rejected_frames"]), 1)

    def test_gap_uses_pc_timezone_and_only_chlorinator_replies(self):
        lines = [
            "2026-10-18T13:08:00+11:00 " + frame(0x1F, [2, 0xD7, 1]),
            "2026-10-18T13:09:00+11:00 " + frame(0x1D, [2, 0x8A, 2], source=0x50),
            "2026-10-18T02:13:00+00:00 " + frame(0x1D, [2, 0x8A, 2]),
        ]
        report = capture.analyse("\n".join(lines))
        self.assertEqual(report["largest_gap_between_captured_replies"]["seconds"], 300)
        self.assertEqual(report["chlorinator_frames"], 2)
        self.assertEqual(report["chlorinator_timeline"][0]["interpretation"], {"field": "ORP measurement", "value": 471})


if __name__ == "__main__":
    unittest.main()
