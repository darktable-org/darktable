# Diffuse Module Optimization Instructions

## ⚠ CRITICAL RULES — VIOLATION WILL BE PENALISED ⚠

1. **DO NOT LOOK AT THE TEST DATA.** Never read, decode, inspect, or analyse the benchmark XMP files, test images, or their parameters. You must optimise the code WITHOUT any knowledge of what parameter values the benchmarks use. Optimisations must be universally correct and beneficial, not tailored to specific inputs.

2. **DO NOT GAME THE BENCHMARK.** Do not add special-case fast paths, early exits, or conditional skips that exploit knowledge of the test data's parameter values (e.g. "if all isotrope, skip X"). Optimisations must improve performance for ALL valid parameter combinations, not just the ones in the benchmark.

   **EXCEPTION:** You MAY optimize for typical usage patterns found in the module's built-in presets (e.g., specific combinations of zeros or signs in the anisotropy parameters) ONLY IF you can PROVE that these optimizations do not degrade performance for the "general case" (arbitrary parameter settings).

3. **DO NOT CHEAT.** Do not modify XMP files, test images, benchmark scripts (except path fixes), quality thresholds, or any test infrastructure to improve measured results. All optimisations must be in the C source code of `diffuse.c` only and must preserve mathematical correctness.

   If you choose to optimize for built-in presets:
   - You MUST create several "general case" performance tests using arbitrary settings that are NOT presets.
   - You MUST document these tests in `diffuse-general-case-performance-tests.md`.
   - You MUST first run these tests on the code from the `03809ba7d5d` commit to establish a new baseline.
   - You MUST add a new line in `diffuse-opt-progress.md` documenting this NEW baseline from `03809ba7d5d` and explaining why the change happened.
   - You MUST prove, by running these tests with a module iteration count of 20 (to ensure the workload is large enough to filter out timing noise), that your optimizations do not hurt the general case's performance.
   - **Performance Trade-off Rule 1**: A reduction in general-case performance of at most **0.5%** is acceptable ONLY IF you can improve preset-based performance by at least **2%**.
   - **Performance Trade-off Rule 2**: A reduction in performance for a specific preset of at most **0.5%** is acceptable ONLY IF you can improve performance of another preset by at least **2%**.
   - You MUST add performance tests for the built-in presets with a module iteration count of 20 for these measurements.
   - Note: "iterations" refers to the `.iterations` parameter within the diffuse module's configuration, not the number of times the benchmark script is executed.
   - Only after establishing the baseline and proving no degradation may you proceed with preset-specific optimizations.

## Objective

Optimize the CPU processing path of darktable's `diffuse` module to reduce execution time while preserving mathematical correctness and visual quality.
You MUST leave the OpenCL path alone. In fact, always run darktable and darktable-cli with `--disable-opencl`. You are optimising the CPU path.

## Scope of Code Changes

You may modify `diffuse.c` **and** functions in other files, provided those functions are **exclusively used by `diffuse.c`**. You MUST NOT introduce regressions into other modules. Before modifying a function outside `diffuse.c`, verify that it has no callers outside of `diffuse.c` (use grep/clangd to confirm). If a function is shared with other modules, leave it as-is and find another approach.

## Measurement Protocol

1. **Benchmark Script**: `python3 benchmark_diffuse.py`
   - Uses a rolling 5-window algorithm to track the sum of the minimum runtimes across 3 varying `diffuse` block instances.
   - Stops execution when the percentage improvement in the last 5 iterations is less than `0.1%`.

2. **Integration Testing (Quality Gate)**:
   - Ensure visual quality remains within tight thresholds using the darktable integration test suite.
   - Run from `src/tests/integration/`:
     ```
     export DARKTABLE_CLI="~/darktable-build/bin/darktable-cli --disable-opencl"
     ./run.sh --disable-opencl 0086-diffuse 0087-diffuse-isotrope 0088-diffuse-gradient 0089-diffuse-mixed 0090-diffuse-sharpen
     ```
   - The deltae script in the submodule may not work with the local Python. Use the modified copy at `diffuse-perf-test-files/deltae` (uses `/tmp/testenv` venv). Before running tests, ensure it's in place:
     ```
     cp ../../diffuse-perf-test-files/deltae ./deltae.local
     ```
     Or symlink `../deltae` → the local copy. If you need to modify any other test harness scripts, put modified copies in `diffuse-perf-test-files/` rather than editing the submodule.
   - Constraints: `Max dE <= 2.3, Avg dE <= 0.77` (deltae exit code 2 = FAIL). All 5 tests must pass.

## Build

Build with `Release` type: `./build.sh --prefix ~/darktable-build --build-type Release --install`

## Progress Tracking

All progress is tracked in `diffuse-opt-progress.md`. This file MUST contain exactly three top-level sections (H1 headers), in this order:

```
# DONE
# IN PROGRESS
# UPCOMING
```

If these sections do not exist, create them before proceeding.

**`# DONE`** — Completed experiments. Each entry records the idea, the outcome (success or failure), the failure stage if applicable (quality gate or benchmark), and a brief summary of what was learned.

**`# IN PROGRESS`** — At most one active experiment at a time. Contains the idea description, the rationale for selecting it, and any notes on the current state (e.g. "code implemented, quality gate passed, benchmark pending" or "quality gate attempt 2/3 failed, investigating").

