#!/usr/bin/env bash
set -euo pipefail

script_path=$(readlink -f "$0")
script_dir=$(dirname "$script_path")
repo_root=$(dirname "$script_dir")

validation_timeout_seconds=${DARKTABLE_LIVE_BRIDGE_TIMEOUT_SECONDS:-15}
helper_timeout_seconds=${DARKTABLE_LIVE_BRIDGE_HELPER_TIMEOUT_SECONDS:-5}
ready_attempts=${DARKTABLE_LIVE_BRIDGE_READY_ATTEMPTS:-8}
post_set_attempts=${DARKTABLE_LIVE_BRIDGE_POST_SET_ATTEMPTS:-4}

if [[ ${1:-} != "--inner" ]]; then
  exec timeout --signal=KILL "${validation_timeout_seconds}s" \
    dbus-run-session -- "$script_path" --inner "$@"
fi
shift

source_asset_path=${DARKTABLE_LIVE_BRIDGE_ASSET:-/home/cgasgarth/Documents/projects/aiPhotoEditing/darktableAI/assets/_DSC8809.ARW}
requested_exposure=${DARKTABLE_LIVE_BRIDGE_EXPOSURE:-1.25}
darktable_bin=${DARKTABLE_LIVE_BRIDGE_DARKTABLE:-/usr/bin/darktable}
bridge_bin=${DARKTABLE_LIVE_BRIDGE_HELPER:-$repo_root/build/bin/darktable-live-bridge}
tmux_session=${DARKTABLE_LIVE_BRIDGE_TMUX_SESSION:-darktable-live-validate-$$}
tmux_socket=${DARKTABLE_LIVE_BRIDGE_TMUX_SOCKET:-darktable-live-validate-$$}

if [[ ! -x "$darktable_bin" ]]; then
  echo "missing darktable binary: $darktable_bin" >&2
  exit 1
fi

if [[ ! -x "$bridge_bin" ]]; then
  echo "missing darktable-live-bridge binary: $bridge_bin" >&2
  exit 1
fi

if [[ ! -f "$source_asset_path" ]]; then
  echo "missing asset: $source_asset_path" >&2
  exit 1
fi

run_root=$(mktemp -d)
config_dir="$run_root/config"
cache_dir="$run_root/cache"
tmp_dir="$run_root/tmp"
runtime_dir="$run_root/runtime"
asset_dir="$run_root/asset"
library_path="$run_root/library.db"
darktable_log="$run_root/darktable.log"
mkdir -p "$config_dir" "$cache_dir" "$tmp_dir" "$runtime_dir" "$asset_dir"
chmod 700 "$runtime_dir"

asset_path="$asset_dir/$(basename "$source_asset_path")"
cp -- "$source_asset_path" "$asset_path"

cleanup() {
  tmux -L "$tmux_socket" kill-session -t "$tmux_session" 2>/dev/null || true
  tmux -L "$tmux_socket" kill-server 2>/dev/null || true
  rm -rf "$run_root"
}
trap cleanup EXIT

capture_tmux_log() {
  tmux -L "$tmux_socket" capture-pane -p -S -200 -t "$tmux_session" 2>/dev/null || true
}

fail() {
  echo "$1" >&2
  if [[ -f "$darktable_log" ]]; then
    echo "--- darktable.log tail ---" >&2
    tail -n 80 "$darktable_log" >&2 || true
  fi
  local pane_log
  pane_log=$(capture_tmux_log)
  if [[ -n "$pane_log" ]]; then
    echo "--- tmux pane tail ---" >&2
    printf '%s\n' "$pane_log" >&2
  fi
  exit 1
}

start_darktable_host() {
  local command
  command=$(cat <<HOST
export NO_AT_BRIDGE=1
export GDK_BACKEND=x11
export GIO_USE_VFS=local
export GVFS_DISABLE_FUSE=1
export XDG_RUNTIME_DIR='$runtime_dir'
exec xvfb-run -a --server-args='-screen 0 1280x800x24' \
  '$darktable_bin' \
  --configdir '$config_dir' \
  --cachedir '$cache_dir' \
  --tmpdir '$tmp_dir' \
  --library '$library_path' \
  --disable-opencl \
  '$asset_path' >'$darktable_log' 2>&1
HOST
)
  tmux -L "$tmux_socket" new-session -d -s "$tmux_session" bash -lc "$command"
}

