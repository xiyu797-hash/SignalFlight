#!/usr/bin/env python3
"""Repeatable local UDP benchmarks; outputs JSON and CSV without third-party packages."""
import argparse
import csv
import json
import pathlib
import os
import tempfile
import socket
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("SF_BIN_DIR", ROOT / ("build-win" if os.name == "nt" else "build")))

def binary(name):
    return str(BIN / (name + (".exe" if os.name == "nt" else "")))


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def scenario(name, extra_source, extra_engine, frames=300, rate=350):
    p = port()
    stop_dir = tempfile.TemporaryDirectory(prefix="sf_bench_stop_")
    stop = pathlib.Path(stop_dir.name) / "stop.flag"
    ready = pathlib.Path(stop_dir.name) / "ready.flag"
    cmd = [binary("sf_engine"), "--stop-file", str(stop), "--ready-file", str(ready), "--host", "127.0.0.1", "--port", str(p), "--duration", "15", *extra_engine]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        until = time.monotonic() + 8
        while not ready.exists():
            if proc.poll() is not None or time.monotonic() >= until:
                raise RuntimeError("receiver did not become ready")
            time.sleep(.02)
        sender = subprocess.run([binary("sf_source"), "--port", str(p), "--frames", str(frames),
                                 "--rate", str(rate), *extra_source], capture_output=True, text=True, timeout=10)
        if sender.returncode:
            raise RuntimeError(sender.stderr)
        time.sleep(.35)
    finally:
        if proc.poll() is None:
            stop.touch()
        try:
            out, err = proc.communicate(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
        stop_dir.cleanup()
    if proc.returncode:
        raise RuntimeError(err)
    report = next(json.loads(line[6:]) for line in out.splitlines() if line.startswith("FINAL "))
    m = report["metrics"]
    return {"scenario": name, "frames_requested": frames, "frame_rate_requested": rate,
            "elapsed_seconds": report["elapsed_seconds"], **m}


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--out", default=str(ROOT / "benchmarks" / "benchmark_results"))
    a.add_argument("--repeat", type=int, default=3)
    args = a.parse_args()
    if not 1 <= args.repeat <= 10:
        raise SystemExit("--repeat must be 1..10")
    cases = [
        ("baseline", [], ["--workers", "2", "--queue", "256"], 300, 350),
        ("weak_network", ["--drop-every", "43", "--duplicate-every", "71", "--swap-every", "4"],
         ["--workers", "2", "--queue", "256", "--batch"], 300, 350),
        ("overloaded", [], ["--workers", "1", "--queue", "16", "--work-us", "4000"], 550, 1500),
    ]
    rows = []
    for repeat in range(args.repeat):
        for name, source, engine, frames, rate in cases:
            row = scenario(name, source, engine, frames, rate)
            row["run"] = repeat + 1
            rows.append(row)
            print(f"[{repeat+1}/{args.repeat}] {name}: complete={row['frames_ready']} "
                  f"lost={row['missing_final']} queue_dropped={row['queue_dropped']} "
                  f"p99_exact_us={row['p99_latency_us']} p99_bucket_us={row['p99_latency_upper_us']}", flush=True)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    with out.with_suffix(".csv").open("w", newline="", encoding="utf8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print("Reports:", out.with_suffix(".json"), out.with_suffix(".csv"))


if __name__ == "__main__":
    main()
