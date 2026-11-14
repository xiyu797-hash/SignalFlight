#!/usr/bin/env python3
"""Cross-platform native UDP receiver restart and graceful-mid-stream shutdown checks.

All Python imports are from the standard library; no numpy, cv2, or AI runtime.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("SF_BIN_DIR", ROOT / ("build-win" if os.name == "nt" else "build")))

def exe(name):
    return str(BIN / (name + (".exe" if os.name == "nt" else "")))

def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def run_one(number, interrupt=False):
    with tempfile.TemporaryDirectory(prefix="sf_restart_") as temp:
        root = Path(temp)
        p = port()
        ready, stop = root / "ready.flag", root / "stop.flag"
        proc = subprocess.Popen([
            exe("sf_engine"), "--host", "127.0.0.1", "--port", str(p),
            "--workers", "3", "--queue", "512", "--duration", "14",
            "--ready-file", str(ready), "--stop-file", str(stop),
            "--record-dir", str(root / "rec"), "--capture-start"
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        source = None
        try:
            deadline = time.monotonic() + 7
            while not ready.is_file():
                if proc.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError("receiver failed before ready signal")
                time.sleep(.02)
            source = subprocess.Popen([
                exe("sf_source"), "--host", "127.0.0.1", "--port", str(p),
                "--frames", "15000" if interrupt else "80",
                "--rate", "1200" if interrupt else "550"
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if interrupt:
                time.sleep(.16)
            else:
                data, errors = source.communicate(timeout=12)
                if source.returncode != 0:
                    raise AssertionError(f"source failed {errors} {data}")
                time.sleep(.35)
            stop.touch()  # request graceful drain while packets may still be incoming
            out, err = proc.communicate(timeout=12)
        finally:
            if source is not None and source.poll() is None:
                source.terminate()
                try:source.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    source.kill();source.communicate()
            if proc.poll() is None:
                proc.kill();proc.communicate()
        if proc.returncode != 0:
            raise AssertionError(f"engine exit={proc.returncode}: {err}\n{out}")
        finals = [json.loads(line[6:]) for line in out.splitlines() if line.startswith("FINAL ")]
        assert len(finals) == 1, "missing or duplicate FINAL metric report"
        m = finals[0]["metrics"]
        assert m["packets_rx"] > 0 and m["analyzed_failed"] == 0, m
        assert m["queue_peak"] <= 512 and m["queue_size"] == 0, m
        accounted = m["analyzed_full"] + m["analyzed_light"] + m["queue_dropped"] + m["analyzed_failed"]
        assert accounted == m["frames_ready"], f"unaccounted work on shutdown {m}"
        assert m["record_write_failed"] == 0 and m["record_completed"] >= 1, m
        if not interrupt:
            assert m["frames_ready"] == 80 and m["analyzed_full"] == 80, m
        captures = sorted((root / "rec").glob("incident_*/packets.bin"))
        assert captures, "recording missing after graceful exit"
        replay = subprocess.run([exe("sf_replay"), "--input", str(captures[0].parent)],
                                capture_output=True, text=True, timeout=10)
        assert replay.returncode == 0, f"replay failed: {replay.stderr}"
        assert not list((root / "rec").glob(".partial_*")), "unfinished recordings after graceful exit"
        print(f"PASS restart {number} ({'midstream' if interrupt else 'normal'}): "
              f"frames={m['frames_ready']}, analyzed={accounted - m['queue_dropped']}, "
              f"dropped={m['queue_dropped']}, captures={len(captures)}", flush=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cycles", type=int, default=4)
    parser.add_argument("--no-midstream", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100:parser.error("cycles must be 1..100")
    for i in range(args.cycles):run_one(i+1)
    if not args.no_midstream:run_one(args.cycles+1, interrupt=True)
    print("PASS stop/restart and midstream graceful shutdown", flush=True)

if __name__ == "__main__":
    main()
