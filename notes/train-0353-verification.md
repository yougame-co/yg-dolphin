# train-0353 verification

Base: `7b2e45efea YG Dolphin 0.3.52`; ancestor check: `BASE-OK`. No version bump, no push.

## Item 1 — embed trace

Added opt-in `ORCA_EMBED_TRACE=1` logging at applied rect/view commands. Backend bounds queries
read only our own NSWindow frame / HWND / X11 window, and run only when the switch is exactly 1.
Format: `embed-trace t=<steady-clock ms> req=<x,y,w,h> real=<x,y,w,h> scale=<s>`.
`view off` reports the active rect as req; too-small rects log their raw request and the
unchanged real bounds after hiding.
macOS real uses Cocoa screen points; Windows real uses physical screen pixels; X11 uses screen
pixels with scale 1. No new window enumeration APIs. Runtime GUI checks intentionally omitted.
Build and test gate results are recorded below after all four items.

## Item 2 — self-embedding result

Added a first-decision latch shared by self-embedding backends, held until ready. Initial native
rect placement counts; hidden/minimized starts wait for actual visibility. Initial embed failure
falls back to a standalone window while retaining the protocol. A parent lost after placement
still stops as before. Adopted sessions disable the latch.
Exact stdout: `orca embed ok` or `orca embed failed <reason>`, where reason is one of
`parent-gone`, `setparent`, `no-parent`, `track-failed`. Never before ready, never twice.
Documented in Status.h and ORCA.md Embedding. Unit tests cover ready gating, first decision,
each failure token and disabled/adopted silence. Native UI checks omitted per task instructions.

## Item 3 — boot allocation failure and memory log

Catch bad_alloc at the EmuThread boot/run boundary (inside its terminal-state guard), plus the
synchronous NoGUI BootCore call. Existing scope guards unwind before reporting. The shared
callable seam allocates nothing and rethrows after the presenter has observed the first frame (also without an Orca profile); other
exception types retain their behavior. Partial HW init is not passed to full HW shutdown. Non-MSVC builds explicitly enable exceptions
for Core.cpp, BootMemory.cpp, MainNoGUI.cpp, the known cache allocator PPCCache.cpp and the test
using the existing per-source pattern; no global exception-mode change. Trace uses the normal ROLLBACK logger enabled by the embedded NoGUI boot; memory
diagnostics use its normal listeners even with category filtering disabled.
Exact Status line: `orca error low_memory Your PC ran out of memory while starting the game. Close other apps first.`
The existing Status finish path retains exit 1 and `orca state ended`, suppressing later errors.
Memory is logged once when BootCore starts and again on errors/reports, log only:
`memory: total <MB> MB, available <MB> MB[, commit <used>/<limit> MB]`.
Windows uses GlobalMemoryStatusEx, macOS hw.memsize + free/inactive host pages, Linux MemTotal /
MemAvailable. MB are integer MiB. Tests inject a throwing boot callable, verify cleanup, exact
error, first-error suppression, exit/ended, propagation after first frame and other exceptions.
