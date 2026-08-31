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
#   -h, --help          this message
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
        -h|--help)     sed -n '3,31p' "$0" | sed -e 's/^# \{0,1\}//'; exit 0 ;;
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

# darktable-cli shim. It does two jobs:
#
#  * timeout, because the suite has no timeout anywhere and under a sanitizer a
#    hang would block the whole run indefinitely;
#  * capture stderr, because log_path only rescues the reports that go through
#    sanitizer_common. GCC's UBSan prints its non-fatal "runtime error:"
#    diagnostics straight to stderr no matter what log_path says, and the suite
#    runs the CLI as "$* 1> /dev/null 2> /dev/null", so without this they are
#    lost. The two channels are complementary, not duplicated: with log_path
#    set, ASan writes only to its log file.
SHIM="$LOG_DIR/darktable-cli"
cat > "$SHIM" <<EOF
#!/bin/sh
exec timeout --signal=TERM --kill-after=30 "$CLI_TIMEOUT" "$REAL_CLI" "\$@" 2>>"$LOG_DIR/stderr.\$\$"
EOF
chmod +x "$SHIM"

DARKTABLE_CLI="$SHIM"
export DARKTABLE_CLI

OPENCL_ARG="--disable-opencl"
[ "$WITH_OPENCL" -eq 1 ] && OPENCL_ARG=""

cat <<EOF
darktable integration tests under sanitizers

Build dir:    $BUILD_DIR
Sanitizers:   ${DT_SANITIZERS:-unknown}
Log dir:      $LOG_DIR
CLI timeout:  ${CLI_TIMEOUT}s per invocation
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

# One stderr capture per CLI invocation; most of them are empty.
find "$LOG_DIR" -maxdepth 1 -name 'stderr.*' -size 0 -delete 2>/dev/null

SUMMARY="$LOG_DIR/summary.txt"

"$DT_SRC_DIR/tools/sanitizer/aggregate-reports.py" "$LOG_DIR" --output "$SUMMARY"
FINDINGS_RC=$?

echo "full reports: $LOG_DIR"
echo "summary:      $SUMMARY"
echo "suite output: $SUITE_OUT"

# Non-zero if the sanitizers found anything, or if the suite itself failed.
if [ "$FINDINGS_RC" -ne 0 ] || [ "$SUITE_ERRORS" -ne 0 ]; then
    exit 1
fi
exit 0
