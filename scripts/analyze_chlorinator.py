#!/usr/bin/env python3
"""Summarise passive capture evidence; unknown states remain unknown.

Accepts a capture ZIP, its directory, or bus-timeline.txt. Uses only Python's
standard library and never connects to the controller or sends bus traffic.
"""
import argparse
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import re
import zipfile

RAW_LINE = re.compile(r"^(\S+)\s+((?:[0-9A-Fa-f]{2}\s+){10,}[0-9A-Fa-f]{2})\s*$")


def parse_frame(line):
    match = RAW_LINE.match(line.strip().lstrip("\ufeff"))
    if not match:
        return None
    stamp, hex_bytes = match.groups()
    frame = bytes.fromhex(hex_bytes)
    if frame[0] != 2 or frame[-1] != 3 or frame[8] != len(frame):
        raise ValueError("Frame boundary/length mismatch")
    if sum(frame[:9]) & 255 != frame[9]:
        raise ValueError("Header checksum mismatch")
    if frame[5:7] == b"\x00\x00" and len(frame) == 11:
        payload = b""
    elif frame[5:7] == b"\x80\x00" and len(frame) >= 12:
        payload = frame[10:-2]
        if sum(payload) & 255 != frame[-2]:
            raise ValueError("Data checksum mismatch")
    else:
        raise ValueError("Invalid frame type")
    datetime.fromisoformat(stamp)  # reject invalid timestamps
    return {
        "captured_at": stamp,
        "source": f"0x{int.from_bytes(frame[1:3], 'big'):04X}",
        "destination": f"0x{int.from_bytes(frame[3:5], 'big'):04X}",
        "command": f"0x{frame[7]:02X}",
        "payload_hex": payload.hex(" ").upper(),
    }


def interpretation(frame):
    payload = bytes.fromhex(frame["payload_hex"])
    if frame["command"] in ("0x1D", "0x1F") and len(payload) >= 3 and payload[0] in (1, 2):
        value = int.from_bytes(payload[1:3], "little")
        name = "pH" if payload[0] == 1 else "ORP"
        kind = "target" if frame["command"] == "0x1D" else "measurement"
        return {"field": f"{name} {kind}", "value": value / 10 if payload[0] == 1 else value}
    if frame["command"] == "0x0A" and len(payload) >= 2:
        return {"field": "firmware", "value": f"{payload[0]}.{payload[1]}"}
    return {"field": "unverified raw payload", "value": None}


def analyse(timeline, observations=()):
    frames, rejected = [], []
    for number, line in enumerate(timeline.splitlines(), 1):
        try:
            frame = parse_frame(line)
        except ValueError as error:
            rejected.append({"line": number, "reason": str(error)})
            continue
        if frame:
            frames.append(frame)
    chlor = [frame for frame in frames if frame["source"] == "0x0084"]
    chlor.sort(key=lambda frame: datetime.fromisoformat(frame["captured_at"]))
    largest_gap = None
    for first, second in zip(chlor, chlor[1:]):
        seconds = (datetime.fromisoformat(second["captured_at"]) -
                   datetime.fromisoformat(first["captured_at"])).total_seconds()
        if largest_gap is None or seconds > largest_gap["seconds"]:
            largest_gap = {"seconds": seconds, "from": first["captured_at"], "to": second["captured_at"]}
    for frame in chlor:
        frame["interpretation"] = interpretation(frame)
    return {
        "valid_raw_frames": len(frames),
        "rejected_frames": rejected,
        "chlorinator_frames": len(chlor),
        "chlorinator_command_counts": dict(Counter(frame["command"] for frame in chlor)),
        "largest_gap_between_captured_replies": largest_gap,
        "captured_gateway_originated_frames": sum(frame["source"] in ("0xAC1D", "0x00F0") for frame in frames),
        "production_active": None,
        "g4_fault": None,
        "manual_off": None,
        "limits": "Reply presence is not proof of two-way health. Gaps may include capture loss. Bus mappings for production/G4/Manual Off are unverified.",
        "display_observations": list(observations),
        "chlorinator_timeline": chlor,
    }


def read_capture(path):
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            candidates = [name for name in archive.namelist() if Path(name).name == "bus-timeline.txt"]
            if len(candidates) != 1:
                raise ValueError("Capture must contain exactly one bus-timeline.txt")
            timeline = archive.read(candidates[0]).decode("utf-8-sig")
            marker_name = str(Path(candidates[0]).parent / "display-observations.jsonl")
            markers = archive.read(marker_name).decode("utf-8-sig") if marker_name in archive.namelist() else ""
    else:
        timeline_path = path / "bus-timeline.txt" if path.is_dir() else path
        timeline = timeline_path.read_text(encoding="utf-8-sig")
        marker_path = timeline_path.parent / "display-observations.jsonl"
        markers = marker_path.read_text(encoding="utf-8-sig") if marker_path.exists() else ""
    return timeline, [json.loads(line) for line in markers.splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, help="Write JSON report to this file instead of stdout")
    args = parser.parse_args()
    report = analyse(*read_capture(args.capture))
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
