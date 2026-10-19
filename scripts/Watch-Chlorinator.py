#!/usr/bin/env python3
"""Unattended, read-only evidence capture for controls12a. Python stdlib only."""
import argparse
from collections import deque
from datetime import datetime
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import threading
import time
import urllib.request
from urllib.parse import urlsplit
import uuid
import zipfile


def stamp():
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


class Detector:
    """Triggers are evidence requests, never declarations of a confirmed G4."""
    def __init__(self, flat=900, stale=300, warmup=180, recent=1800, outage=90):
        self.flat, self.stale, self.warmup = flat, stale, warmup
        self.recent, self.outage = recent, outage
        self.value = self.changed = self.running_since = self.last_running = None
        self.last_success = self.failed_since = self.uptime = None
        self.active = self.armed = False
        self.latched = set()

    def edge(self, code, condition, details):
        if not condition:
            self.latched.discard(code)
            return []
        if code in self.latched:
            return []
        self.latched.add(code)
        return [{"code": code, "confirmed_g4": False, "details": details}]

    def failure(self, now):
        if self.failed_since is None:
            self.failed_since = self.last_success if self.last_success is not None else now
        return self.edge("gateway_unreachable", now - self.failed_since >= self.outage,
                         "HTTP status unavailable; this is a connectivity event, not a G4 diagnosis")

    def observe(self, data, now):
        events = []
        gap = self.last_success is not None and now - self.last_success > 60
        self.last_success, self.failed_since = now, None
        self.latched.discard("gateway_unreachable")
        uptime = data.get("current_ms")
        if number(uptime) and number(self.uptime) and uptime < self.uptime:
            if not (self.uptime > 0xF0000000 and uptime < 0x10000000):
                events.append({"code": "gateway_restarted", "confirmed_g4": False,
                               "details": "Gateway uptime fell; ORP observation timers restarted"})
                self.active = self.armed = False
                self.running_since = self.last_running = self.changed = self.value = None
                self.latched.clear()
        self.uptime = uptime
        chemistry = data.get("chlorinator") or {}
        value, age = chemistry.get("orp_reading"), chemistry.get("orp_reading_age_ms")
        active = any(ch.get("active") is True and
                     (ch.get("id") == 8 or ch.get("type") == "Filter")
                     for ch in data.get("channels", []))
        # These are reported states, not independent physical flow measurements.
        if active:
            if not self.active:
                self.running_since = now
                if not self.armed:
                    self.changed = now
                    self.latched.discard("orp_flatline")
                    self.latched.discard("orp_data_stale")
            self.last_running = now
            if now - self.running_since >= self.warmup:
                self.armed = True
        else:
            self.running_since = None
            if self.last_running is None or now - self.last_running > self.recent:
                self.armed = False
        self.active = active
        if gap or not number(value) or self.value != value or self.changed is None:
            self.changed = now
        self.value = value
        context = "reported running" if active else "recently reported running"
        stale = number(age) and age >= self.stale * 1000
        if not number(value) and active and self.running_since is not None:
            stale = now - self.running_since >= self.stale
        events += self.edge("orp_data_stale", self.armed and stale,
                            {"orp": value, "reading_age_ms": age, "pump_context": context})
        events += self.edge("orp_flatline", self.armed and number(value) and now - self.changed >= self.flat,
                            {"orp": value, "unchanged_seconds": now - self.changed,
                             "reading_age_ms": age, "pump_context": context,
                             "fresh_messages_continue": number(age) and age < self.stale * 1000})
        return events


