#!/bin/bash
#
# Run the darktable integration test suite against a sanitizer build and
# collect the sanitizer findings.
#
# The suite itself lives in src/tests/integration, which is a separate upstream
# repository (darktable-org/darktable-tests). This driver therefore does its
# work from the outside, without patching anything in there:
#
#   * it points DARKTABLE_CLI at the sanitizer build (the suite's 'run' honours
#     that environment variable),
#   * it sources the generated sanitizer-env.sh, whose log_path settings are
#     what rescue the reports from the "2> /dev/null" in the suite's call(),
#   * it interposes a shim so that a hung darktable-cli hits a timeout -- the
#     suite has no timeout of its own anywhere,
#   * it preflights the binary, so that a sanitizer runtime which cannot even
#     start is reported as such instead of as every test failing,
#   * and it aggregates and deduplicates the reports afterwards.
#
# Usage:
#   ./tools/run-integration-tests.sh [options] [test names passed to ./run]
#
# Options:
#   --build-dir <dir>   sanitizer build tree      (default: build-sanitize)
#   --log-dir <dir>     where reports land        (default: <build>/sanitizer-logs/<stamp>)
#   --timeout <secs>    per darktable-cli call    (default: 1800)
#   --with-opencl       also run the GPU pass     (default: CPU pass only)
#   --no-openmp         serialise OpenMP (rewrites the suite's -t to 1)
#   --no-aslr           force ASLR off for the CLI (setarch -R)
#   --keep-aslr         never disable ASLR, even if the runtime needs it
#   -h, --help          this message
#
# --no-openmp is mostly for ThreadSanitizer. GCC's libgomp carries no TSan
# annotations, so TSan cannot see the happens-before edges OpenMP barriers
# establish and reports a race on nearly every parallel loop -- in one full-suite
# run, 97% of all reports. Serialising the OpenMP loops removes that at the
# source instead of trying to suppress it afterwards, leaving darktable's own
# threading (pixelpipe, caches, lua) visible.
#
# ASLR handling defaults to auto: the preflight only falls back to setarch -R
# when the sanitizer runtime turns out to need it.
#
# Everything not recognised is passed straight through to the suite's ./run,
# so a single test can be selected by name:
#
#   ./tools/run-integration-tests.sh --build-dir build-asan 0001-exposure

set -u

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/.." && pwd -P)

BUILD_DIR="$DT_SRC_DIR/build-sanitize"
LOG_DIR=""
CLI_TIMEOUT=1800
WITH_OPENCL=0
NO_OPENMP=0
ASLR_MODE=auto
RUN_ARGS=()

while [ "$#" -ge 1 ]; do
    case "$1" in
        --build-dir)   BUILD_DIR="$2"; shift ;;
        --build-dir=*) BUILD_DIR="${1#--build-dir=}" ;;
        --log-dir)     LOG_DIR="$2"; shift ;;
        --log-dir=*)   LOG_DIR="${1#--log-dir=}" ;;
        --timeout)     CLI_TIMEOUT="$2"; shift ;;
        --timeout=*)   CLI_TIMEOUT="${1#--timeout=}" ;;
        --with-opencl) WITH_OPENCL=1 ;;
        --no-openmp)   NO_OPENMP=1 ;;
        --no-aslr)     ASLR_MODE=off ;;
        --keep-aslr)   ASLR_MODE=keep ;;
        # Print the header block, however long it happens to be.
        -h|--help)     awk 'NR>2 && /^#/ { sub(/^# ?/, ""); print; next }
                            NR>2         { exit }' "$0"; exit 0 ;;
        *)             RUN_ARGS+=("$1") ;;
    esac
    shift
done

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$(cd "$(dirname "$BUILD_DIR")" 2>/dev/null && pwd -P)/$(basename "$BUILD_DIR")" ;;
esac

SUITE_DIR="$DT_SRC_DIR/src/tests/integration"
REAL_CLI="$BUILD_DIR/bin/darktable-cli"
SAN_ENV="$BUILD_DIR/bin/sanitizer-env.sh"

die() { echo "error: $*" >&2; exit 1; }

[ -x "$REAL_CLI" ] || die "no darktable-cli in $BUILD_DIR/bin -- build it first, e.g.
  ./build.sh --build-dir $BUILD_DIR --sanitize address,undefined --build-type RelWithDebInfo"
[ -d "$SUITE_DIR" ] && [ -x "$SUITE_DIR/run" ] || die "integration test suite not checked out. Run:
  git submodule update --init src/tests/integration"

