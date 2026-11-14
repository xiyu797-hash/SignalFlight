#!/usr/bin/env python3
"""CPU-only DSP comparison: reference 64-point DFT versus radix-2 FFT.

The benchmark is *not* a claim about UDP end-to-end performance; results vary by CPU,
compiler, thermal state, OS scheduler and frame size. Both paths process the same
frames in the same executable with the same flags and numerical checksum.
"""
import argparse
import json
import os
import pathlib
import platform
import statistics
import subprocess
import time

ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get('SF_BIN_DIR', ROOT / ('build-win' if os.name == 'nt' else 'build')))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--repeat',type=int,default=5)
    parser.add_argument('--frames',type=int,default=4000)
    parser.add_argument('--samples',type=int,default=1024)
    parser.add_argument('--out',default=str(ROOT/'benchmarks'/'dsp_comparison.json'))
    args=parser.parse_args()
    if not 1<=args.repeat<=20 or not 100<=args.frames<=1000000 or not 64<=args.samples<=16384:
        parser.error('repeat 1..20; frames 100..1000000; samples 64..16384')
    cmd=str(BIN / ('sf_bench.exe' if os.name=='nt' else 'sf_bench'))
    measured={'fft':[],'reference':[]}
    checksums={}
    for trial in range(args.repeat):
        # Alternate order to reduce warm-up and power-scaling bias.
        order=['reference','fft'] if trial%2==0 else ['fft','reference']
        for backend in order:
            output=subprocess.run([cmd,'--compute-only','--frames',str(args.frames),
                                   '--frame-samples',str(args.samples),'--dsp-backend',backend],
                                  capture_output=True,text=True,timeout=90,check=True)
            lines=[json.loads(x[6:]) for x in output.stdout.splitlines() if x.startswith('BENCH ')]
            if len(lines)!=1:raise RuntimeError('no BENCH JSON emitted: '+output.stdout)
            data=lines[0]
            assert data['mode']=='compute-only' and data['backend']==backend
            measured[backend].append(data['elapsed_seconds'])
            checksums[backend]=data['checksum']
    delta=abs(checksums['fft']-checksums['reference'])
    if delta>1e-9*max(1.0,abs(checksums['reference'])):
        raise AssertionError(f'FFT and reference checksum differ: {checksums}')
    med={name:statistics.median(runs) for name,runs in measured.items()}
    speedup=med['reference']/med['fft']
    report={
        'purpose':'isolated DSP kernel only, not network or system throughput',
        'generated_epoch_ms':int(time.time()*1000),
        'environment':{'platform':platform.platform(),'machine':platform.machine(),
                       'python':platform.python_version(),'processor':platform.processor()},
        'frames_per_trial':args.frames,'samples_per_frame':args.samples,'repeats':args.repeat,
        'seconds_by_trial':measured,'median_seconds':med,
        'speedup_reference_over_fft':speedup,'checksums':checksums,
        'checksum_absolute_difference':delta,
        'caveat':'single-process microbenchmark; no CPU pinning; no guaranteed end-to-end speedup'
    }
    out=pathlib.Path(args.out);out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(f'DSP verified: {args.frames} frames x {args.repeat} repeats; '
          f"median reference={med['reference']:.6f}s, fft={med['fft']:.6f}s, "
          f'speedup={speedup:.2f}x')
    print('Report:',out)

if __name__=='__main__':
    main()
