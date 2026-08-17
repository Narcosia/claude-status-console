#!/usr/bin/env python3
"""Read the console's serial output, with DTR asserted.

    ./tools/serial-capture.py                 # capture 15s of whatever it says
    ./tools/serial-capture.py --reset         # reset first, catch the boot log
    ./tools/serial-capture.py --reset -t 30   # ...for 30 seconds

DTR is the whole point of this script existing.

There is no UART bridge on this board; Serial is the ESP32-S3's native USB CDC.
The firmware calls Serial.setTxTimeoutMs(0) so that printing never blocks when
nothing is listening - without it, a print stalls the render loop and shows up
as display stutter. The cost is that the device drops output entirely unless a
host is *visibly* attached, and DTR is how it decides.

A capture script that opens the port without asserting DTR therefore returns
nothing at all, on a perfectly healthy device. That silence reads exactly like
"the code never ran". It cost most of a day: several rounds of "zero touches
registered" were the capture failing, not the firmware.

--reset drives EN via RTS (the classic esptool sequence) so the boot log can be
seen. uiSelfTest() reports the LVGL pool at boot, and a page that fails to
render hangs there - so the boot log is where a whole class of bug shows up.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial:  pip install pyserial")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="/dev/ttyACM0")
    ap.add_argument("-t", "--seconds", type=float, default=15)
    ap.add_argument("--reset", action="store_true",
                    help="reset the board first, to capture the boot log")
    args = ap.parse_args()

    try:
        s = serial.Serial(args.port, 115200, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"cannot open {args.port}: {e}")

    if args.reset:
        s.dtr = False
        s.rts = True            # EN low
        time.sleep(0.15)
        s.rts = False           # release, board boots
        time.sleep(0.05)

    s.dtr = True                # REQUIRED - see the note above
    s.reset_input_buffer()

    total, end = 0, time.time() + args.seconds
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            total += len(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()

    print(f"\n--- {total} bytes in {args.seconds:.0f}s ---", file=sys.stderr)
    if total == 0:
        print("nothing captured. The device may be hung - but check first that "
              "something else is not already holding the port.", file=sys.stderr)


if __name__ == "__main__":
    main()