if [ ! -f "$SAN_ENV" ]; then
    die "$SAN_ENV is missing -- $BUILD_DIR was not configured with -DDT_SANITIZE"
fi

if [ -z "$LOG_DIR" ]; then
    LOG_DIR="$BUILD_DIR/sanitizer-logs/$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$LOG_DIR"

DT_SAN_LOGDIR="$LOG_DIR"
export DT_SAN_LOGDIR
# shellcheck source=/dev/null
. "$SAN_ENV"

# The suite hardcodes --configdir /tmp/darktable-test and shares it across runs.
# A library.db left behind by a non-sanitized run can change what gets processed,
# so start from a clean one.
rm -rf /tmp/darktable-test

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
#
# A sanitizer runtime that cannot lay out its shadow memory dies inside its own
# initialiser, before main(). Every test then fails in a fraction of a second
# with "darktable-cli errored", which reads like a darktable bug and is not one.
# Catch that here, up front, with a message that says what actually happened.
#
# The usual cause is ASLR entropy: kernels default vm.mmap_rnd_bits to 32 while
# GCC's runtimes support at most 28. Running the child with randomisation off
# works around it without root, and costs a test run nothing.

# The preflight output has to come back on stderr, so drop the log_path that
# would otherwise divert it into a file and pollute the report directory.
strip_log_path() {
    printf '%s' "$1" | sed -e 's/log_path=[^:]*://' -e 's/:log_path=[^:]*//'
}

run_preflight() {
    # shellcheck disable=SC2086  # $1 is a command prefix and must word-split
    ASAN_OPTIONS=$(strip_log_path "${ASAN_OPTIONS:-}") \
    TSAN_OPTIONS=$(strip_log_path "${TSAN_OPTIONS:-}") \
    UBSAN_OPTIONS=$(strip_log_path "${UBSAN_OPTIONS:-}") \
    LSAN_OPTIONS=$(strip_log_path "${LSAN_OPTIONS:-}") \
        $1 "$REAL_CLI" --version 2>&1
}

startup_failed() {
    case "$1" in
        *"unexpected memory mapping"*)      return 0 ;;
        *"Shadow memory range interleaves"*) return 0 ;;
        *"FATAL: "*"Sanitizer"*)            return 0 ;;
    esac
    return 1
}

# Whether the runtime survives is probabilistic: it depends on where the kernel
# happened to place a mapping, so a single probe proves nothing. Measured on
# this 7.x kernel with the default vm.mmap_rnd_bits=32, TSan started in 1 run
# out of 40 -- an unlucky single preflight would wave through a configuration in
# which almost every test then dies.
PREFLIGHT_PROBES=5

# Succeeds only when every probe started cleanly. The last failing output is
# left in PREFLIGHT for the diagnostic below.
probe_startup() {
    local prefix="$1" i out
    PREFLIGHT=""
    for i in $(seq "$PREFLIGHT_PROBES"); do
        out=$(run_preflight "$prefix")
        if startup_failed "$out"; then
            PREFLIGHT="$out"
            return 1
        fi
    done
    return 0
}

SETARCH_PREFIX=""
ASLR_NOTE="on"
HAVE_SETARCH=0
command -v setarch >/dev/null 2>&1 && HAVE_SETARCH=1

if [ "$ASLR_MODE" = off ]; then
    [ "$HAVE_SETARCH" -eq 1 ] || die "--no-aslr needs setarch (util-linux)"
    SETARCH_PREFIX="setarch -R"
    ASLR_NOTE="off (forced)"
fi

if ! probe_startup "$SETARCH_PREFIX"; then
    if [ "$ASLR_MODE" = auto ] && [ "$HAVE_SETARCH" -eq 1 ] \
       && probe_startup "setarch -R"; then
        SETARCH_PREFIX="setarch -R"
        ASLR_NOTE="off (auto: the runtime needs it)"
    else
        case "$ASLR_MODE" in
            keep) ADVICE="
--keep-aslr suppressed the 'setarch -R' fallback, which works around this
without root." ;;
            off)  ADVICE="
'setarch -R' was already in use and did not help, so ASLR entropy is not the
only thing wrong here." ;;
            *)    if [ "$HAVE_SETARCH" -eq 1 ]; then
                      ADVICE="
'setarch -R' did not help either."
                  else
                      ADVICE="
