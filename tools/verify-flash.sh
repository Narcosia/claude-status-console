#!/usr/bin/env bash
# Build, flash, and prove it - then read the boot log back.
#
#   ./tools/verify-flash.sh [/dev/ttyACM0]
#
# deploy.sh compiles and uploads. This wraps it in assertions, because the
# checks I wrote by hand around it lied twice:
#
#   grep -q "Failed uploading" log || echo "upload ok"
#       ...prints "upload ok" when the log file does not exist. A typo'd
#       script name meant nothing was ever built, and this cheerfully
#       reported success.
#
#   for i in $(seq 1 20); do curl -sf .../health && break; sleep 2; done
#       ...prints nothing at all on failure, so a dead device looks
#       indistinguishable from a slow one.
#
# Both failed toward "fine", which is the wrong direction for a check to fail.
# Every assertion here is positive: it must FIND the evidence, and it says so
# out loud when it does not.
#
# The last step is the important one. A green compile, a verified flash and a
# 200 from /health were all true of a build that hung the device the moment a
# finger touched the lights page. Only the boot log shows uiSelfTest() render
# every page.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-/dev/ttyACM0}"
LOG="$(mktemp -t verify-flash.XXXXXX.log)"
IP="${CONSOLE_IP:-192.168.1.200}"

fail() { echo "FAIL: $*" >&2; echo "full log: $LOG" >&2; exit 1; }

echo "building and flashing (log: $LOG)"
"$HERE/deploy.sh" "$PORT" > "$LOG" 2>&1
status=$?

[[ -s "$LOG" ]] || fail "log is empty - did deploy.sh run at all?"
[[ $status -eq 0 ]] || fail "deploy.sh exited $status"

grep -q "Sketch uses" "$LOG" || fail "no size line - the compile did not finish"
grep -E "error:|undefined reference" "$LOG" && fail "compile errors above"

images=$(grep -c "Hash of data verified" "$LOG")
[[ "$images" -ge 2 ]] || fail "only $images verified image(s), expected >= 2"

echo "  $(grep 'Sketch uses' "$LOG")"
echo "  $images images verified"

# Boot log. This is the part that catches a build that flashes fine and then
# cannot draw.
echo "reading boot log..."
boot=$(python3 "$HERE/tools/serial-capture.py" "$PORT" --reset -t 12 2>/dev/null)

if grep -q "selftest: both pages rendered ok" <<<"$boot"; then
  echo "  self-test passed:"
  grep -E "lvgl mem" <<<"$boot" | sed 's/^/    /'
else
  echo "$boot" | tail -20 >&2
  if grep -q "selftest: rendering" <<<"$boot"; then
    fail "hung while rendering the page named above - it never finished"
  fi
  fail "no self-test result in the boot log"
fi

grep -q "setup done" <<<"$boot" || fail "setup never completed"

# Network last: useful, but the weakest of the three signals.
if curl -sf --max-time 3 "http://$IP/health" >/dev/null 2>&1; then
  echo "  serving on $IP"
else
  echo "  WARNING: no HTTP response from $IP (device is up, network is not)" >&2
fi

echo "OK"
rm -f "$LOG"