**`# UPCOMING`** — A pipeline of candidate ideas to try next. Each entry is a brief description of the optimisation idea. Ideas already present anywhere in the file (DONE, IN PROGRESS, or UPCOMING) MUST NOT be duplicated.

## Workflow

Follow this state machine on every session. Execute each step in order; do not skip steps.

### Step 1: Check IN PROGRESS

If the `# IN PROGRESS` section contains an active experiment:

1. **Compare the code to the described idea.** Read `diffuse.c` and determine whether the code already reflects the planned change (e.g. the previous session implemented the change but was interrupted before testing).
   - If the code **matches** the idea: do NOT modify the code. Proceed directly to Step 2 (testing).
   - If the code **does not match** the idea: implement the change, then proceed to Step 2.

If the `# IN PROGRESS` section is empty, skip to Step 4.

### Step 2: Test

After the code change is in place (whether freshly implemented or carried over):

1. **Build**: `./build.sh --prefix ~/darktable-build --build-type Release --install`
2. **Quality gate first**: Run the integration tests. All 5 tests must pass.
   - If the quality gate **fails**: record the failure in the `# IN PROGRESS` section (e.g. "Quality gate attempt 1/3: FAILED — test 0088 exceeded Max dE"). Investigate and attempt to fix. You have a maximum of **3 quality gate attempts** for the current idea. If all 3 fail, proceed to Step 3 as a failure — but leave the idea in `# IN PROGRESS` (do not move to DONE). The other agent or the user may attempt to fix it. Then go to Step 5 (termination check).
   - If the quality gate **passes**: proceed to the performance benchmark.
3. **Performance benchmark**: Run `python3 benchmark_diffuse.py`.
   - The active baseline is the last line of `diffuse-performance.log`.
   - **Fast Fail**: If the first iteration is >10% worse than the baseline, the script aborts immediately.

### Step 3: Record Outcome

After testing is complete:

**On success** (quality gate passed AND benchmark improved over baseline):
1. Update `# IN PROGRESS` notes with the measured result.
2. Move the experiment from `# IN PROGRESS` to `# DONE`, recording: the outcome as **success**, the measured sum, and a brief summary.
3. Append to `diffuse-performance.log` in this exact format:
   ```
   <commit hash>
   <Description of change>
   <measured sum>
   ```
4. Commit `diffuse.c`, `diffuse-opt-progress.md`, and `diffuse-performance.log` together in a **single atomic commit** with a descriptive message. The new sum becomes the baseline.

**On failure** (quality gate passed but benchmark did not improve, OR quality gate failed 3 times):
1. Move the experiment from `# IN PROGRESS` to `# DONE`, recording: the outcome as **failure**, the failure stage (quality gate or benchmark), and what was learned. **Exception**: if the quality gate failed 3 times, leave the idea in `# IN PROGRESS` for the other agent to attempt. Note "3/3 quality gate failures, leaving for next agent" in the entry.
2. **Roll back `diffuse.c`** to the previous committed version on the current branch: `git checkout HEAD -- src/iop/diffuse.c` (adjust path as needed). Do NOT roll back the state tracking files.
3. Commit `diffuse-opt-progress.md` (and `diffuse-performance.log` if it was updated) with a message noting the failed experiment.

### Step 4: Replenish the Idea Pipeline

Check the `# UPCOMING` section. If it contains **fewer than 5 ideas**:
1. Generate new optimisation ideas for `diffuse.c`.
2. Before adding each idea, verify it does not already appear anywhere in `diffuse-opt-progress.md` (in DONE, IN PROGRESS, or UPCOMING).
3. Add ideas until UPCOMING has at least 5 entries.

Once there are at least 5 ideas in UPCOMING:
1. Select the idea you judge most likely to yield a measurable performance improvement.
2. Briefly document your reasoning for the selection (1–2 sentences explaining why this idea, why now).
3. Move it from `# UPCOMING` to `# IN PROGRESS`.
4. Go to Step 1 (which will find the new IN PROGRESS item and begin implementation).

### Step 5: Termination

**You MUST stop the session if any of these conditions is met:**

1. **3 consecutive failed experiments in THIS session by THIS agent.** Count experiments that moved to DONE with a failure outcome, consecutively, without an intervening success. Quality gate exhaustion (3/3 failures left in IN PROGRESS) also counts as a failed experiment for this counter. When you stop, note in `diffuse-opt-progress.md`: "Session terminated: 3 consecutive failures. Handing off to next agent."

2. **Quality gate exhaustion for the current idea.** If you have failed the quality gate 3 times for the same idea, you MUST stop immediately (do not pick the next idea). Leave the idea in `# IN PROGRESS`. Note: "Quality gate failed 3/3 times. Leaving in IN PROGRESS for next agent or manual intervention."

When stopped, ensure all state tracking files are committed before exiting.

## Summary of Files

| File | Purpose | Who updates it |
|---|---|---|
| `diffuse.c` | The optimisation target. All code changes go here. | Agent |
| `diffuse-opt-progress.md` | Tracks DONE / IN PROGRESS / UPCOMING experiments. | Agent |
| `diffuse-performance.log` | Append-only log of successful improvements (commit, description, measured sum). | Agent (on success only) |
| `diffuse-general-case-performance-tests.md` | Documents general-case tests if preset-specific optimisations are attempted. | Agent (when applicable) |
| `benchmark_diffuse.py` | Benchmark script. Do not modify (except path fixes). | — |
