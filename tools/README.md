# tools

Diagnostics that earned their place. Each one exists because something went
wrong that it would have caught, or did catch. The reasoning behind them is in
[../docs/LESSONS.md](../docs/LESSONS.md).

Nothing here has credentials in it — `tuya-probe.py` reads `config.h`. An
earlier generation of these scripts had the local key pasted into each file,
which meant they had to be destroyed rather than kept. Don't do that again.

| Tool | Use it when |
|---|---|
| `verify-flash.sh` | Always. Build, flash, and read the boot log back. |
| `serial-capture.py` | The device is misbehaving and you want its own account. |
| `tuya-discover.py` | Before touching Tuya anything — *which device is this?* |
| `tuya-probe.py` | Asking the light what it actually supports. |

```bash
./tools/verify-flash.sh                  # the one to reach for by default
./tools/serial-capture.py --reset -t 20  # boot log, DTR asserted
./tools/tuya-discover.py                 # device ids and protocol versions
./tools/tuya-probe.py query              # dump every datapoint
./tools/tuya-probe.py set 20 true        # switch the light on
./tools/tuya-probe.py watch 30           # poll for changes
```

Python tools need `pyserial` and `cryptography`.

## Also in the firmware

Diagnostics that live on the device rather than here:

| | |
|---|---|
| `uiSelfTest()` | Renders every page at boot, reports LVGL pool usage. On by default — it costs two frames and catches a class of hang that a fingertip would otherwise have to find. |
| `POST /ringscan` | Walks the candidate GPIOs one at a time. This is what found GPIO18 after a probe that drove three pins at once proved nothing. |
| `POST /ringzero` | Marks LED 0 with a direction trail — for setting `RING_ZERO_DEG` and `RING_CLOCKWISE` once the enclosure fixes the ring's orientation. |
| `POST /ringtest` | Test pattern across the whole ring. |
| `POST /clear` | Forget every session. |
| `UI_PROFILE` | Frame timing and ring heartbeat. Default 0 — these prints block on USB CDC and cause the stutter they measure. |
| `TOUCH_DEBUG` | Per-press coordinates and which object claimed them. Default 0, same reason. |

`stage-demo.sh` fills the screen with invented sessions for photographs, so
real project names stay out of the README.

**The HTTP diagnostics are unauthenticated**, like `/hook` itself. Fine on a
home LAN, worth knowing before putting this anywhere else — `/clear` wipes the
session list and `/ringtest` takes over the ring.
