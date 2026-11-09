#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-38271}"
RUNS="${RUNS:-3}"
RUN_SECONDS="${RUN_SECONDS:-30}"
# usb: stream through the adb forward tunnel. wifi: stream over the LAN to a
# tablet already paired with this Mac (adb is still used for logs/screenshots).
TRANSPORT="${TRANSPORT:-usb}"
WIFI_TABLET="${WIFI_TABLET:-}"          # name or id; empty = any paired tablet
# Fail thresholds for desktop compositor -> Android render (real render time
# from MediaCodec onFrameRendered, clocks aligned with the ping offset).
MAX_P50_MS="${MAX_P50_MS:-25}"
MAX_P95_MS="${MAX_P95_MS:-40}"
VALIDATION_SCENE="${VALIDATION_SCENE:-1}" # 1 = moving window, 0 = static desktop
# BURST_EVERY=N: move the cursor over the virtual display every N seconds and
# report the latency of the first frame after each still period ("wake").
BURST_EVERY="${BURST_EVERY:-0}"
OUT_ROOT="${OUT_ROOT:-/tmp/droidscreen-validation}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$OUT_ROOT/$TRANSPORT-$STAMP"
APK="$ROOT_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
BUILD_DIR="${DROIDSCREEN_BUILD_DIR:-$ROOT_DIR/build-macos}"
export DROIDSCREEN_BUILD_DIR="$BUILD_DIR"
BUILT_DESKTOP_APP="$BUILD_DIR/desktop/macos/droidscreen_desktop.app"
DESKTOP_APP="${DESKTOP_APP:-/Applications/DroidScreen.app}"
DESKTOP_BIN="$DESKTOP_APP/Contents/MacOS/droidscreen_desktop"
VALIDATION_HELPER_BIN="$BUILD_DIR/desktop/macos/droidscreen_validation_scene_helper"
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

if [[ "${SKIP_BUILD:-0}" == "1" ]]; then
  echo "[validate] skipping builds"
else
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
fi

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

def luma(row, x):
    i = x * bpp
    return (54 * row[i] + 183 * row[i + 1] + 19 * row[i + 2]) >> 8

boundary_sum = boundary_count = 0
reference_sum = reference_count = 0
for row in rows:
    for x in range(16, width, 16):
        boundary_sum += abs(luma(row, x) - luma(row, x - 1))
        boundary_count += 1
    for x in range(8, width, 16):
        reference_sum += abs(luma(row, x) - luma(row, x - 1))
        reference_count += 1

for y in range(16, height, 16):
    prev = rows[y - 1]
    row = rows[y]
    for x in range(0, width, 2):
        boundary_sum += abs(luma(row, x) - luma(prev, x))
        boundary_count += 1
for y in range(8, height, 16):
    prev = rows[y - 1]
    row = rows[y]
    for x in range(0, width, 2):
        reference_sum += abs(luma(row, x) - luma(prev, x))
        reference_count += 1

boundary_avg = boundary_sum / boundary_count if boundary_count else 0.0
reference_avg = reference_sum / reference_count if reference_count else 0.0
blockiness = max(0.0, boundary_avg / max(0.001, reference_avg) - 1.0)

print(
    f"mean={mean:.2f} variance={variance:.2f} non_black={non_black_ratio:.4f} "
    f"blockiness={blockiness:.3f}"
)
if mean < 5.0 or variance < 2.0 or non_black_ratio < 0.05:
    raise SystemExit("black_or_blank")
PY
}