class Recorder:
    """Two bounded disk buffers plus a bounded, durable active incident."""
    def __init__(self, folder, pre=1800, post=600, clock=time.monotonic,
                 ring_limit=16*1024*1024, incident_limit=16*1024*1024):
        self.folder, self.pre, self.post, self.clock = Path(folder), pre, post, clock
        self.folder.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.ring_limit, self.incident_limit = ring_limit, incident_limit
        self.current, self.previous = self.folder/"rolling-current.jsonl", self.folder/"rolling-previous.jsonl"
        self.ring = self.current.open("ab")
        self.active = self.meta = None
        self.active_bytes = self.serial = 0
        self.closed = False

    def record(self, kind, data):
        now = self.clock()
        line = (json.dumps({"monotonic": now, "captured_at": stamp(), "kind": kind,
                            "data": data}, ensure_ascii=True, separators=(",", ":")) + "\n").encode()
        with self.lock:
            if self.closed:
                return
            if self.ring.tell() + len(line) > self.ring_limit:
                self.ring.close()
                os.replace(self.current, self.previous)
                self.ring = self.current.open("wb")
            self.ring.write(line)
            self.ring.flush()
            if self.active:
                if self.active_bytes + len(line) <= self.incident_limit:
                    self.active.write(line)
                    self.active.flush()
                    self.active_bytes += len(line)
                else:
                    self.meta["size_limit_reached"] = True

    def start(self, reason):
        with self.lock:
            if self.active:
                self.meta["triggers"].append(reason)
                self.record("trigger", reason)
                return
            now = self.clock()
            self.serial += 1
            self.work = self.folder/("incident-%02d" % self.serial)
            self.work.mkdir()
            self.meta = {"started_at": stamp(), "confirmed_g4": False, "triggers": [reason],
                         "pre_seconds_requested": self.pre, "post_seconds_requested": self.post,
                         "size_limit_reached": False, "prehistory_size_limited": False}
            self.ends = now + self.post
            self.active = (self.work/"records.jsonl").open("wb")
            kept, size = deque(), 0
            for path in (self.previous, self.current):
                if not path.exists():
                    continue
                with path.open("rb") as stream:
                    for line in stream:
                        try:
                            if json.loads(line)["monotonic"] < now - self.pre:
                                continue
                        except (ValueError, KeyError):
                            continue
                        kept.append(line)
                        size += len(line)
                        while size > self.incident_limit and kept:
                            size -= len(kept.popleft())
                            self.meta["prehistory_size_limited"] = True
            self.active.writelines(kept)
            self.active.flush()
            self.active_bytes = size
            self.record("trigger", reason)
            (self.work/"incident.json").write_text(json.dumps(self.meta, indent=2), encoding="utf-8")

    def due(self):
        with self.lock:
            return bool(self.active and (self.clock() >= self.ends or self.meta["size_limit_reached"]))

    def finish(self, stopped=False):
        with self.lock:
            if not self.active:
                return None
            self.active.close()
            self.active = None
            work, meta = self.work, self.meta
            meta.update(ended_at=stamp(), watcher_stopped=stopped)
            self.meta = None
        # Stream to compatible capture files; TCP recording continues in the ring.
        with (work/"bus-timeline.txt").open("w", encoding="utf-8") as bus, \
             (work/"status-samples.jsonl").open("w", encoding="utf-8") as status, \
             (work/"capture-health.jsonl").open("w", encoding="utf-8") as health:
            with (work/"records.jsonl").open(encoding="utf-8") as records:
                for line in records:
                    item = json.loads(line)
                    if item["kind"] == "bus":
                        bus.write(item["captured_at"] + " " + item["data"] + "\n")
                    else:
                        target = status if item["kind"] == "status" else health
                        target.write(line)
        (work/"incident.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
        destination = self.folder/(work.name + ".zip")
        temporary = destination.with_suffix(".zip.tmp")
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as archive:
            for name in ("bus-timeline.txt", "status-samples.jsonl", "capture-health.jsonl", "incident.json"):
                archive.write(work/name, name)
        os.replace(temporary, destination)
        shutil.rmtree(work)
        return destination

    def close(self):
        with self.lock:
            self.closed = True
            self.ring.close()


def get_status(base, timeout=5):
    # LAN addresses must not be sent through the PC's configured web proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(base + "/status", timeout=timeout) as response:
        raw = response.read(1024*1024 + 1)
    if len(raw) > 1024*1024:
        raise ValueError("Status response too large")
    result = json.loads(raw)
    if not isinstance(result, dict) or "chlorinator" not in result:
        raise ValueError("Response is not a pool controller status object")
    return result


def http_worker(base, interval, stop, recorder, messages):
    while not stop.is_set():
        try:
            status = get_status(base)
            recorder.record("status", status)
            message = (True, status, time.monotonic())
        except Exception as error:
            recorder.record("http_error", str(error))
            message = (False, str(error), time.monotonic())
        try:
            messages.put_nowait(message)
        except queue.Full:
            recorder.record("watcher_gap", "Status analysis queue full; raw status still recorded")
        stop.wait(interval)


def tcp_worker(host, port, stop, recorder, counters):
    delay = 5
    last_notice = float("-inf")
    raw_line = re.compile(r"^(?:[0-9A-Fa-f]{2}\s+){10,}[0-9A-Fa-f]{2}\s*$")
    while not stop.is_set():
        try:
            with socket.create_connection((host, port), timeout=5) as connection:
                connection.settimeout(1)
                recorder.record("tcp_connected", {"port": port})
                pending, last_raw = b"", time.monotonic()
                counters["connected"] = True
                while not stop.is_set():
                    try:
                        chunk = connection.recv(4096)
                    except socket.timeout:
                        chunk = None
                    if chunk == b"":
                        raise ConnectionError("Debug connection closed")
                    if chunk:
                        pending += chunk
                        while b"\n" in pending:
                            line, pending = pending.split(b"\n", 1)
                            text = line.decode("ascii", errors="replace").rstrip("\r")
                            recorder.record("bus", text)
                            if raw_line.fullmatch(text.strip()):
                                counters["frames"] += 1
                                if counters["frames"] == 1:
                                    print("Raw bus frames confirmed.", flush=True)
                                last_raw = time.monotonic()
                                delay = 5
                        if len(pending) > 65536:
                            recorder.record("tcp_error", "Oversized unfinished debug line discarded")
                            pending = b""
                    if time.monotonic() - last_raw > 90:
                        raise TimeoutError("No raw bus frames for 90 seconds; close other debug clients")
        except Exception as error:
            recorder.record("tcp_error", str(error))
            counters["connected"] = False
            if time.monotonic() - last_notice >= 60:
                print("DEBUG CAPTURE UNAVAILABLE: %s. Retrying automatically." % error, flush=True)
                last_notice = time.monotonic()
            stop.wait(delay)
            delay = min(delay * 2, 30)


def desktop():
    if os.name == "nt":
        import ctypes
        buffer = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buffer) == 0:
            return Path(buffer.value)
    return Path.home()/"Desktop" if (Path.home()/"Desktop").exists() else Path.cwd()


