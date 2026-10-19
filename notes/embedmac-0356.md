# embedmac-0356

macOS EMBED caches parent bounds/visibility/owner and limits parent queries to 100 ms, ordering
queries to 250 ms. Owner activation immediately refreshes ordering; explicit rect/visible/focus
commands can refresh both with a 20 ms floor (deferred requests remain pending). Cached bounds
still apply rect changes immediately. Startup sleeps 20/50/100/200 ms, capped at 200; the existing
time-based two-second parent-loss grace and fallback/stop decisions remain. The event loop waits
up to 1/60 s; rendering runs separately. Mouse interception changes only when control hit state
changes. A nonactivating NSPanel prevents embedded clicks from activating YG Dolphin while
preserving Orb::ClickControls. The parent-gone fallback replaces the panel style with the existing
titled/resizable mask before becoming a regular app. Standalone behavior, other platforms,
rendering, version and the stdin protocol are unchanged.

## Window-list API calls per second (code reading)

| State | Before | After |
| --- | --- | --- |
| Steady visible, no commands/activations | ~120 IncludingWindow + 120 Above = 240 | <=10 IncludingWindow + 4 Above = 14 |
| Missing/minimized parent, All fallback | ~120 IncludingWindow + 120 All = 240 | <=10 IncludingWindow + 10 All = 20; no Above while hidden |
| Sustained explicit commands | Not bounded by the loop timeout | <=50 parent refreshes + 50 Above, or 50 IncludingWindow + 50 All on misses |
| Startup with absent parent | ~50 attempts / 100 calls | 20/50/100/200 ms sleeps, then <=5 attempts / 10 calls |

Normal visible tracking falls about 94%. Owner-activation notifications add immediate Above
checks. These are API counts, not CPU measurements or proof that the 10-18 hang cannot recur.

## Deferred manual smoke: spare Mac or release kit only

**Do not run on Luis's Mac.** Automated validation for this task creates no windows. Use an offline
local disc and one YG Dolphin instance at a time. From the built worktree, substitute the disc path:

```sh
smoke_user="$(mktemp -d -t embedmac-0356-user)"
ORCA_EMBED_TRACE=1 build-embedmac/embed-test-host \
  build-embedmac/Binaries/dolphin-emu-nogui \
  -u "$smoke_user" -e /absolute/path/to/local-test-disc.iso \
  -C Dolphin.Display.Fullscreen=False
```

Wait for `ready` / `orca embed ok`. Click orb/Delta: actions must work while YouGame stays
frontmost and owns the keyboard. Other picture clicks must print `host> mouse-down ... in the box`.
In host stdin, issue these individually and inspect each transition:

```text
move 260 260
size 960 640
panel 200
panel 0
send blur
send focus
send rect 0 0 1 1
send show
size 1000 660
send hide
send show
mini
unmini
front
```

The tiny rect stays hidden through `show`; resizing restores the valid host rectangle. Hide/mini
must drop input and restore cleanly. Movement follows within roughly 100 ms; rapid valid rects
apply immediately from cached parent bounds. Switch to another app and back: YG Dolphin must
return above YouGame promptly, below host menus, without activating. Issue `closewin` with stdin
open: YG Dolphin must stop after about two seconds. Then `exit` the host.

Separately check the initially hidden parent-gone fallback, keeping stdin open:

```sh
build-embedmac/Binaries/dolphin-emu-nogui --embed --parent 4294967294 \
  --rect 0 0 1 1 -u "$smoke_user" -e /absolute/path/to/local-test-disc.iso \
  -C Dolphin.Display.Fullscreen=False
```

Expect a titled window after the grace (up to one 200 ms retry later), then
`orca embed failed parent-gone` after `ready`. Verify key/main focus, clicks and closing. Also
check a normal standalone launch. Finally, on the spare Mac/release kit, repeat the real desktop
app's jailed EMBED launch in full screen with Game Mode, orb/Delta clicks and hide/show/focus
changes; record frontmost app and responsiveness. This windowed smoke is deferred, not run here.