parse_metrics() {
  local log="$1"
  local desktop_log="${2:-}"
  local run_dir="${3:-}"
  python3 - "$log" "$desktop_log" "$MAX_P50_MS" "$MAX_P95_MS" "$run_dir" <<'PY'
import re
import statistics
import sys

# One Android line every ~2 s, percentiles over every frame of that window:
# decode[sync]: fed=N rendered=N err=N | F fed/s R render/s idle=N skip=N |
#   ring=B bytes N frames Mms dec=N |
#   comp_render n=N p50=X p95=X p99=X max=X | send_render n=... | recv_render n=... |
#   clock_unc=Xms rtt=Xms capsend=Xms | cb=N expired=N
decode_pattern = re.compile(
    r"decode\[[^\]]+\]: fed=(\d+) rendered=(\d+) err=(\d+) \| "
    r"([0-9.]+) fed/s ([0-9.]+) render/s idle=(\d+) skip=(\d+)"
)
def lat_pattern(name):
    return re.compile(
        name + r" n=(\d+) p50=(-?[0-9.]+) p95=(-?[0-9.]+) p99=(-?[0-9.]+) max=(-?[0-9.]+)"
    )
lat_patterns = {k: lat_pattern(k) for k in ("comp_render", "send_render", "recv_render")}
ring_pattern = re.compile(r"ring=(\d+) bytes (\d+) frames ([0-9.]+)ms dec=(\d+)")
clock_pattern = re.compile(r"clock_unc=([0-9.]+)ms rtt=([0-9.]+)ms capsend=([0-9.]+)ms")
quality_pattern = re.compile(
    r"\[quality\] fps=([0-9.]+) kbps=([0-9.]+) frame=([0-9.]+)kb "
    r"delta=([0-9.]+)kb key=([0-9.]+)kb motion=([0-9.]+) "
    r"bits_per_motion=([0-9.]+)"
)
windows = {k: [] for k in lat_patterns}   # (n, p50, p95, p99, max)
feed_fps = []
render_fps = []
feed_errors = []
skips = []
ring_ms = []
clock_unc = []
rtt = []
capsend = []
quality_fps = []
quality_kbps = []
quality_frame_kb = []
quality_motion = []
quality_bits_per_motion = []
for line in open(sys.argv[1], errors="ignore"):
    d = decode_pattern.search(line)
    if not d:
        continue
    feed_errors.append(int(d.group(3)))
    feed_fps.append(float(d.group(4)))
    render_fps.append(float(d.group(5)))
    skips.append(int(d.group(7)))
    for k, pat in lat_patterns.items():
        m = pat.search(line)
        if m and int(m.group(1)) > 0:
            windows[k].append((int(m.group(1)),) + tuple(float(x) for x in m.groups()[1:]))
    r = ring_pattern.search(line)
    if r:
        ring_ms.append(float(r.group(3)))
    c = clock_pattern.search(line)
    if c:
        clock_unc.append(float(c.group(1)))
        rtt.append(float(c.group(2)))
        capsend.append(float(c.group(3)))

stats_pattern = re.compile(r"\[Stats\].*rtt=(\d+) us.*send_q=(\d+)(?: enc_ms=([0-9.]+))?")
rtt_desktop = []
send_q = []
enc_ms = []
if len(sys.argv) > 2 and sys.argv[2]:
    try:
        for line in open(sys.argv[2], errors="ignore"):
            st = stats_pattern.search(line)
            if st:
                rtt_desktop.append(int(st.group(1)) / 1000.0)
                send_q.append(int(st.group(2)))
                if st.group(3) and float(st.group(3)) > 0:
                    enc_ms.append(float(st.group(3)))
    except OSError:
        pass

if len(sys.argv) > 2 and sys.argv[2]:
    try:
        quality_lines = open(sys.argv[2], errors="ignore")
    except OSError:
        quality_lines = []
    for line in quality_lines:
        q = quality_pattern.search(line)
        if q:
            quality_fps.append(float(q.group(1)))
            quality_kbps.append(float(q.group(2)))
            quality_frame_kb.append(float(q.group(3)))
            quality_motion.append(float(q.group(6)))
            quality_bits_per_motion.append(float(q.group(7)))

def p50(xs):
    return statistics.median(xs)

def weighted_median(pairs):
    """pairs: (weight, value); median of the values weighted by sample count."""
    pairs = sorted(pairs, key=lambda p: p[1])
    total = sum(w for w, _ in pairs)
    acc = 0
    for w, v in pairs:
        acc += w
        if acc * 2 >= total:
            return v
    return pairs[-1][1]

def summary(name):
    ws = windows[name]
    if not ws:
        return None
    return {
        "n": sum(w[0] for w in ws),
        "p50": weighted_median([(w[0], w[1]) for w in ws]),
        "p95": weighted_median([(w[0], w[2]) for w in ws]),
        "p99_max": max(w[3] for w in ws),
        "max": max(w[4] for w in ws),
    }

if not feed_fps:
    print("metrics=none (no decode[] lines)")
    raise SystemExit(2)

out = []
sums = {k: summary(k) for k in windows}
for k in ("comp_render", "send_render", "recv_render"):
    s_ = sums[k]
    if s_:
        out.append(
            f"{k}_n={s_['n']} {k}_p50={s_['p50']:.1f} {k}_p95={s_['p95']:.1f} "
            f"{k}_p99max={s_['p99_max']:.1f} {k}_max={s_['max']:.1f}"
        )
    else:
        out.append(f"{k}_n=0")
out.append(
    f"fed_fps_p50={p50(feed_fps):.1f} render_fps_p50={p50(render_fps):.1f}"
    f" feed_errors_max={max(feed_errors)} skipped_max={max(skips)}"
)
if ring_ms:
    out.append(f"ring_ms_p50={p50(ring_ms):.0f} ring_ms_max={max(ring_ms):.0f}")
if clock_unc:
    out.append(
        f"clock_unc_p50={p50(clock_unc):.2f} rtt_p50={p50(rtt):.1f} capsend_p50={p50(capsend):.1f}"
    )
if quality_kbps:
    out.append(
        f"quality_kbps_p50={p50(quality_kbps):.0f}"
        f" motion_p50={p50(quality_motion):.2f}"
        f" bits_per_motion_p50={p50(quality_bits_per_motion):.0f}"
        f" qframe_kb_p50={p50(quality_frame_kb):.1f}"
    )
if rtt_desktop:
    out.append(
        f"desk_rtt_p50={p50(rtt_desktop):.1f} desk_rtt_max={max(rtt_desktop):.1f}"
        f" send_q_max={max(send_q)}"
    )
if enc_ms:
    out.append(f"vt_enc_ms_p50={p50(enc_ms):.2f}")

wake = []
wake_pattern = re.compile(r"wake: gap=\d+ms comp_render=(-?[0-9.]+)ms")
for line in open(sys.argv[1], errors="ignore"):
    w = wake_pattern.search(line)
    if w and float(w.group(1)) >= 0:
        wake.append(float(w.group(1)))
if wake:
    out.append(f"wake_n={len(wake)} wake_p50={p50(wake):.1f} wake_max={max(wake):.1f}")

import os
if len(sys.argv) > 5 and sys.argv[5]:
    try:
        cpu = [float(x) for x in open(os.path.join(sys.argv[5], "cpu.txt")) if x.strip()]
        cpu = cpu[3:] or cpu  # skip startup samples
        if cpu:
            out.append(f"mac_cpu_p50={p50(cpu):.1f}% mac_cpu_max={max(cpu):.1f}%")
    except (OSError, ValueError):
        pass
    try:
        acpu = open(os.path.join(sys.argv[5], "android-cpu.txt")).read().split()
        if acpu:
            out.append(f"android_cpu={acpu[0]}%")
    except OSError:
        pass
print(" ".join(out))

# Gate: compositor -> render needs the desktop's display time and a clock
# offset estimate (telemetry v3). Without them there is nothing to gate on.
gate = sums["comp_render"]
if not gate:
    print("metrics=none (no comp_render samples: desktop sent no display_time/clock offset)")
    raise SystemExit(2)
if (gate["p50"] > float(sys.argv[3]) or gate["p95"] > float(sys.argv[4])
        or max(feed_errors) > 3):
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
  DESKTOP_ARGS=(--port "$PORT")
  # Extra args, e.g. "-DroidScreenFPS 60 -DroidScreenBitrate 20000": NSUserDefaults
  # reads "-Key value" pairs as overrides without touching saved settings.
  if [[ -n "${EXTRA_DESKTOP_ARGS:-}" ]]; then
    read -r -a extra_args <<< "$EXTRA_DESKTOP_ARGS"
    DESKTOP_ARGS+=("${extra_args[@]}")
  fi
  if [[ "$TRANSPORT" == "wifi" ]]; then
    DESKTOP_ARGS+=(--wifi)
    if [[ -n "$WIFI_TABLET" ]]; then
      DESKTOP_ARGS+=("$WIFI_TABLET")
    fi
  else
    adb forward "tcp:$PORT" "tcp:$PORT"
  fi
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

  DROIDSCREEN_VALIDATION_SCENE="$VALIDATION_SCENE" \
  DROIDSCREEN_VALIDATION_HELPER_PATH="$VALIDATION_HELPER_BIN" \
    "$DESKTOP_BIN" "${DESKTOP_ARGS[@]}" > "$RUN_DIR/desktop.log" 2>&1 &
  DESKTOP_PID=$!
  # Sample desktop CPU (% of one core) every 2 s for the cost report.
  ( while kill -0 "$DESKTOP_PID" 2>/dev/null; do
      ps -o %cpu= -p "$DESKTOP_PID" 2>/dev/null | tr -d ' '
      sleep 2
    done ) > "$RUN_DIR/cpu.txt" &
  CPU_PID=$!

  wait_for_log "handshake complete" "$RUN_DIR/logcat.txt" 35
  if [[ "$TRANSPORT" == "wifi" ]] && ! grep -q "connected (wifi)" "$RUN_DIR/logcat.txt"; then
    echo "[validate] expected a Wi-Fi connection but the tablet saw USB" >&2
    exit 4
  fi
  wait_for_log "decode\[" "$RUN_DIR/logcat.txt" 35
  if [[ "$BURST_EVERY" != "0" ]]; then
    VD_ID="$(grep -aoE 'Virtual display created \(ID=[0-9]+' "$DESKTOP_DEBUG_LOG" | tail -1 | grep -oE '[0-9]+$')"
    swiftc -O "$ROOT_DIR/scripts/tools/burst_cursor.swift" -o "$BUILD_DIR/burst_cursor" 2>/dev/null || true
    "$BUILD_DIR/burst_cursor" "$VD_ID" "$BURST_EVERY" "$RUN_SECONDS" &
    BURST_PID=$!
  fi
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

  # Tablet-side CPU of the app at the end of the run (% of one core).
  ANDROID_PID="$(adb shell pidof com.droidscreen.app 2>/dev/null | tr -d '\r')"
  if [[ -n "$ANDROID_PID" ]]; then
    adb shell top -b -n 1 -p "$ANDROID_PID" 2>/dev/null | awk -v p="$ANDROID_PID" '$1==p {print $9}' > "$RUN_DIR/android-cpu.txt"
  fi

  kill "$DESKTOP_PID" 2>/dev/null || true
  wait "$DESKTOP_PID" 2>/dev/null || true
  unset DESKTOP_PID
  kill "$CPU_PID" 2>/dev/null || true
  if [[ -n "${BURST_PID:-}" ]]; then
    kill "$BURST_PID" 2>/dev/null || true
    unset BURST_PID
  fi
  if [[ -f "$DESKTOP_DEBUG_LOG" ]]; then
    cp "$DESKTOP_DEBUG_LOG" "$RUN_DIR/desktop-debug.log"
  fi

  kill "$LOGCAT_PID" 2>/dev/null || true
  wait "$LOGCAT_PID" 2>/dev/null || true
  unset LOGCAT_PID

  set +e
  parse_metrics "$RUN_DIR/logcat.txt" "$RUN_DIR/desktop-debug.log" "$RUN_DIR" | tee "$RUN_DIR/metrics.txt"
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
