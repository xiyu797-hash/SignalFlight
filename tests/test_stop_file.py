#!/usr/bin/env python3
"""The receiver must honour the stop file and shut down cleanly when idle."""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='signalflight-stop-') as tmp:
    root = pathlib.Path(tmp)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    ready, stop = root/'ready', root/'stop'
    process = subprocess.Popen([sys.argv[1], '--host', '127.0.0.1', '--port', str(port),
                                '--duration', '10', '--ready-file', str(ready), '--stop-file', str(stop)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic()+5
        while not ready.exists():
            if process.poll() is not None or time.monotonic()>deadline:
                raise RuntimeError('receiver failed to become ready')
            time.sleep(.01)
        start=time.monotonic()
        stop.touch()
        stdout, stderr=process.communicate(timeout=2)
        assert process.returncode==0, stderr
        assert time.monotonic()-start<2 and 'FINAL ' in stdout
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
