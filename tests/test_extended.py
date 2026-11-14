#!/usr/bin/env python3
"""Independent end-to-end regression: multi-source UDP, replay identity, corrupted capture and pressure."""
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

def binary(name):
    return str(BIN / (name + ('.exe' if os.name == 'nt' else '')))

def run(name, *args, check=True, timeout=20):
    result = subprocess.run([binary(name), *map(str, args)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if check and result.returncode:
        raise AssertionError(f'{name} failed: stdout={result.stdout}\nstderr={result.stderr}')
    return result

def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

def report(result, prefix):
    return next(json.loads(line[len(prefix):]) for line in result.stdout.splitlines() if line.startswith(prefix))

def test_multisource_replay():
    with tempfile.TemporaryDirectory(prefix='sf_extended_') as folder:
        root=pathlib.Path(folder)
        port=free_port()
        ready=root/'ready.flag';stop=root/'stop.flag'
        proc=subprocess.Popen([binary('sf_engine'), '--host', '127.0.0.1', '--port', str(port),
                               '--workers', '4', '--queue', '512', '--record-dir', str(root/'rec'),
                               '--capture-start', '--results-out', str(root/'online_results.jsonl'),
                               '--ready-file', str(ready), '--stop-file', str(stop), '--duration', '15'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline=time.monotonic()+8
            while not ready.exists():
                if proc.poll() is not None or time.monotonic()>deadline:
                    raise AssertionError('receiver did not become ready')
                time.sleep(.02)
            source=run('sf_source', '--port', port, '--frames', 60, '--sources', 3, '--rate', 250)
            stats=report(source, 'SOURCE ')
            assert stats['send_failed']==0, stats
            time.sleep(.65)
        finally:
            if proc.poll() is None:stop.touch()
            try:out,err=proc.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                proc.kill();out,err=proc.communicate()
        assert proc.returncode==0, f'{out}\n{err}'
        final=next(json.loads(x[6:]) for x in out.splitlines() if x.startswith('FINAL '))
        m=final['metrics']
        assert m['frames_ready']==180 and m['analyzed_full']==180, m
        assert m['duplicates']==0 and m['frames_expired']==0 and m['queue_dropped']==0, m
        assert len((root/'online_results.jsonl').read_text().splitlines())==180
        incident=next((root/'rec').glob('incident_*'))
        # Replay the exact same recording twice and compare every result line.
        results=[]
        for suffix in ('a','b'):
            r=run('sf_replay', '--input', incident, '--results-out', root/f'replay_{suffix}.jsonl',
                  '--metrics-out', root/f'replay_{suffix}.json')
            rep=report(r, 'REPLAY ')
            assert rep['recorded_packets']>0 and rep['metrics']['analyzed_full']>0, rep
            results.append((root/f'replay_{suffix}.jsonl').read_bytes())
        assert results[0]==results[1], 'same input yielded different sequential replay results'
        assert (root/'replay_a.json').read_bytes()==(root/'replay_b.json').read_bytes()
        # Explicitly corrupt one replay packet. Integrity verification must reject it.
        bad=root/'corrupt'
        bad.mkdir()
        raw=bytearray((incident/'packets.bin').read_bytes())
        assert len(raw)>45
        assert raw[:8]==b'SFR0002\n', 'new captures must include the SFR2 footer'
        raw[35]^=0x01
        (bad/'packets.bin').write_bytes(raw)
        invalid=run('sf_replay', '--input', bad, '--results-out', bad/'partial.jsonl', check=False)
        assert invalid.returncode!=0 and 'integrity' in invalid.stderr.lower(), invalid.stderr
        assert not (bad/'partial.jsonl').exists(), 'corrupt recording left partially valid results'
        # Truncation exactly on a record boundary used to be accepted as EOF.
        aligned=root/'aligned_truncation';aligned.mkdir()
        (aligned/'packets.bin').write_bytes((incident/'packets.bin').read_bytes()[:-24])
        truncated=run('sf_replay','--input',aligned,check=False)
        assert truncated.returncode!=0 and 'footer' in truncated.stderr.lower(),truncated.stderr
        print('PASS extended multi-source: 180/180; replay twice identical; corrupt replay rejected')

def test_malformed_network_and_pressure():
    # Oversized and malformed UDP traffic must not crash the receiver. This is
    # especially important on Windows: Winsock reports WSAEMSGSIZE for a
    # datagram larger than recvfrom's buffer.
    with tempfile.TemporaryDirectory(prefix='sf_malformed_') as folder:
        root=pathlib.Path(folder);p=free_port();ready=root/'ready';stop=root/'stop'
        proc=subprocess.Popen([binary('sf_engine'),'--host','127.0.0.1','--port',str(p),
                               '--duration','15','--ready-file',str(ready),'--stop-file',str(stop),
                               '--record-dir',str(root/'capture'),'--capture-start'],
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            until=time.monotonic()+8
            while not ready.exists():
                if proc.poll() is not None or time.monotonic()>until:raise AssertionError('malformed receiver not ready')
                time.sleep(.02)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
                for i in range(12):
                    wire=os.urandom(2300 if i==0 else 100)
                    sock.sendto(wire,('127.0.0.1',p));time.sleep(.005)
            run('sf_source','--port',p,'--frames',20,'--rate',150)
            time.sleep(.35)
        finally:
            if proc.poll() is None:stop.touch()
            try:out,err=proc.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                proc.kill();out,err=proc.communicate()
        assert proc.returncode==0,f'oversized UDP caused server failure: {err}\n{out}'
        m=next(json.loads(x[6:]) for x in out.splitlines() if x.startswith('FINAL '))['metrics']
        assert m['bad_packets']>=12 and m['frames_ready']==20 and m['analyzed_full']==20, m
        incident=next((root/'capture').glob('incident_*'))
        # Malformed UDP frames are evidence, and the recorder must preserve
        # them in a valid replay file rather than writing an unreadable incident.
        replay=run('sf_replay','--input',incident)
        rm=report(replay,'REPLAY ')['metrics']
        assert rm['bad_packets']>=10,rm
        assert not list((root/'capture').glob('.partial_*')),'successful capture left unfinished staging directory'
    print('PASS malformed UDP: oversized/invalid datagrams rejected; 20 valid frames still complete')

    # Controlled internal stress (does not rely on the OS UDP buffering rate).
    p=run('sf_bench', '--frames', 12000, '--queue', 16, '--workers', 2)
    m=report(p,'BENCH ')['metrics']
    assert m['frames_ready']==12000 and m['queue_peak']<=16, m
    assert m['analyzed_full']+m['analyzed_light']+m['queue_dropped']+m['analyzed_failed']==m['frames_ready'], m
    print(f"PASS pressure: 12000 frames, queue peak={m['queue_peak']}, dropped={m['queue_dropped']}")
    # Systems Edition rejects AI-only options explicitly; never pretend inference occurred.
    no_model=run('sf_replay','--input',ROOT/'examples'/'sample_incident',
                 '--onnx-model',ROOT/'examples'/'not_a_real_model.onnx',check=False)
    assert no_model.returncode!=0, 'missing model must fail explicitly'
    print('PASS unsupported inference flag fails explicitly')

def test_signal_source_modes():
    # Exercise all built-in modulations and raw cf32 file replay with wrapping.
    cases=[
        ('bpsk',['--modulation','bpsk','--snr-db','12'],24),
        ('qam',['--modulation','16qam','--snr-db','24','--jitter-ms','1','--burst-every','5'],24),
        ('raw_iq_loop',['--iq-file',str(ROOT/'examples'/'qpsk_test.sigmf-data'),'--loop'],32),
    ]
    for name,extra,frames in cases:
        with tempfile.TemporaryDirectory(prefix='sf_modes_') as folder:
            d=pathlib.Path(folder);port=free_port();ready=d/'ready';stop=d/'stop'
            proc=subprocess.Popen([binary('sf_engine'),'--host','127.0.0.1','--port',str(port),
                                   '--duration','15','--ready-file',str(ready),'--stop-file',str(stop),
                                   '--workers','2','--queue','64'],
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                limit=time.monotonic()+8
                while not ready.exists():
                    if proc.poll() is not None or time.monotonic()>limit:raise AssertionError('engine not ready')
                    time.sleep(.02)
                run('sf_source','--port',port,'--frames',frames,'--rate',150,*extra)
                time.sleep(.25)
            finally:
                if proc.poll() is None:stop.touch()
                try:out,err=proc.communicate(timeout=12)
                except subprocess.TimeoutExpired:
                    proc.kill();out,err=proc.communicate()
            assert proc.returncode==0,err
            m=next(json.loads(s[6:]) for s in out.splitlines() if s.startswith('FINAL '))['metrics']
            assert m['frames_ready']==frames and m['analyzed_full']==frames and m['analyzed_failed']==0,m
        print(f'PASS signal source {name}: {frames}/{frames} frames')

def test_source_restart_session():
    # A source process restarts and resets seq/frame IDs to zero.  Auto-generated
    # stream IDs must keep the two sessions logically independent.
    with tempfile.TemporaryDirectory(prefix='sf_restart_') as folder:
        root=pathlib.Path(folder); port=free_port(); ready=root/'ready';stop=root/'stop'
        proc=subprocess.Popen([binary('sf_engine'),'--host','127.0.0.1','--port',str(port),
                               '--duration','18','--ready-file',str(ready),'--stop-file',str(stop),
                               '--workers','2','--queue','128'],stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,text=True)
        try:
            until=time.monotonic()+8
            while not ready.exists():
                if proc.poll() is not None or time.monotonic()>until:raise AssertionError('restart receiver not ready')
                time.sleep(.02)
            a=report(run('sf_source','--port',port,'--frames',25,'--rate',250),'SOURCE ')
            time.sleep(.02)
            b=report(run('sf_source','--port',port,'--frames',25,'--rate',250),'SOURCE ')
            assert a['stream_id']!=b['stream_id'],(a,b)
            time.sleep(.3)
        finally:
            if proc.poll() is None:stop.touch()
            out,err=proc.communicate(timeout=12)
        assert proc.returncode==0,err
        m=next(json.loads(x[6:]) for x in out.splitlines() if x.startswith('FINAL '))['metrics']
        assert m['frames_ready']==50 and m['analyzed_full']==50 and m['frames_duplicate']==0,m
    print('PASS source restart: two auto sessions, 50/50 frames')

if __name__=='__main__':
    test_multisource_replay()
    test_malformed_network_and_pressure()
    test_signal_source_modes()
    test_source_restart_session()
    print('ALL EXTENDED TESTS PASS')
