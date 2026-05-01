#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-38271}"
RUNS="${RUNS:-3}"
RUN_SECONDS="${RUN_SECONDS:-30}"
OUT_ROOT="${OUT_ROOT:-/tmp/droidscreen-validation}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$OUT_ROOT/moonlight-port-$STAMP"
APK="$ROOT_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
BUILT_DESKTOP_APP="$ROOT_DIR/build-macos/desktop/macos/droidscreen_desktop.app"
DESKTOP_APP="${DESKTOP_APP:-/Applications/DroidScreen.app}"
DESKTOP_BIN="$DESKTOP_APP/Contents/MacOS/droidscreen_desktop"
VALIDATION_HELPER_BIN="$ROOT_DIR/build-macos/desktop/macos/droidscreen_validation_scene_helper"
DESKTOP_DEBUG_LOG="$HOME/Library/Logs/DroidScreen/droidscreen.log"

mkdir -p "$OUT_DIR"

cleanup() {
  if [[ -n "${DESKTOP_PID:-}" ]]; then
    kill "$DESKTOP_PID" 2>/dev/null || true
    wait "$DESKTOP_PID" 2>/dev/null || true
  fi
  if [[ -n "${LOGCAT_PID:-}" ]]; then
    kill "$LOGCAT_PID" 2>/dev/null || true
    wait "$LOGCAT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "[validate] output: $OUT_DIR"

echo "[validate] building Android"
(
  cd "$ROOT_DIR/android"
  ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}" \
  ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}" \
  JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home}" \
  ./gradlew :app:assembleDebug
)

echo "[validate] building macOS"
"$ROOT_DIR/scripts/build_macos.sh"

if [[ "${SKIP_MACOS_INSTALL:-0}" == "1" ]]; then
  echo "[validate] skipping macOS app install; using built app"
  DESKTOP_APP="$BUILT_DESKTOP_APP"
  DESKTOP_BIN="$DESKTOP_APP/Contents/MacOS/droidscreen_desktop"
else
  echo "[validate] installing signed macOS app to $DESKTOP_APP"
  if pgrep -f "$DESKTOP_APP/Contents/MacOS/droidscreen_desktop" >/dev/null 2>&1; then
    pkill -f "$DESKTOP_APP/Contents/MacOS/droidscreen_desktop" || true
    sleep 1
  fi
  rm -rf "$DESKTOP_APP"
  /usr/bin/ditto "$BUILT_DESKTOP_APP" "$DESKTOP_APP"
  xattr -dr com.apple.quarantine "$DESKTOP_APP" 2>/dev/null || true
  codesign --verify --deep --strict --verbose=2 "$DESKTOP_APP"
fi

echo "[validate] installing APK"
if [[ "${SKIP_INSTALL:-0}" == "1" ]]; then
  echo "[validate] skipping APK install"
else
  adb install --no-streaming -r "$APK" >/dev/null
  sleep 2
fi

analyze_png() {
  local file="$1"
  python3 - "$file" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
data = open(path, "rb").read()
if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    raise SystemExit("not_png")

pos = 8
width = height = color_type = bit_depth = None
raw = bytearray()
while pos + 8 <= len(data):
    length = struct.unpack(">I", data[pos:pos + 4])[0]
    ctype = data[pos + 4:pos + 8]
    chunk = data[pos + 8:pos + 8 + length]
    pos += 12 + length
    if ctype == b"IHDR":
        width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])[:4]
    elif ctype == b"IDAT":
        raw.extend(chunk)
    elif ctype == b"IEND":
        break

if bit_depth != 8 or color_type not in (2, 6):
    raise SystemExit(f"unsupported_png bit_depth={bit_depth} color={color_type}")