def request_awake():
    """Temporarily inhibit Windows idle sleep on this thread, not lid actions."""
    if os.name != "nt":
        return lambda: None
    try:
        import ctypes
        function = ctypes.windll.kernel32.SetThreadExecutionState
        function.argtypes, function.restype = [ctypes.c_uint], ctypes.c_uint
        previous = function(0x80000001)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
        if previous:
            print("Windows idle sleep inhibited while watching; leave a laptop lid open.", flush=True)
            return lambda: function(previous | 0x80000000)
    except Exception as error:
        print("Could not request Windows awake state: %s" % error, flush=True)
    print("Keep this PC awake manually for the capture.", flush=True)
    return lambda: None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", required=True, help="Controller IP or HTTP URL")
    parser.add_argument("--hours", type=float, default=24)
    parser.add_argument("--flat-minutes", type=float, default=15)
    parser.add_argument("--stale-minutes", type=float, default=5)
    parser.add_argument("--warmup-minutes", type=float, default=3)
    parser.add_argument("--pre-minutes", type=float, default=30)
    parser.add_argument("--post-minutes", type=float, default=10)
    parser.add_argument("--poll-seconds", type=float, default=15)
    parser.add_argument("--debug-port", type=int, default=7373)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    values = (args.hours, args.flat_minutes, args.stale_minutes, args.warmup_minutes,
              args.pre_minutes, args.post_minutes, args.poll_seconds)
    if any(not math.isfinite(value) or value <= 0 for value in values) or args.hours > 168 or args.pre_minutes > 120 or args.post_minutes > 60:
        parser.error("Use positive finite limits: at most 168 hours, 120 pre-minutes and 60 post-minutes")
    if not 5 <= args.poll_seconds <= 30 or not 1 <= args.debug_port <= 65535:
        parser.error("Poll interval must be 5-30 seconds and debug port must be 1-65535")
    controller = args.controller if "://" in args.controller else "http://" + args.controller
    parsed = urlsplit(controller)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password:
        parser.error("Use a controller HTTP address without embedded credentials")
    base = parsed.scheme + "://" + parsed.netloc
    folder = args.output or desktop()/("PoolChlorinatorWatch-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
    if folder.exists() and any(folder.iterdir()):
        parser.error("Choose an empty output folder")
    recorder = Recorder(folder, args.pre_minutes*60, args.post_minutes*60)
    detector = Detector(args.flat_minutes*60, args.stale_minutes*60, args.warmup_minutes*60)
    stop, messages = threading.Event(), queue.Queue(maxsize=8)
    counters = {"frames": 0, "connected": False}
    threads = [threading.Thread(target=http_worker, args=(base, args.poll_seconds, stop, recorder, messages), daemon=True),
               threading.Thread(target=tcp_worker, args=(parsed.hostname, args.debug_port, stop, recorder, counters), daemon=True)]
    for worker in threads:
        worker.start()
    restore_sleep = request_awake()
    print("Watching for %.1f hours. Output: %s" % (args.hours, folder), flush=True)
    print("No equipment commands or automatic resets. Keep this PC awake and this window open.", flush=True)
    print("Close other PuTTY/debug/capture clients. Triggers mean suspected faults, not confirmed G4.", flush=True)
    end, progress, cooldown, incidents = time.monotonic()+args.hours*3600, 0, 0, 0
    latest = None
    exit_code = 0
    try:
        while time.monotonic() < end:
            if any(not worker.is_alive() for worker in threads):
                raise RuntimeError("A recording worker stopped; stopping to avoid a silent failed capture")
            try:
                ok, data, now = messages.get(timeout=1)
                if ok:
                    if latest is None:
                        print("HTTP status confirmed; ORP monitoring is active.", flush=True)
                    latest = data
                    events = detector.observe(data, now)
                else:
                    if detector.failed_since is None:
                        print("HTTP STATUS UNAVAILABLE: %s. Retrying automatically." % data, flush=True)
                    events = detector.failure(now)
                for event in events:
                    recorder.record("detector_event", event)
                    if recorder.active or (time.monotonic() >= cooldown and incidents < 6):
                        if not recorder.active:
                            incidents += 1
                            cooldown = time.monotonic()+1800
                            print("Suspected event: %s. Saving pre-history and recording aftermath." % event["code"], flush=True)
                        recorder.start(event)
            except queue.Empty:
                pass
            if recorder.due():
                print("CAPTURE SAVED: %s" % recorder.finish(), flush=True)
            if time.monotonic() >= progress:
                chemistry = (latest or {}).get("chlorinator") or {}
                print("%s ORP=%s; sample age=%s ms; raw frames=%s; debug connected=%s; captures=%s/6" %
                      (stamp(), chemistry.get("orp_reading"), chemistry.get("orp_reading_age_ms"),
                       counters["frames"], counters["connected"], incidents), flush=True)
                if not counters["frames"] and progress:
                    print("WARNING: No raw bus evidence yet. Check the debug connection; HTTP monitoring continues.", flush=True)
                progress = time.monotonic()+300
    except KeyboardInterrupt:
        print("Stopping; preserving the current evidence.", flush=True)
    except Exception as error:
        exit_code = 1
        print("WATCHER ERROR: %s" % error, flush=True)
    finally:
        try:
            stop.set()
            for worker in threads:
                worker.join(timeout=6)
            if recorder.active:
                print("CAPTURE SAVED: %s" % recorder.finish(stopped=True), flush=True)
            elif incidents == 0:
                recorder.start({"code": "baseline_at_exit", "confirmed_g4": False,
                                "details": "No incident captured; preserving the latest rolling baseline"})
                print("BASELINE SAVED: %s" % recorder.finish(stopped=True), flush=True)
        finally:
            try:
                recorder.close()
            finally:
                restore_sleep()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
