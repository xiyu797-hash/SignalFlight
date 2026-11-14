#!/usr/bin/env python3
"""One foreground command: run the receiver, generate traffic, stop cleanly and replay."""
import argparse
import json
import pathlib
import os
import socket
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("SF_BIN_DIR", ROOT / ("build-win" if os.name == "nt" else "build")))

def binary(name):
    return str(BIN / (name + (".exe" if os.name == "nt" else "")))


def find_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    ap = argparse.ArgumentParser(description="SignalFlight all-in-one foreground demonstration")
    ap.add_argument("--fault", action="store_true", help="inject packet loss, duplication and reordering")
    ap.add_argument("--overload", action="store_true", help="force queue overload using slow analysis")
    ap.add_argument("--no-replay", action="store_true")
    a = ap.parse_args()
    port = find_port()
    folder = ROOT / "recordings" / f"demo_{time.time_ns()}"
    stop = folder / "stop.flag"
    ready = folder / "ready.flag"
    folder.mkdir(parents=True, exist_ok=True)
    engine = [binary("sf_engine"), "--host", "127.0.0.1", "--port", str(port),
              "--duration", "15", "--batch", "--stop-file", str(stop), "--ready-file", str(ready), "--record-dir", str(folder), "--capture-start"]
    source = [binary("sf_source"), "--host", "127.0.0.1", "--port", str(port),
              "--frames", "140", "--rate", "250"]
    if a.fault:
        source += ["--swap-every", "3", "--drop-every", "29", "--duplicate-every", "37", "--corrupt-every", "79"]
    if a.overload:
        source[source.index("--frames") + 1] = "500"
        source[source.index("--rate") + 1] = "1500"
        engine += ["--queue", "8", "--workers", "1", "--work-us", "5000"]
    print("\n[SignalFlight] receiver starting; use Ctrl+C to stop\n", flush=True)
    proc = subprocess.Popen(engine)
    try:
        until = time.monotonic() + 8
        while not ready.exists():
            if proc.poll() is not None or time.monotonic() >= until:
                raise RuntimeError("receiver did not become ready")
            time.sleep(.02)
        subprocess.run(source, check=True)
        time.sleep(.45)
    except KeyboardInterrupt:
        print("\n[SignalFlight] interrupted", flush=True)
    finally:
        if proc.poll() is None:
            stop.touch()
        try:
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        stop.unlink(missing_ok=True)
        ready.unlink(missing_ok=True)
    if proc.returncode:
        raise SystemExit(f"receiver returned {proc.returncode}")
    inc = sorted(folder.glob("incident_*/packets.bin"))
    print(f"\n[SignalFlight] incident recordings: {len(inc)} at {folder}", flush=True)
    if inc and not a.no_replay:
        print("[SignalFlight] replaying first capture:", flush=True)
        subprocess.run([binary("sf_replay"), "--input", str(inc[0].parent)], check=True)
    print("[SignalFlight] demo completed; all processes exited", flush=True)


if __name__ == "__main__":
    main()
