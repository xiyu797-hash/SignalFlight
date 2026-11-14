#!/usr/bin/env python3
"""Repeatable no-dependency systems qualification (bounded queue and restart).

This is an extended engineering soak, not a claim of weeks of production uptime.
Run from project root after compilation. Results saved in benchmarks/.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
BIN=Path(os.environ.get("SF_BIN_DIR", ROOT/("build-win" if os.name=="nt" else "build")))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--rounds",type=int,default=8)
    parser.add_argument("--frames",type=int,default=4000)
    parser.add_argument("--restart-cycles",type=int,default=4)
    parser.add_argument("--report",default=str(ROOT/"benchmarks"/"qualification_report.json"))
    args=parser.parse_args()
    if not 1<=args.rounds<=100 or not 100<=args.frames<=200000 or not 1<=args.restart_cycles<=100:
        parser.error("rounds 1..100; frames 100..200000; restart-cycles 1..100")
    exe=BIN/("sf_bench.exe" if os.name=="nt" else "sf_bench")
    start=time.perf_counter()
    rows=[]
    try:
        for i in range(args.rounds):
            command=[str(exe),"--frames",str(args.frames),"--queue",str(args.frames+32),"--workers","4"]
            result=subprocess.run(command,text=True,capture_output=True,timeout=120)
            if result.returncode:
                raise AssertionError(f"round {i+1}: {result.stderr}")
            lines=[json.loads(line[6:]) for line in result.stdout.splitlines() if line.startswith("BENCH ")]
            assert len(lines)==1,f"BENCH summary not emitted: {result.stdout}"
            report=lines[0];m=report["metrics"]
            assert m["frames_ready"]==args.frames, m
            assert m["queue_dropped"]==0 and m["analyzed_failed"]==0, m
            assert m["analyzed_full"]==args.frames and m["queue_size"]==0, m
            assert m["queue_peak"]<=args.frames+32, m
            fps=report["incoming_fps"]
            rows.append({"round":i+1,"frames":args.frames,"fps":fps,"seconds":report["elapsed_seconds"],
                         "queue_peak":m["queue_peak"],"p99_us":m.get("p99_latency_us")})
            print(f"[QUALIFY] pipeline {i+1}/{args.rounds}: {fps:.0f} frames/s, complete={args.frames}",flush=True)
        stoptest=subprocess.run([sys.executable,str(ROOT/"tests/test_stop_restart.py"),
                                 "--cycles",str(args.restart_cycles)],
                                capture_output=True,text=True,timeout=max(120,args.restart_cycles*12))
        if stoptest.returncode:
            raise AssertionError(f"native stop/restart test failed: {stoptest.stderr}\n{stoptest.stdout}")
        print(stoptest.stdout,flush=True)
        results={"project":"SignalFlight","edition":"Systems 1.8","platform":platform.platform(),
                 "rounds":rows,"median_fps":statistics.median(x["fps"] for x in rows),
                 "total_frames_tested":args.rounds*args.frames,
                 "restart_cycles":args.restart_cycles,"midstream_stop_tested":True,
                 "elapsed_seconds":round(time.perf_counter()-start,2),"passed":True}
    except (Exception,AssertionError) as ex:
        results={"project":"SignalFlight","edition":"Systems 1.8","platform":platform.platform(),
                 "rounds":rows,"elapsed_seconds":round(time.perf_counter()-start,2),
                 "passed":False,"error":str(ex)}
        print(f"[QUALIFY] FAILED: {ex}",file=sys.stderr,flush=True)
    out=Path(args.report);out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(results,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print(f"[QUALIFY] {'PASS' if results['passed'] else 'FAIL'} report: {out}",flush=True)
    return 0 if results["passed"] else 1

if __name__=='__main__':sys.exit(main())
