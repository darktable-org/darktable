# Running darktable under runtime sanitizers

## Quick start

```bash
# build (a dedicated build dir, RelWithDebInfo, OpenCL off)
./build.sh --build-dir "$PWD/build-sanitize" \
           --sanitize address,undefined \
           --build-type RelWithDebInfo \
           --build-generator Ninja \
           --disable-opencl

# confirm the binary really is instrumented
ldd build-sanitize/bin/darktable-cli | grep -E 'libasan|libubsan'

# run the integration test suite against it
./tools/run-integration-tests.sh --build-dir "$PWD/build-sanitize"
```

The run ends with a deduplicated summary of every sanitizer finding and writes
the full reports plus a `summary.txt` under
`build-sanitize/sanitizer-logs/<timestamp>/`.

Reports are filed under the test that produced them, and tagged with the pass
they came from, so a whole-suite run stays traceable:

```
sanitizer-logs/20260904-103244/
├── 0000-nop/
│   ├── stderr-cpu.147106      captured stderr (GCC's UBSan reports here)
│   └── ubsan-cpu.147121       whatever the runtime's log_path caught
├── 0002-local-contrast/
│   └── ...
├── darktable-cli              the generated shim, for reproducing by hand
├── suite-output.txt
└── summary.txt
```

Tests that report nothing leave no directory behind, so what remains is exactly
the set that found something. The summary names the tests each finding turned up
in, which is most of the triage: one test behaving differently from the other
180 is a very different problem from a defect every test walks into.

## Available sanitizers

`--sanitize` takes a comma separated list. Slowdowns are relative to an
uninstrumented build of the same build type.

| Value | What it finds | Cost | Notes |
|---|---|---|---|
| `address` | heap/stack/global overflows, use-after-free, double free | ~2x CPU, ~3x RSS | includes LeakSanitizer, but leak reporting is off by default (see below) |
| `undefined` | signed overflow, bad shifts, misaligned access, invalid casts, null deref | ~1.2x CPU | cheapest useful signal, good default for CI |
| `leak` | memory leaks at exit | ~native | standalone LSan, without ASan's slowdown |
| `thread` | data races, lock-order inversions | ~5-15x CPU, ~5-10x RSS | see the OpenMP caveat below |

Mutually exclusive: `thread` with either `address` or `leak`. `address` already
includes `leak`, so asking for both is rejected.

MemorySanitizer is deliberately **not** offered. It only produces usable results
when every dependency (glib, GTK, lcms2, exiv2, libjpeg, ...) is MSan
instrumented as well; against stock system libraries it reports nothing but
false positives.

## Recommended invocations

Baseline for the full 177-test suite is roughly 32-40 minutes with both the CPU
and the OpenCL pass. `run-integration-tests.sh` skips the OpenCL pass by
default, which roughly halves the work before the sanitizer overhead applies.

```bash
# the default: memory errors + undefined behaviour, ~45-60 min for the suite
./build.sh --build-dir "$PWD/build-asan" --sanitize address,undefined \
           --build-type RelWithDebInfo --build-generator Ninja --disable-opencl
./tools/run-integration-tests.sh --build-dir "$PWD/build-asan"

# cheap enough for every commit, ~20-25 min
./build.sh --build-dir "$PWD/build-ubsan" --sanitize undefined ...

# leak hunting, roughly baseline speed
./build.sh --build-dir "$PWD/build-lsan" --sanitize leak ...

# races: far too slow for the whole suite, run named tests
./build.sh --build-dir "$PWD/build-tsan" --sanitize thread ...
./tools/run-integration-tests.sh --build-dir "$PWD/build-tsan" 0035-multiple-modules
```

## Things worth knowing

**Use `RelWithDebInfo`.** `Release` adds `-O3 -ffast-math -fno-finite-math-only`,
which lets the compiler assume no NaN/Inf and makes UBSan's floating point
checks unreliable. `Debug` defines `_DEBUG`, under which `dt_alloc_aligned()`
over-allocates by a cacheline and hands out an interior pointer, so ASan's
redzones no longer sit next to the user buffer and small over/underflows go
undetected. The build warns about both.

