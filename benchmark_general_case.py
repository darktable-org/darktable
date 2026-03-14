#!/usr/bin/env python3
import subprocess
import re
import sys
import os
import glob

def run_test(xmp_name):
    cmd = [
        os.path.expanduser("~/darktable-build/bin/darktable-cli"),
        "/workspace/darktable/diffuse-perf-test-files/DSC_9034.NEF",
        f"/workspace/darktable/diffuse-perf-test-files/general_{xmp_name}.xmp",
        "/workspace/darktable/diffuse-perf-test-files",
        "--core",
        "--disable-opencl",
        "-d", "perf",
        "--configdir", "/tmp/darktable-perftest/"
    ]
    
    process = subprocess.run(cmd, capture_output=True, text=True)
    
    diffuse_times = []
    for line in process.stdout.splitlines():
        if "processed `diffuse" in line and "on CPU" in line:
            m = re.search(r"took (\d+\.\d+) secs", line)
            if m:
                diffuse_times.append(float(m.group(1)))
                
    for f in glob.glob("/workspace/darktable/diffuse-perf-test-files/*.jpg"):
        try:
            os.remove(f)
        except OSError:
            pass
            
    return diffuse_times

def benchmark_case(case_name):
    print(f"Benchmarking Test Case {case_name}...")
    best_time = None
    best_time_at_last_improvement = None
    run_idx = 1
    last_improved = 0
    
    while True:
        times = run_test(case_name)
        if not times:
            print(f"  run {run_idx}: FAILED")
            run_idx += 1
            continue
            
        t = times[0]
        if best_time is None:
            best_time = t
            best_time_at_last_improvement = t
            last_improved = run_idx
            print(f"  run {run_idx}: {t:.3f} -> best so far: {best_time:.3f}; last improved in run {last_improved}")
        else:
            improved = False
            if t < best_time:
                best_time = t
                improved = True
                
            percent_improvement = ((best_time_at_last_improvement - best_time) / best_time_at_last_improvement) * 100.0
            
            if improved and percent_improvement >= 0.1:
                last_improved = run_idx
                best_time_at_last_improvement = best_time
                
            print(f"  run {run_idx}: {t:.3f} -> best so far: {best_time:.3f}; last improved in run {last_improved}")
            
            if run_idx - last_improved >= 5:
                print(f"  (stopping for {case_name}, improvement {percent_improvement:.3f}% < 0.1% in last 5 runs)")
                break
        
        run_idx += 1
    return best_time

def main():
    test_cases = ["G1", "G2", "G3"]
    results = {}
    
    for case in test_cases:
        results[case] = benchmark_case(case)

    total_sum = sum(results.values())
    print("\n--- General Case Benchmark Results ---")
    for case in test_cases:
        print(f"Test Case {case}: {results[case]:.3f}s")
    print(f"TOTAL SUM: {total_sum:.3f}s")

if __name__ == "__main__":
    main()
