#include "layout.h"

size_t computeArcs(size_t count, uint16_t ledCount, ArcSpan *out, size_t maxOut) {
  if (count == 0 || ledCount == 0 || maxOut == 0) return 0;
  if (count > ledCount) count = ledCount;
  if (count > maxOut) count = maxOut;

  uint16_t n = (uint16_t)count;
  uint16_t gap = n > 1 ? 1 : 0;
  uint16_t usable = ledCount - gap * n;
  if (usable < n) {
    gap = 0;
    usable = ledCount;
  }
  uint16_t span = usable / n;
  uint16_t extras = usable % n;

  uint16_t pos = 0;
  for (uint16_t i = 0; i < n; i++) {
    out[i].lo = pos;
    out[i].hi = pos + span + (i < extras ? 1 : 0);
    pos = out[i].hi + gap;
  }
  return n;
}