bpp = 4 if color_type == 6 else 3
stride = width * bpp
decoded = zlib.decompress(bytes(raw))
prev = bytearray(stride)
rows = []
idx = 0
for _ in range(height):
    f = decoded[idx]
    idx += 1
    row = bytearray(decoded[idx:idx + stride])
    idx += stride
    for i in range(stride):
        left = row[i - bpp] if i >= bpp else 0
        up = prev[i]
        up_left = prev[i - bpp] if i >= bpp else 0
        if f == 1:
            row[i] = (row[i] + left) & 0xff
        elif f == 2:
            row[i] = (row[i] + up) & 0xff
        elif f == 3:
            row[i] = (row[i] + ((left + up) // 2)) & 0xff
        elif f == 4:
            p = left + up - up_left
            pa = abs(p - left)
            pb = abs(p - up)
            pc = abs(p - up_left)
            pr = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
            row[i] = (row[i] + pr) & 0xff
    rows.append(row)
    prev = row

count = width * height
total = 0.0
total_sq = 0.0
non_black = 0
for row in rows:
    for i in range(0, len(row), bpp):
        r, g, b = row[i], row[i + 1], row[i + 2]
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        total += y
        total_sq += y * y
        if r > 8 or g > 8 or b > 8:
            non_black += 1

mean = total / count
variance = max(0.0, total_sq / count - mean * mean)
non_black_ratio = non_black / count
print(f"mean={mean:.2f} variance={variance:.2f} non_black={non_black_ratio:.4f}")
if mean < 5.0 or variance < 2.0 or non_black_ratio < 0.05:
    raise SystemExit("black_or_blank")
PY
}

parse_metrics() {
  local log="$1"
  python3 - "$log" <<'PY'
import re
import statistics
import sys

lat_pattern = re.compile(
    r"lat feed=([0-9.]+)ms release=([0-9.]+)ms desk=([0-9.]+)ms android=([0-9.]+)ms rtt=([0-9.]+)ms(?: capenc=([0-9.]+)ms encsend=([0-9.]+)ms)?"
)
decode_pattern = re.compile(
    r"decode\[[^\]]+\]: fed=(\d+) rendered=(\d+) err=(\d+) \| "
    r"([0-9.]+) fed/s ([0-9.]+) render/s"
)
feed = []
release = []
desk = []
android = []
rtt = []
capenc = []
encsend = []
feed_fps = []
render_fps = []
feed_errors = []
for line in open(sys.argv[1], errors="ignore"):
    m = lat_pattern.search(line)
    if m:
        vals = [float(x) for x in m.groups()[:5]]
        feed.append(vals[0])
        release.append(vals[1])
        desk.append(vals[2])
        android.append(vals[3])
        rtt.append(vals[4])
        if m.group(6) is not None:
            capenc.append(float(m.group(6)))
            encsend.append(float(m.group(7)))

    d = decode_pattern.search(line)
    if d:
        feed_errors.append(int(d.group(3)))
        feed_fps.append(float(d.group(4)))
        render_fps.append(float(d.group(5)))

if not release:
    print("metrics=none")
    raise SystemExit(2)

def p50(xs):
    return statistics.median(xs)

extra = ""
if capenc:
    extra = f" capenc_p50={p50(capenc):.1f} encsend_p50={p50(encsend):.1f}"
if feed_fps:
    extra += (
        f" fed_fps_p50={p50(feed_fps):.1f}"
        f" render_fps_p50={p50(render_fps):.1f}"
        f" feed_errors_max={max(feed_errors)}"
    )
print(
    f"samples={len(release)} "
    f"feed_p50={p50(feed):.1f} release_p50={p50(release):.1f} "
    f"desk_p50={p50(desk):.1f} android_p50={p50(android):.1f} rtt_p50={p50(rtt):.1f}"
    f"{extra} release_last={release[-1]:.1f}"
)
if p50(release) > 15.0 or (feed_errors and max(feed_errors) > 3):
    raise SystemExit(3)
PY
}

wait_for_log() {
  local pattern="$1"
  local log="$2"
  local timeout_s="$3"
  local start
  start="$(date +%s)"
  while true; do
    if grep -q "$pattern" "$log" 2>/dev/null; then
      return 0
    fi
    if (( "$(date +%s)" - start >= timeout_s )); then
      echo "[validate] timed out waiting for log pattern: $pattern" >&2
      return 1
    fi
    sleep 0.5
  done
}

overall_status=0

for run in $(seq 1 "$RUNS"); do
  RUN_DIR="$OUT_DIR/run-$run"
  mkdir -p "$RUN_DIR"
  echo "[validate] run $run/$RUNS"

  adb forward --remove "tcp:$PORT" >/dev/null 2>&1 || true
  adb forward "tcp:$PORT" "tcp:$PORT"
  adb logcat -c
  adb logcat -v time > "$RUN_DIR/logcat.txt" &
  LOGCAT_PID=$!

  adb shell am force-stop com.droidscreen.app || true
  for start_attempt in 1 2 3; do
    adb shell am start -n com.droidscreen.app/.MainActivity >/dev/null
    sleep 2
    if adb shell pidof com.droidscreen.app >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  if ! adb shell pidof com.droidscreen.app >/dev/null 2>&1; then
    echo "[validate] DroidScreen Android app did not stay running" >&2
    exit 2
  fi
  sleep 2
  adb exec-out screencap -p > "$RUN_DIR/01-waiting.png"

  DROIDSCREEN_VALIDATION_SCENE=1 \
  DROIDSCREEN_VALIDATION_HELPER_PATH="$VALIDATION_HELPER_BIN" \
    "$DESKTOP_BIN" --port "$PORT" > "$RUN_DIR/desktop.log" 2>&1 &
  DESKTOP_PID=$!

  wait_for_log "handshake complete" "$RUN_DIR/logcat.txt" 35
  wait_for_log "lat feed=" "$RUN_DIR/logcat.txt" 35
  sleep 2
  adb exec-out screencap -p > "$RUN_DIR/02-active-5s.png"
  analyze_png "$RUN_DIR/02-active-5s.png" > "$RUN_DIR/02-active-5s.analysis"

  sleep 15
  adb exec-out screencap -p > "$RUN_DIR/03-active-20s.png"
  analyze_png "$RUN_DIR/03-active-20s.png" > "$RUN_DIR/03-active-20s.analysis"

  remaining=$((RUN_SECONDS - 20))
  if (( remaining > 0 )); then
    sleep "$remaining"
  fi

  adb exec-out screencap -p > "$RUN_DIR/04-active-final.png"
  analyze_png "$RUN_DIR/04-active-final.png" > "$RUN_DIR/04-active-final.analysis"

  kill "$DESKTOP_PID" 2>/dev/null || true
  wait "$DESKTOP_PID" 2>/dev/null || true
  unset DESKTOP_PID
  if [[ -f "$DESKTOP_DEBUG_LOG" ]]; then
    cp "$DESKTOP_DEBUG_LOG" "$RUN_DIR/desktop-debug.log"
  fi

  kill "$LOGCAT_PID" 2>/dev/null || true
  wait "$LOGCAT_PID" 2>/dev/null || true
  unset LOGCAT_PID

  set +e
  parse_metrics "$RUN_DIR/logcat.txt" | tee "$RUN_DIR/metrics.txt"
  metrics_status=${PIPESTATUS[0]}
  set -e
  if (( metrics_status != 0 )); then
    overall_status=$metrics_status
    echo "[validate] run $run failed latency/feed criteria (status=$metrics_status)" | tee -a "$RUN_DIR/metrics.txt"
  fi
done

if (( overall_status != 0 )); then
  echo "[validate] FAIL: artifacts in $OUT_DIR"
  exit "$overall_status"
fi

echo "[validate] PASS: artifacts in $OUT_DIR"
