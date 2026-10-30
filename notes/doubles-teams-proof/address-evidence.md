# Read-only native address evidence

Collected before the pin implementation, muted/headless via the shared GPU slot and run-lock.
Native controller input only; no probe write/utf16 commands.

`GAME_GLOBAL = 0x805A00E0 -> 0x90181300`; global +0x10 -> `0x90180B40`.

```text
CSS relative frame 1400: task +0x5C8=0, +0x5CB=0; area[i] +0x1C0 (u32)=[0, 0, 0, 0]
CSS relative frame 1460: task +0x5C8=1, +0x5CB=1; area[i] +0x1C0 (u32)=[0, 1, 0, 1]
scSelStage relative frame 1: select +0x33=1; select +0xB8 + i*0x5C +0x0B=[0, 1, 0, 1]
scSelStage relative frame 100: select +0x33=1; select +0xB8 + i*0x5C +0x0B=[0, 1, 0, 1]
scMelee relative frame 1: select +0x33=1; select +0xB8 + i*0x5C +0x0B=[0, 1, 0, 1]
Result: 0x9017F420 +0x24 +i*0x2AC +0x06=[0, 1, 0, 1]
Native mode cycle, CSS 1400: task +0x5C8=0, +0x5CB=0
Native mode cycle, CSS 1460: task +0x5C8=1, +0x5CB=1
Native mode cycle, CSS 1560: task +0x5C8=1, +0x5CB=2
Native mode cycle, CSS 1660: task +0x5C8=1, +0x5CB=3
Native mode cycle, CSS 1760: task +0x5C8=0, +0x5CB=0
After native cycle to off: stage select +0x33=0
```

Probe log address lines (the heaps can move on other runs):
```text
59:25:109 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelctCharacter 1460: dump 815c75e0 700 scSelctCharacter-1460-task
59:25:109 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelctCharacter 1460: dump 8153f060 240 scSelctCharacter-1460-area0
59:25:109 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelctCharacter 1460: dump 814f6080 240 scSelctCharacter-1460-area1
59:25:109 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelctCharacter 1460: dump 814ad0a0 240 scSelctCharacter-1460-area2
59:25:109 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelctCharacter 1460: dump 814640c0 240 scSelctCharacter-1460-area3
59:25:783 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scSelStage 1: dump 90180b40 1000 scSelStage-1-select
59:32:317 Core/Orca/UX/Probe.cpp:273 N[ROLLBACK]: Probe scVsResult 1: dump 9017f420 1400 scVsResult-1-result
```

The select snapshot keeps its previous values during CSS; CSS area data is copied into it on leaving CSS. Its +0x33 team mode and per-player +0x0B colours are the stage select / fight source. The melee init block is rebuilt later, so writing only it during CSS would not establish the select colours.

Reproduce the read-only run with `discovery.py` using the wrapper in `README.md`. The cycle script adds native A presses at 1550, 1650 and 1750; Project+ cycles three team modes, then off. No native flag is written by this change.