Installing util-linux would let this script fall back to 'setarch -R', which
works around this without root."
                  fi ;;
        esac

        die "the sanitizer runtime does not start reliably ($PREFLIGHT_PROBES probes):

  $PREFLIGHT

This is an environment problem rather than a darktable one: the runtime could
not lay out its shadow memory, so the tests would have failed without ever
running. It is probabilistic -- an occasional run that does start does not
contradict it.

Lower the kernel's ASLR entropy to something the runtime supports:

  sudo sysctl -w vm.mmap_rnd_bits=28

and make it permanent with a line in /etc/sysctl.d/.
$ADVICE"
    fi
fi

# darktable-cli shim. It does three jobs:
#
#  * timeout, because the suite has no timeout anywhere and under a sanitizer a
#    hang would block the whole run indefinitely;
#  * capture stderr, because log_path only rescues the reports that go through
#    sanitizer_common. GCC's UBSan prints its non-fatal "runtime error:"
#    diagnostics straight to stderr no matter what log_path says, and the suite
#    runs the CLI as "$* 1> /dev/null 2> /dev/null", so without this they are
#    lost. The two channels are complementary, not duplicated: with log_path
#    set, ASan writes only to its log file;
#  * file both channels under the test they belong to. A whole-suite run is
#    ~180 tests, and a flat directory of stderr.<pid> and ubsan.<pid> files
#    cannot be traced back to the test that produced them.
#
# The shim is generated in two halves: an unquoted here-document for the values
# this script has to bake in, then a quoted one for the body, so that the body
# needs no escaping and reads like the shell script it is.
SHIM="$LOG_DIR/darktable-cli"
{
cat <<EOF
#!/bin/sh
# Generated by tools/run-integration-tests.sh. Do not edit; it is rewritten on
# every run.
DT_SAN_RUN_LOGDIR="$LOG_DIR"
DT_SAN_REAL_CLI="$REAL_CLI"
DT_SAN_TIMEOUT="$CLI_TIMEOUT"
DT_SAN_SETARCH="$SETARCH_PREFIX"
DT_SAN_NO_OPENMP="$NO_OPENMP"
EOF
cat <<'EOF'

# The suite does "cd $dir" before invoking the CLI, so the working directory is
# the test directory, and that is the only handle on the test's identity we get
# from outside the submodule. Tests carrying their own test.sh driver may run
# from somewhere else; bucket those rather than guessing.
case "$PWD" in
    */[0-9][0-9][0-9][0-9]-*) test_name=$(basename "$PWD") ;;
    *)                        test_name=_other ;;
esac

log_dir="$DT_SAN_RUN_LOGDIR/$test_name"
mkdir -p "$log_dir"

# Serialising OpenMP has to be done on the command line, not through the
# environment: dt_init() derives its thread count from -t/--threads and then
# calls omp_set_num_threads() (src/common/darktable.c:1618), which overrides
# whatever OMP_NUM_THREADS said. The suite always passes -t, so rewrite it.
if [ "$DT_SAN_NO_OPENMP" = 1 ]; then
    remaining=$#
    replace_next=0
    while [ "$remaining" -gt 0 ]; do
        arg=$1
        shift
        if [ "$replace_next" = 1 ]; then
            set -- "$@" 1
            replace_next=0
        else
            case "$arg" in
                -t|--threads) replace_next=1 ;;
            esac
            set -- "$@" "$arg"
        fi
        remaining=$((remaining - 1))
    done
fi

# With --with-opencl the suite renders each test twice from the same directory.
# The output file it asks for is what tells the two invocations apart.
pass=cpu
for arg in "$@"; do
    case "$arg" in *output-cl.png) pass=gpu ;; esac
done

# Point each runtime's log_path into this test's directory. sanitizer-env.sh
# exported a run-wide prefix; only that prefix changes here.
retarget()
{
    printf '%s' "$1" | sed -e "s|log_path=[^:]*|log_path=$log_dir/$2-$pass|"
}

[ -n "${ASAN_OPTIONS:-}" ]  && ASAN_OPTIONS=$(retarget  "$ASAN_OPTIONS"  asan)
[ -n "${TSAN_OPTIONS:-}" ]  && TSAN_OPTIONS=$(retarget  "$TSAN_OPTIONS"  tsan)
[ -n "${UBSAN_OPTIONS:-}" ] && UBSAN_OPTIONS=$(retarget "$UBSAN_OPTIONS" ubsan)
[ -n "${LSAN_OPTIONS:-}" ]  && LSAN_OPTIONS=$(retarget  "$LSAN_OPTIONS"  lsan)
export ASAN_OPTIONS TSAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS

