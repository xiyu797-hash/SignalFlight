#!/usr/bin/env python3
"""Regression: CLI, replay, live UDP and file I/O under non-ASCII Windows paths.

Must run with the native Windows executable, not a Linux test on an ASCII temp
folder alone. No numpy, OpenCV, network access or extra Python packages needed.
"""
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get('SF_BIN_DIR', ROOT / ('build-win' if os.name == 'nt' else 'build')))
SUFFIX = '.exe' if os.name == 'nt' else ''


def binary(name):
    return str(BIN / (name + SUFFIX))


def run(name, *arguments, timeout=30):
    p = subprocess.run([binary(name), *map(str, arguments)], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       encoding='utf-8', errors='replace', timeout=timeout)
    if p.returncode:
        raise AssertionError(f'{name} failed with non-ASCII paths (exit {p.returncode}):\n{p.stderr}\n{p.stdout}')
    return p


def available_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    (ROOT / 'recordings').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sf_unicode_', dir=ROOT / 'recordings') as temp:
        root = pathlib.Path(temp) / '中文目录_空格 test'
        root.mkdir()
        fixture = root / '故障记录'
        shutil.copytree(ROOT / 'examples' / 'sample_incident', fixture)
        metrics_out = root / '回放结果 metrics.json'
        results_out = root / '分析结果 results.jsonl'
        out = run('sf_replay', '--input', fixture, '--metrics-out', metrics_out,
                  '--results-out', results_out)
        assert 'REPLAY ' in out.stdout, out.stdout
        assert metrics_out.is_file() and json.loads(metrics_out.read_text(encoding='utf-8'))
        assert results_out.is_file() and results_out.stat().st_size > 0

        iq = root / '中文 IQ 数据.sigmf-data'
        shutil.copyfile(ROOT / 'examples' / 'qpsk_test.sigmf-data', iq)
        # Actually exercise Windows wchar -> UTF-8 argument conversion for a
        # user-supplied path while opening an IQ file from the sender.
        run('sf_source', '--host', '127.0.0.1', '--port', available_port(),
            '--iq-file', iq, '--frames', 1, '--rate', 100)

        # Exercise live engine paths (ready/stop/record/results/metrics), not
        # only the replay utility, using Chinese names and absolute paths.
        port = available_port()
        ready = root / '启动就绪.flag'
        stop = root / '停止服务.flag'
        recording = root / '飞行记录'
        live_results = root / '在线结果.jsonl'
        live_metrics = root / '在线指标.json'
        args = [binary('sf_engine'), '--host', '127.0.0.1', '--port', str(port),
                '--duration', '12', '--workers', '2', '--queue', '64',
                '--ready-file', str(ready), '--stop-file', str(stop),
                '--record-dir', str(recording), '--capture-start',
                '--results-out', str(live_results), '--metrics-out', str(live_metrics)]
        proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, encoding='utf-8', errors='replace')
        try:
            deadline = time.monotonic() + 8
            while not ready.exists():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError('engine did not create Unicode ready-file')
                time.sleep(0.02)
            run('sf_source', '--port', port, '--frames', 12, '--rate', 150)
            time.sleep(0.30)
        finally:
            if proc.poll() is None:
                stop.touch()
            try:
                stdout, stderr = proc.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
        assert proc.returncode == 0, (stderr, stdout)
        final = next(json.loads(x[6:]) for x in stdout.splitlines() if x.startswith('FINAL '))
        assert final['metrics']['analyzed_full'] == 12, final
        assert live_results.is_file() and len(live_results.read_text(encoding='utf-8').splitlines()) == 12
        assert json.loads(live_metrics.read_text(encoding='utf-8'))['analyzed_full'] == 12
        incident = next(recording.glob('incident_*'))
        run('sf_replay', '--input', incident)
    print('test_unicode_paths PASS: replay, source IQ, live UDP, ready/stop, recorder and metrics')


if __name__ == '__main__':
    main()
