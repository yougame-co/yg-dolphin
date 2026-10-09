#!/usr/bin/env python3
"""match-latency-shots.py <run dir>: what each player saw, from a match-latency.mjs --video Metal --shots run.

Each side asked Orca for a screenshot every --shots ms (shot_req events); Orca saves each at its
next present, so a screenshot's file time says when a picture was presented, and a stretch with no
saved screenshot is a stretch with no new picture (a frozen screen). For every match this prints the
present gaps over 150 ms per side (relative to `matched`), and writes a contact sheet per match
(shots/<match>-<side>.png: one frame every 250 ms from -0.5 s to +7 s, labelled with its time).
"""
import json, os, sys
from PIL import Image, ImageDraw

run = sys.argv[1]
sides = [f[:-10] for f in os.listdir(run) if f.endswith(".raw.jsonl")]
out = []
for side in sorted(sides):
    rows = [json.loads(l) for l in open(os.path.join(run, f"{side}.raw.jsonl")) if l.strip()]
    matched = [(r["n"], r["t"]) for r in rows if r.get("ev") == "matched"]
    idx_path = os.path.join(run, "shots", side, "index.json")
    if not os.path.exists(idx_path):
        print(f"{side}: no shots")
        continue
    shots = json.load(open(idx_path))
    by_name = {s["f"][:-4]: s for s in shots}
    for n, t0 in matched:
        tag = f"m{n}-"
        mine = sorted(((s["mtime"] - t0, name) for name, s in by_name.items() if name.startswith(tag)))
        if not mine:
            continue
        times = [t for t, _ in mine]
        gaps = [(round(a), round(b - a)) for a, b in zip(times, times[1:]) if b - a > 150]
        print(f"{side} match {n}: {len(mine)} shots from {round(times[0])} to {round(times[-1])} ms; "
              f"gaps over 150 ms (start+length): {' '.join(f'{a}+{g}' for a, g in gaps) or 'none'}")
        out.append({"side": side, "n": n, "gaps": gaps, "first": times[0], "last": times[-1]})
        # Contact sheet: the shot presented at or just after each 250 ms step.
        steps = [s for s in range(-500, 7001, 250)]
        tiles = []
        for s in steps:
            pick = next(((t, nm) for t, nm in mine if t >= s), None)
            if pick is None:
                break
            tiles.append((s, pick))
        if not tiles:
            continue
        w, h = 320, 264
        cols = 6
        rows_n = (len(tiles) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * w, rows_n * (h + 18)), "black")
        draw = ImageDraw.Draw(sheet)
        for i, (s, (t, nm)) in enumerate(tiles):
            try:
                im = Image.open(os.path.join(run, "shots", side, nm + ".png")).convert("RGB").resize((w, h))
            except Exception:
                continue
            x, y = (i % cols) * w, (i // cols) * (h + 18)
            sheet.paste(im, (x, y + 18))
            draw.text((x + 4, y + 2), f"{side} +{s / 1000:.2f}s (shown {t / 1000:.2f}s)", fill="white")
        p = os.path.join(run, "shots", f"m{n}-{side}.png")
        sheet.save(p)
        print(f"  contact sheet {p}")
json.dump(out, open(os.path.join(run, "vis.json"), "w"), indent=1)