exec timeout --signal=TERM --kill-after=30 "$DT_SAN_TIMEOUT" \
     $DT_SAN_SETARCH "$DT_SAN_REAL_CLI" "$@" 2>>"$log_dir/stderr-$pass.$$"
EOF
} > "$SHIM"
chmod +x "$SHIM"

DARKTABLE_CLI="$SHIM"
export DARKTABLE_CLI

# Belt and braces: darktable itself ignores this (see the shim), but other
# OpenMP users in the process, rawspeed included, do honour it.
if [ "$NO_OPENMP" -eq 1 ]; then
    OMP_NUM_THREADS=1
    export OMP_NUM_THREADS
fi

OPENCL_ARG="--disable-opencl"
[ "$WITH_OPENCL" -eq 1 ] && OPENCL_ARG=""

cat <<EOF
darktable integration tests under sanitizers

Build dir:    $BUILD_DIR
Sanitizers:   ${DT_SANITIZERS:-unknown}
Log dir:      $LOG_DIR
CLI timeout:  ${CLI_TIMEOUT}s per invocation
ASLR:         $ASLR_NOTE
OpenMP:       $([ "$NO_OPENMP" -eq 1 ] && echo "serialised (-t 1)" || echo "on")
OpenCL pass:  $([ "$WITH_OPENCL" -eq 1 ] && echo yes || echo "no (--with-opencl to enable)")
Tests:        ${RUN_ARGS[*]:-all}

EOF

SUITE_OUT="$LOG_DIR/suite-output.txt"

START=$(date +%s)
(
    cd "$SUITE_DIR" || exit 1
    # shellcheck disable=SC2086
    ./run $OPENCL_ARG "${RUN_ARGS[@]}"
) 2>&1 | tee "$SUITE_OUT"
ELAPSED=$(( $(date +%s) - START ))

# The suite's ./run always exits 0, so read its own tally instead. Note that a
# failing test here is not necessarily a sanitizer finding: the expected.png
# references were produced by a -O3 -ffast-math Release build, so a
# RelWithDebInfo build drifts on some tests for reasons unrelated to
# instrumentation.
SUITE_TOTAL=$(sed -n 's/^Total test  *\([0-9][0-9]*\)$/\1/p' "$SUITE_OUT" | tail -1)
SUITE_ERRORS=$(sed -n 's/^Errors  *\([0-9][0-9]*\)$/\1/p' "$SUITE_OUT" | tail -1)
SUITE_TOTAL=${SUITE_TOTAL:-0}
SUITE_ERRORS=${SUITE_ERRORS:-0}

printf '\nSuite finished in %dm%02ds: %s of %s tests reported a failure\n\n' \
    $((ELAPSED / 60)) $((ELAPSED % 60)) "$SUITE_ERRORS" "$SUITE_TOTAL"

# ---------------------------------------------------------------------------
# Aggregate the sanitizer reports
# ---------------------------------------------------------------------------

# One stderr capture per CLI invocation; most of them are empty. Sweep the
# per-test directories too, then drop any directory left with nothing in it, so
# that what remains is exactly the set of tests that reported something.
find "$LOG_DIR" -name 'stderr-*' -size 0 -delete 2>/dev/null
find "$LOG_DIR" -mindepth 1 -type d -empty -delete 2>/dev/null

SUMMARY="$LOG_DIR/summary.txt"

"$DT_SRC_DIR/tools/sanitizer/aggregate-reports.py" "$LOG_DIR" \
    --build-dir "$BUILD_DIR" --output "$SUMMARY"
FINDINGS_RC=$?

# 2 means a runtime died during start-up: the suite ran, but part of it was
# never instrumented, so the findings below understate the truth.
if [ "$FINDINGS_RC" -eq 2 ]; then
    echo
    echo "warning: a sanitizer failed to start during this run, so the findings"
    echo "         above cover less than the whole suite."
fi

echo "full reports: $LOG_DIR"
echo "summary:      $SUMMARY"
echo "suite output: $SUITE_OUT"

# Non-zero if the sanitizers found anything, or if the suite itself failed.
if [ "$FINDINGS_RC" -ne 0 ] || [ "$SUITE_ERRORS" -ne 0 ]; then
    exit 1
fi
exit 0
