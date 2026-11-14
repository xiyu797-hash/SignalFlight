#!/usr/bin/env python3
"""Two-path CRC32 comparison on identical data, same compiler/flags, repeated runs."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]
BIN=Path(os.environ.get('SF_BIN_DIR', ROOT / ('build-win' if os.name == 'nt' else 'build')))

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--frames',type=int,default=4000)
    p.add_argument('--repeat',type=int,default=5)
    p.add_argument('--out',default=str(ROOT/'benchmarks'/'crc_comparison.json'))
    a=p.parse_args()
    if not 1<=a.repeat<=20 or not 100<=a.frames<=100000:
        p.error('repeat 1..20, frames 100..100000')
    exe=BIN/('sf_crc_bench.exe' if os.name=='nt' else 'sf_crc_bench')
    trials={'table':[],'reference':[]}
    for round_id in range(a.repeat):
        order=['table','reference'] if round_id%2 else ['reference','table']
        checksums={}
        for backend in order:
            r=subprocess.run([str(exe),'--frames',str(a.frames),'--backend',backend],
                             capture_output=True,text=True,check=True,timeout=100)
            line=next(x for x in r.stdout.splitlines() if x.startswith('CRC_BENCH '))
            result=json.loads(line[len('CRC_BENCH '):])
            trials[backend].append(result['elapsed_seconds'])
            checksums[backend]=result['checksum']
        if checksums['table']!=checksums['reference']:
            raise AssertionError(f'CRC reference mismatch in round {round_id+1}: {checksums}')
    median={key:statistics.median(t) for key,t in trials.items()}
    output={'purpose':'isolated CRC32 kernel; NOT end-to-end UDP speedup',
            'platform':platform.platform(),'frames_per_trial':a.frames,'repeats':a.repeat,
            'seconds_by_trial':trials,'median_seconds':median,
            'speedup_reference_over_table':median['reference']/median['table'],
            'verified_equal_checksums':True,'generated_epoch_ms':int(time.time()*1000)}
    dst=Path(a.out);dst.parent.mkdir(parents=True,exist_ok=True)
    dst.write_text(json.dumps(output,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(f"CRC verified: {a.frames} datagrams x {a.repeat}; reference={median['reference']:.6f}s "
          f"table={median['table']:.6f}s speedup={output['speedup_reference_over_table']:.2f}x")
    print('Report:',dst)
if __name__=='__main__':main()
