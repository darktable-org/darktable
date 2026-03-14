#!/usr/bin/env python3
import subprocess
import re
import sys
import argparse
import glob
import os

def run_test(mode="cpu"):
    cmd = [
        os.path.expanduser("~/darktable-build/bin/darktable-cli"),
        "/workspace/darktable/diffuse-perf-test-files/DSC_9034.NEF",
        "/workspace/darktable/diffuse-perf-test-files/DSC_9034.NEF.xmp",
        "/workspace/darktable/diffuse-perf-test-files",
        "--core",
        "-d", "perf",
        "-d", "opencl",
        "--configdir", "/tmp/darktable-perftest/"
    ]
    if mode == "cpu":
        cmd.insert(5, "--disable-opencl")
    
    process = subprocess.run(cmd, capture_output=True, text=True)
    
    diffuse_times = []
    search_mode = "on CPU" if mode == "cpu" else "on GPU"
    for line in process.stdout.splitlines():
        if "processed `diffuse" in line and search_mode in line:
            m = re.search(r"took (\d+\.\d+) secs", line)
            if m:
                diffuse_times.append(float(m.group(1)))
                
    for f in glob.glob("/workspace/darktable/diffuse-perf-test-files/*.jpg"):
        try:
            os.remove(f)
        except OSError:
            pass
            
    return diffuse_times

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mode', choices=['cpu', 'gpu'], default='cpu')
    args = parser.parse_args()
    
    global_mode = args.mode
    
    baseline = None
    try:
        with open("/workspace/darktable/diffuse-performance.log", "r") as f:
            lines = f.read().strip().split('\n')
            if lines:
                baseline = float(lines[-1].strip())
    except Exception as e:
        print(f"Warning: Could not read baseline from diffuse-performance.log: {e}")
    
    best_times = None
    best_sum_at_last_improvement = None
    
    run_idx = 1
    last_improved = 0
    
    print(f"Starting new performance measurement for {global_mode.upper()}...")
    while True:
        times = run_test(global_mode)
        
        if len(times) != 3:
            print(f"Warning: Expected 3 diffuse times, got {len(times)}: {times}")
            if len(times) < 3:
                times.extend([float('inf')] * (3 - len(times)))
                
        if best_times is None:
            best_times = times[:3]
            best_sum_at_last_improvement = sum(best_times)
            last_improved = run_idx
            
            times_str = ", ".join(f"{t:g}" for t in times[:3])
            best_str = ", ".join(f"{b:g}" for b in best_times)
            print(f"run {run_idx}: {times_str} -> best so far: {best_str}; last improved in run {last_improved}")
            
            if baseline is not None:
                current_sum = sum(best_times)
                if current_sum > baseline * 1.10:
                    print(f"FAILED FAST: First iteration sum ({current_sum:.3f}s) is >10% worse than baseline ({baseline:.3f}s). Aborting.")
                    sys.exit(1)
            
            print("(we cannot stop, there was an improvement in one of the last 5 steps)")
        else:
            improved_elements = False
            for i in range(3):
                if times[i] < best_times[i]:
                    best_times[i] = times[i]
                    improved_elements = True
            
            current_best_sum = sum(best_times)
            
            percent_improvement = ((best_sum_at_last_improvement - current_best_sum) / best_sum_at_last_improvement) * 100.0
                
            if improved_elements and percent_improvement >= 0.1:
                last_improved = run_idx
                best_sum_at_last_improvement = current_best_sum
                
            times_str = ", ".join(f"{t:g}" for t in times[:3])
            best_str = ", ".join(f"{b:g}" for b in best_times)
            
            print(f"run {run_idx}: {times_str} -> best so far: {best_str}; last improved in run {last_improved}")
            
            if run_idx - last_improved >= 5:
                print(f"(we can stop, there was an improvement of {percent_improvement:.3f}% in the last 5 steps, which is less than 0.1%)")
                break
            else:
                print(f"(we cannot stop, there was an improvement of {percent_improvement:.3f}% in the last 5 steps)")
            
        run_idx += 1
        
    best_sum = sum(best_times)
    best_str = ", ".join(f"{b:g}" for b in best_times)
    print(f"\nBenchmark result: sum({best_str}) = {best_sum:.3f}")

if __name__ == "__main__":
    main()
