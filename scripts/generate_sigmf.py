#!/usr/bin/env python3
"""Produce a small valid SigMF cf32_le IQ recording without external packages."""
import argparse
import json
import math
import pathlib
import random
import struct


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="examples/qpsk_test")
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--samples", type=int, default=1024)
    parser.add_argument("--sample-rate", type=float, default=1e6)
    a = parser.parse_args()
    if a.frames <= 0 or a.samples <= 0 or a.sample_rate <= 0:
        raise SystemExit("invalid frames/samples/sample-rate")
    root = pathlib.Path(a.out)
    root.parent.mkdir(parents=True, exist_ok=True)
    rng = random.Random(42)
    with root.with_suffix(".sigmf-data").open("wb") as file:
        for _ in range(a.frames * a.samples):
            real = (1 if rng.randrange(2) else -1) / math.sqrt(2)
            imag = (1 if rng.randrange(2) else -1) / math.sqrt(2)
            file.write(struct.pack("<ff", real, imag))
    meta = {"global": {"core:datatype": "cf32_le", "core:sample_rate": a.sample_rate,
                        "core:version": "1.2.0", "core:description": "SignalFlight simulated QPSK"},
            "captures": [{"core:sample_start": 0}], "annotations": []}
    root.with_suffix(".sigmf-meta").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf8")
    print("Wrote", root.with_suffix(".sigmf-data"), "and", root.with_suffix(".sigmf-meta"))


if __name__ == "__main__":
    main()
