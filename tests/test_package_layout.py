#!/usr/bin/env python3
"""Check files required by launch scripts and release layout.

This deliberately checks presence rather than MANIFEST.sha256: runtime benchmark
reports are intentionally updated during testing and would alter their hashes.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
required = [
    'CMakeLists.txt',
    'scripts/build_windows.ps1',
    'scripts/run_windows.ps1',
    'scripts/run_demo.py',
    'scripts/run_demo.sh',
    'scripts/verify_all.py',
    'scripts/benchmark.py',
    'scripts/benchmark_dsp.py',
    'scripts/benchmark_crc.py',
    'apps/sf_crc_bench.cpp',
    'tests/test_core.cpp',
    'tests/test_hardening.cpp',
    'tests/test_integration.py',
    'tests/test_extended.py',
    'tests/test_unicode_paths.py',
    'docs/WINDOWS_POWERSHELL.md',
    'docs/INTERVIEW_GUIDE.md',
    'docs/validation_v18.md',
    'scripts/qualify.py',
    'tests/test_stop_restart.py',
    'examples/sample_incident/packets.bin',
    '.github/workflows/cross-platform.yml',
]
missing = [f for f in required if not (root / f).is_file()]
if missing:
    raise SystemExit('Missing required release files: ' + ', '.join(missing))
if (root / 'examples/sample_incident/packets.bin').read_bytes()[:8] != b'SFR0002\n':
    raise SystemExit('Sample recording must use verified SFR0002 format')
print(f'PASS package layout: {len(required)} essential files present')
