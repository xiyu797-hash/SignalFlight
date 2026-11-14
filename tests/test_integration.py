#!/usr/bin/env python3
"""Local UDP integration tests. Needs only Python standard library and the compiled binaries."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("SF_BIN_DIR", ROOT / ("build-win" if os.name == "nt" else "build")))

def binary(name):
    return str(BIN / (name + (".exe" if os.name == "nt" else "")))


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_case(name, source_args, engine_args, *, verify=None):
    port = free_udp_port()
    with tempfile.TemporaryDirectory(prefix="sf_integration_") as tmp:
        base = pathlib.Path(tmp)
        stop = base / "stop.flag"
        ready = base / "ready.flag"
        server = [binary("sf_engine"), "--stop-file", str(stop), "--ready-file", str(ready), "--host", "127.0.0.1", "--port", str(port),
                  "--duration", "15", "--record-dir", str(base / "rec"), "--capture-start", *engine_args]
        proc = subprocess.Popen(server, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            until = time.monotonic() + 8
            while not ready.exists():
                if proc.poll() is not None or time.monotonic() >= until:
                    raise RuntimeError("receiver did not become ready")
                time.sleep(.02)
            source = subprocess.run([binary("sf_source"), "--host", "127.0.0.1", "--port", str(port), *source_args],
                                    capture_output=True, text=True, timeout=10)
            if source.returncode:
                raise AssertionError(f"source failed: {source.stderr}")
            time.sleep(.4)
        finally:
            if proc.poll() is None:
                stop.touch()
            try:
                out, err = proc.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate()
        if proc.returncode:
            raise AssertionError(f"engine failed: {err}\n{out}")
        prefix = "FINAL "
        finals = [json.loads(line[len(prefix):]) for line in out.splitlines() if line.startswith(prefix)]
        if len(finals) != 1:
            raise AssertionError(f"no final report: {out}\n{err}")
        metrics = finals[0]["metrics"]
        assert metrics["packets_rx"] > 0, metrics
        assert metrics["analyzed_failed"] == 0, metrics
        assert metrics["queue_peak"] <= int(read_arg(engine_args, "--queue", "256")), metrics
        recordings = list((base / "rec").glob("incident_*/packets.bin"))
        assert recordings, f"capture not generated: {out}"
        replay = subprocess.run([binary("sf_replay"), "--input", str(recordings[0].parent)],
                                capture_output=True, text=True, timeout=10)
        assert replay.returncode == 0, replay.stderr
        report = [json.loads(line[len("REPLAY "):]) for line in replay.stdout.splitlines() if line.startswith("REPLAY ")]
        assert len(report) == 1 and report[0]["recorded_packets"] > 0
        if verify:
            verify(metrics)
        print(f"PASS {name}: packets={metrics['packets_rx']} frames={metrics['frames_ready']} "
              f"drops={metrics['queue_dropped']} reordered={metrics['reordered']} incidents={len(recordings)}")


def read_arg(args, key, default):
    if key in args:
        return args[args.index(key) + 1]
    return default


def verify_normal(m):
    assert m["frames_ready"] == 120, m
    assert m["analyzed_full"] == 120, m
    assert m["bad_packets"] == 0 and m["missing_final"] == 0 and m["frames_expired"] == 0, m


def verify_fault(m):
    assert m["reordered"] > 0 and m["bad_packets"] > 0 and m["duplicates"] > 0, m
    assert m["missing_final"] > 0 and m["frames_expired"] > 0, m


def verify_overload(m):
    assert m["queue_dropped"] > 0, m
    assert m["analyzed_light"] > 0 or m["queue_dropped"] > 0, m


if __name__ == "__main__":
    run_case("normal", ["--frames", "120", "--rate", "300"], ["--workers", "2", "--queue", "128"], verify=verify_normal)
    run_case("faults", ["--frames", "130", "--rate", "250", "--drop-every", "31",
                        "--duplicate-every", "47", "--swap-every", "3", "--corrupt-every", "79"],
             ["--workers", "2", "--queue", "128", "--batch"], verify=verify_fault)
    run_case("overload", ["--frames", "450", "--rate", "1500"],
             ["--workers", "1", "--queue", "8", "--work-us", "5000"], verify=verify_overload)
    print("ALL INTEGRATION TESTS PASS")