ensure_tmux_session_alive() {
  if ! tmux -L "$tmux_socket" has-session -t "$tmux_session" 2>/dev/null; then
    fail "darktable tmux session exited unexpectedly"
  fi
}

wait_for_remote_lua() {
  local attempt
  for attempt in $(seq 1 "$ready_attempts"); do
    ensure_tmux_session_alive
    if gdbus call \
      --session \
      --timeout 1 \
      --dest org.darktable.service \
      --object-path /darktable \
      --method org.darktable.service.Remote.Lua \
      "return 'ready'" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  fail "timed out waiting for Remote.Lua readiness"
}

run_bridge() {
  timeout --signal=KILL "${helper_timeout_seconds}s" "$bridge_bin" "$@"
}

wait_for_session_payload() {
  local attempts=$1
  local expected_mode=$2
  local attempt json
  for attempt in $(seq 1 "$attempts"); do
    ensure_tmux_session_alive
    if json=$(run_bridge get-session 2>/dev/null); then
      if python3 - "$json" "$asset_path" "$expected_mode" "$requested_exposure" <<'PY'
import json, math, os, sys
payload = json.loads(sys.argv[1])
asset = os.path.realpath(sys.argv[2])
mode = sys.argv[3]
requested = float(sys.argv[4])
active = payload.get('activeImage') or {}
source = active.get('sourceAssetPath')
if payload.get('status') != 'ok' or not source or os.path.realpath(source) != asset:
    raise SystemExit(1)
if mode == 'post-set':
    exposure = (payload.get('exposure') or {}).get('current')
    if not isinstance(exposure, (int, float)) or math.isnan(exposure) or abs(exposure - requested) > 1e-6:
        raise SystemExit(1)
raise SystemExit(0)
PY
      then
        printf '%s\n' "$json"
        return 0
      fi
    fi
    sleep 1
  done
  if [[ "$expected_mode" == "post-set" ]]; then
    fail "timed out waiting for post-set exposure readback"
  fi
  fail "timed out waiting for active darkroom session"
}

start_darktable_host
wait_for_remote_lua

initial_json=$(wait_for_session_payload "$ready_attempts" initial)
set_json=$(run_bridge set-exposure "$requested_exposure")
post_set_json=$(wait_for_session_payload "$post_set_attempts" post-set)

python3 - "$initial_json" "$set_json" "$post_set_json" "$requested_exposure" "$asset_path" <<'PY'
import json, math, os, sys
initial = json.loads(sys.argv[1])
set_payload = json.loads(sys.argv[2])
post_set = json.loads(sys.argv[3])
requested = float(sys.argv[4])
asset = os.path.realpath(sys.argv[5])

def expect_ok(name, payload):
    if payload.get('status') != 'ok':
        raise SystemExit(f'{name} status not ok: {payload}')
    active = payload.get('activeImage') or {}
    if os.path.realpath(active.get('sourceAssetPath') or '') != asset:
        raise SystemExit(f'{name} active image mismatch: {payload}')

def expect_close(name, value, target):
    if not isinstance(value, (int, float)) or math.isnan(value) or abs(value - target) > 1e-6:
        raise SystemExit(f'{name} expected {target}, got {value}')

expect_ok('initial', initial)
expect_ok('set-exposure', set_payload)
expect_ok('post-set', post_set)
initial_current = (initial.get('exposure') or {}).get('current')
if isinstance(initial_current, (int, float)) and not math.isnan(initial_current):
    if abs(initial_current - requested) <= 1e-6:
        raise SystemExit(f'initial exposure already equals requested exposure {requested}: {initial}')
expect_close('set-exposure requested', (set_payload.get('exposure') or {}).get('requested'), requested)
expect_close('set-exposure current', (set_payload.get('exposure') or {}).get('current'), requested)
expect_close('post-set current', (post_set.get('exposure') or {}).get('current'), requested)
print('initial:', json.dumps(initial, separators=(",", ":")))
print('set-exposure:', json.dumps(set_payload, separators=(",", ":")))
print('post-set:', json.dumps(post_set, separators=(",", ":")))
print('result: post-set get-session reports requested exposure')
PY
