# Diffuse Module Optimization Instructions

## Objective
Optimize the CPU processing path of darktable's `diffuse` module to reduce execution time while preserving mathematical correctness and visual quality.

## Measurement Protocol
1. **Benchmark Script**: `python3 benchmark_diffuse.py`
   - Uses a rolling 5-window algorithm to track the sum of the minimum runtimes across 3 varying `diffuse` block instances.
   - Stops execution when the percentage improvement in the last 5 iterations is less than `0.1%`.

2. **Integration Testing (Quality Gate)**:
   - Ensure visual quality remains within tight thresholds using the darktable integration test suite.
   - Run `export DARKTABLE_CLI="/home/test/darktable-build/bin/darktable-cli -d perf -d opencl"; ./run.sh 0086-diffuse`
   - Constraints: `Max dE <= 1.25, Avg dE <= 0.01, Std dE <= 0.025, Pixels above tolerance == 0%`.

## Iteration Rules 
- Build with `Release` type: `./build.sh --prefix /home/test/darktable-build --build-type Release --install`
- The active CPU baseline is fetched automatically from the last line of `diffuse-performance.log`. Ensure this log is always up-to-date.
- **Fast Fail**: If the first iteration of the benchmark is >10% worse than the active baseline, the script aborts immediately.
- After any successful improvement:
  1. Commit the change with the measured sum.
  2. Append to `diffuse-performance.log` following the format:
     ```
     <commit hash>
     <Description of change>
     <measured sum>
     ```
  3. The new sum becomes the baseline for subsequent iterations.
- Track all approaches (successes & failures) in `diffuse-opt-progress.md`.
- **Termination condition**: Stop after failing to make progress across 3 consecutive, unique approaches.
