# train-0353 verification

Base: `7b2e45efea YG Dolphin 0.3.52`; ancestor check: `BASE-OK`. No version bump, no push.

## Item 1 — embed trace

Added opt-in `ORCA_EMBED_TRACE=1` logging at applied rect/view commands. Backend bounds queries
read only our own NSWindow frame / HWND / X11 window, and run only when the switch is exactly 1.
Format: `embed-trace t=<steady-clock ms> req=<x,y,w,h> real=<x,y,w,h> scale=<s>`.
`view off` reports the active rect as req; too-small rects only hide, so are not applied resizes.
macOS real uses Cocoa screen points; Windows real uses physical screen pixels; X11 uses screen
pixels with scale 1. No new window enumeration APIs. Runtime GUI checks intentionally omitted.
Build and test gate results are recorded below after all four items.
