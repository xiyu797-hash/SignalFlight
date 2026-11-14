#!/usr/bin/env python3
"""Run all locally available self-contained tests with an auditable JSON report."""
import argparse
import json
import os
import pathlib
import platform
import subprocess
import sys
import time

ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get('SF_BIN_DIR', ROOT / ('build-win' if os.name == 'nt' else 'build')))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--full',action='store_true',help='also run UDP/CRC/DSP benchmarks')
    parser.add_argument('--qualify',action='store_true',help='also run repeated systems qualification')
    parser.add_argument('--report',default=str(ROOT/'benchmarks'/'verification_report.json'))
    args=parser.parse_args()
    suffix='.exe' if os.name=='nt' else ''
    tests=[
        ('package_layout', [sys.executable, str(ROOT/'tests'/'test_package_layout.py')]),
        ('core', [str(BIN/('test_core'+suffix))]),
        ('hardening',[str(BIN/('test_hardening'+suffix))]),
        ('udp_integration',[sys.executable,str(ROOT/'tests'/'test_integration.py')]),
        ('extended_integration',[sys.executable,str(ROOT/'tests'/'test_extended.py')]),
        ('sample_replay',[str(BIN/('sf_replay'+suffix)),'--input',str(ROOT/'examples'/'sample_incident')]),
        ('unicode_paths',[sys.executable,str(ROOT/'tests'/'test_unicode_paths.py')]),
        ('stop_restart',[sys.executable,str(ROOT/'tests'/'test_stop_restart.py'),'--cycles','2']),
    ]
    # Systems Edition has no AI/ONNX dependencies. All listed checks MUST run.
    skipped=[]
    if args.full:
        tests += [
            ('udp_benchmark',[sys.executable,str(ROOT/'scripts'/'benchmark.py'),'--repeat','3']),
            ('dsp_comparison',[sys.executable,str(ROOT/'scripts'/'benchmark_dsp.py'),'--repeat','5']),
            ('crc_comparison',[sys.executable,str(ROOT/'scripts'/'benchmark_crc.py'),'--repeat','5']),
        ]
    if args.qualify:
        tests.append(('qualification',[sys.executable,str(ROOT/'scripts'/'qualify.py')]))
    rows=[]
    for name,command in tests:
        start=time.perf_counter()
        print(f'[VERIFY] {name} ...',flush=True)
        try:
            result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=180)
            ok=result.returncode==0
            row={'test':name,'passed':ok,'exit_code':result.returncode,
                 'elapsed_seconds':round(time.perf_counter()-start,3),
                 'stdout_tail':result.stdout[-5000:],'stderr_tail':result.stderr[-5000:]}
        except (subprocess.TimeoutExpired,OSError) as ex:
            ok=False;row={'test':name,'passed':False,'error':str(ex),
                          'elapsed_seconds':round(time.perf_counter()-start,3)}
        rows.append(row)
        print(f"[VERIFY] {name}: {'PASS' if ok else 'FAIL'} ({row['elapsed_seconds']}s)",flush=True)
        if not ok:
            print(row.get('stderr_tail',row.get('error','unknown error')),file=sys.stderr)
            break
    report={'project':'SignalFlight','version':'1.8.0','generated_epoch_ms':int(time.time()*1000),
            'platform':platform.platform(),'architecture':platform.machine(),
            'python':sys.version.split()[0],'full_benchmarks':args.full,'checks':rows,'skipped_optional_checks':skipped,
            'passed':len(rows)==len(tests) and all(x['passed'] for x in rows)}
    dest=pathlib.Path(args.report);dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f"[VERIFY] {'ALL ENABLED CHECKS PASS' if report['passed'] else 'FAILED'}; optional_skipped={skipped}; report: {dest}",flush=True)
    return 0 if report['passed'] else 1

if __name__=='__main__':
    sys.exit(main())