**Judge the run by the report count, not by OK/FAILS.** The suite's
`expected.png` references came from a `-O3 -ffast-math` Release build.
RelWithDebInfo is `-O2` without fast-math, so some tests drift against the
Delta-E threshold for reasons that have nothing to do with sanitizers. Also,
when a sanitizer does end a test the suite just prints `FAILS: darktable-cli
errored` without any detail; the detail is in the log directory.

**Use `run-integration-tests.sh`, not `src/tests/integration/run` directly.**
The suite invokes darktable-cli as `$* 1> /dev/null 2> /dev/null`, and
sanitizers report on stderr, so driving it by hand throws every finding away.
Two mechanisms rescue them, and both are needed: the `log_path=` settings in
the generated `sanitizer-env.sh` catch everything that reports through
sanitizer_common (ASan, LSan, TSan), and the driver's darktable-cli shim
captures stderr per invocation, because GCC's UBSan prints its non-fatal
`runtime error:` diagnostics directly to stderr no matter what `log_path` says.

**A sanitizer that refuses to start is almost always ASLR.** If the runtime
dies before `main()` with

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x793e64c72000-0x793e65100000
```

(ASan words it as `Shadow memory range interleaves with an existing memory
mapping`), the kernel is handing out mappings with more entropy than the
runtime's fixed shadow layout can accommodate. Kernels default
`vm.mmap_rnd_bits` to 32; GCC's runtimes support at most 28. Either lower it

```bash
sudo sysctl -w vm.mmap_rnd_bits=28    # persist via /etc/sysctl.d/
```

or run with randomisation off, which needs no root:

```bash
setarch -R build-tsan/bin/darktable-cli --version
```

`run-integration-tests.sh` preflights the binary once and falls back to
`setarch -R` by itself when it sees this, reporting the choice as `ASLR: off
(auto: ...)` in its banner. Force it either way with `--no-aslr` / `--keep-aslr`.
Without the preflight the symptom is misleading: every test fails in a fraction
of a second with `FAILS: darktable-cli errored`, which looks like a darktable
bug rather than an environment one.

**Leak reporting is off under `address`.** darktable-cli exits without tearing
down its GTK/glib/lua state, so exit-time leak reports would fire on every
single test. Build with `--sanitize leak` when you actually want to hunt leaks.

**OpenCL is off by default.** `--with-opencl` makes the suite render every test
a second time on the GPU, into `output-cl.png` beside the CPU's `output.png`,
and then diff the two against the per-test `cpugpu.maxpix` threshold where one
exists. So it doubles the number of instrumented darktable-cli invocations, on
top of whatever the sanitizer already costs. It also couples the two passes:
`run` sums both exit codes, so a failure in either one marks the whole test as
an error and skips the Delta-E comparison for both.

The more important cost is signal quality. The ICD loader and the vendor driver
behind it are closed, uninstrumented binaries, but ASan still intercepts their
allocations, so their internal caches, worker pools and JIT buffers surface as
findings in code you neither own nor can fix.

Which sanitizer actually pays for that depends on the build:

* `address` builds run with `detect_leaks=0` (see above), so leaks are never
  reported and `lsan.supp` is not consulted at all. What can still reach you is
  driver-internal noise reported through ASan's interceptors.
* `leak` builds are where `lsan.supp` earns its keep, and where the GPU pass
  will turn up allocations you have to add to it.

To extend it, run the suite, find the `Direct leak` / `Indirect leak` entries in
the log directory, pick a frame that identifies the owning library, and add one
line per owner:

```
leak:libMyVendorOpenCL
```

The syntax is `leak:<substring>`, matched against function names and against
source and library paths anywhere in the leak's stack. Prefer the library soname
over a specific symbol: vendor symbols get renamed between driver releases,
paths generally do not. Keep each entry narrow enough to stay honest --
`leak:libOpenCL.so` is a fair suppression, `leak:malloc` would bury darktable's
own leaks along with the driver's.

The usual failure mode is a suppression that silently never matches, because a
typo just means the substring is not found. LSan can tell you which entries
fired, but the generated `sanitizer-env.sh` sets `print_suppressions=0`; change
it in `sanitizer-env.sh.in` and re-run CMake to regenerate, then check that each
new entry reports a non-zero count.

The entries already in `lsan.supp` cover the ICD loader plus the AMD, Intel and
NVIDIA runtimes. A Mesa (rusticl) or POCL stack allocates through different
libraries and will need its own.

Under `thread` the GPU pass is not worth attempting. The driver's own worker
threads carry no TSan annotations, so their synchronisation is invisible and
nearly every OpenCL call turns into a reported race.

**ASan does not stop at the first finding.** The build passes
`-fsanitize-recover=address` and the environment sets `halt_on_error=0`, so a
recoverable error is reported and execution continues. Both halves are required:
without the compile flag every ASan finding is a hard abort no matter what
`halt_on_error` says. This matters because a single over-read during start-up
would otherwise end every test in the suite at the same place and hide
everything behind it. Errors ASan cannot recover from -- SIGSEGV, allocator
failures -- still end the process. A defect inside a loop reports once per
iteration, which is what the aggregator's deduplication is for.

**TSan exits 0 on findings, on purpose.** Sanitizers default to `exitcode=66`
once they have reported anything, and the suite treats any non-zero exit as
`FAILS: darktable-cli errored` and never compares the images. A race does not
necessarily corrupt the export -- `output.png` is written normally -- so leaving
the default would discard the suite's own signal on every TSan test. The
generated `sanitizer-env.sh` therefore sets `exitcode=0` for TSan only. Nothing
is hidden: the reports still land in the log directory, and
`run-integration-tests.sh` still exits non-zero whenever the aggregator finds
anything. A genuine crash still exits non-zero by itself. ASan keeps the default,
because there `halt_on_error=1` means the run really was cut short.

**TSan against a GCC build needs the suppressions here.** GCC's libgomp is not
built with TSan annotations, so TSan cannot see the happens-before edges that
OpenMP barriers establish and reports a race on essentially every parallel loop.
`tsan.supp` filters those. Clang with an annotated libomp (or archer) does not
need them.

**Clang builds need libomp.** CMake's `FindOpenMP` tries `-fopenmp=libomp`
first, so a clang build needs the matching `libomp-<version>-dev` package
installed. GCC works out of the box.

**`-Werror` is dropped for sanitizer builds.** Instrumentation perturbs the
optimizer and reliably produces fresh `-Wmaybe-uninitialized` / `-Wstringop-*`
diagnostics; with `-Werror -Wfatal-errors` those would abort the build before a
single sanitizer had a chance to run. The warnings themselves stay on.

## Files

| Path | Purpose |
|---|---|
| `cmake/sanitizers.cmake` | flag assembly, validation, link check, env file generation |
| `DefineOptions.cmake` | the `DT_SANITIZE` / `DT_SANITIZE_EXTRA_CHECKS` cache entries |
| `tools/sanitizer/sanitizer-env.sh.in` | template for the generated `<build>/bin/sanitizer-env.sh` |
| `tools/sanitizer/{ubsan,lsan,tsan}.supp` | suppression files |
| `tools/run-integration-tests.sh` | suite driver, timeout and stderr-capture shim |
| `tools/sanitizer/aggregate-reports.py` | deduplicates the reports; also usable on its own against an old log dir. Exits 0 clean, 1 with findings, 2 when a runtime failed to start |

`DT_SANITIZE_EXTRA_CHECKS=ON` adds clang's `-fsanitize=integer,implicit-conversion`.
It is off by default because darktable's pixel loops rely on wrapping arithmetic
and implicit narrowing, so it produces thousands of non-bugs.
