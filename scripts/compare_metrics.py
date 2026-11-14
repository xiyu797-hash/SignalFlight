#!/usr/bin/env python3
"""Compare before/after benchmark or replay metrics. Supports raw metrics JSON or FINAL/REPLAY wrapper JSON."""
import argparse
import json
from pathlib import Path


def load(path):
    val = json.loads(Path(path).read_text(encoding='utf8'))
    if isinstance(val, list):
        raise ValueError("provide one metrics JSON file, not a benchmark collection")
    return val.get("metrics", val)


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument('before')
    cli.add_argument('after')
    a = cli.parse_args()
    old, new = load(a.before), load(a.after)
    print(f"{'Metric':<26} {'Before':>14} {'After':>14} {'Delta':>14} {'Change':>10}")
    for k in sorted(set(old) & set(new)):
        x, y = old[k], new[k]
        if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
            continue
        delta = y - x
        pct = f'{(delta / x * 100):+.1f}%' if x else 'n/a'
        print(f'{k:<26} {x:>14.4g} {y:>14.4g} {delta:>+14.4g} {pct:>10}')
    print('Interpretation requires matching input, environment and time windows; a lower p99 alongside higher drops may not be a net improvement.')


if __name__ == '__main__':
    main()
