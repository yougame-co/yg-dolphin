# Orca's texture pack

`Data/Sys/Orca/Textures/RSBE01/` holds the labels of the Online menu (ORCA.md "Online menu"):
Dolphin hires textures that Orca loads in every session, so the game's own buttons read PLAY
ONLINE, CASUAL and RANKED and Brawl's page title reads ONLINE. Brawl and Project+ both load their
textures as `RSBE01`; their labels hash differently, so one folder serves both.

| File (and its `_mip1`, `_mip2`)      | Replaces                                                                                        | Game     |
| ------------------------------------ | ----------------------------------------------------------------------------------------------- | -------- |
| `tex1_127x96_fdd5334e5af7357c_2.png` | the main menu's "NINTENDO Wi-Fi CONNECTION" button label (MenMainFrTop15)                       | Brawl    |
| `tex1_120x16_a56825159146a723_0.png` | the page title "NINTENDO WFC" (MenMainTtlJ.03; it heads the ONLINE pages and their breadcrumbs) | Brawl    |
| `tex1_144x88_115d82941c7412cd_2.png` | With Anyone's "BASIC BRAWL" (MenMainWifi22): CASUAL                                             | Brawl    |
| `tex1_160x80_e364b4db513567ba_2.png` | With Anyone's "TEAM BATTLE" (MenMainWifi23): RANKED                                             | Brawl    |
| `tex1_144x88_ecd09d11be34045f_5.png` | With Anyone's "BASIC VERSUS" (MenMainWifi22): CASUAL                                            | Project+ |
| `tex1_160x80_5b8f52ac0127b2a8_5.png` | With Anyone's "TEAM BATTLE" (MenMainWifi23): RANKED                                             | Project+ |

Project+'s own button already reads PLAY ONLINE and its title ONLINE.

The art is ours, drawn by `make-labels.py` (needs Pillow) in each game's style: Brawl's black heavy
letters with a white rim and halo, Project+'s black condensed letters with a white outline, and
Brawl's ONLINE title in a condensed face like the game's own titles (SELECT MODE, / WITH ANYONE), at
the height of theirs. Nothing in it comes from the game's art; the boxes in the script are where the
game's own letters sit in its textures, so ours sit in the same place. Each label is 4x the game's
size (Dolphin takes any integer multiple), with mip levels down to the game's own size
(`<name>_mip1.png` at 2x, `_mip2.png` at 1x): Dolphin gives a custom texture's sampler its mip
levels even where the game's texture has none, so a label drawn at 1x internal resolution reads the
clean 1x level instead of the 4x one every fourth texel (which breaks thin strokes, like Anton's N).
Regenerate with:

```bash
python3 Tools/orca/textures/make-labels.py            # writes Data/Sys/Orca/Textures/RSBE01
python3 Tools/orca/textures/make-labels.py --out /tmp/x --sheet /tmp/sheet.png   # a preview
```

The output is byte-identical for the same Pillow (11.3.0 made the committed files).

## Fonts

Both fonts are from [google/fonts](https://github.com/google/fonts) and are licensed under the
SIL Open Font License, Version 1.1. Their licence texts, with the copyright notices, are next to
them in `fonts/`. Neither is shipped with Orca: only the PNGs drawn with them are.

| Font                             | Use                              | Licence                                                                         | Source              | SHA-256                                                            |
| -------------------------------- | -------------------------------- | ------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------ |
| `fonts/ArchivoBlack-Regular.ttf` | Brawl's labels                   | `fonts/OFL-ArchivoBlack.txt` (Copyright 2017 The Archivo Black Project Authors) | `ofl/archivoblack/` | `dd9a89a019b4849f66ab75455fe7bdf931311042cbb0f0f97acc061539703180` |
| `fonts/Anton-Regular.ttf`        | Project+'s labels, Brawl's title | `fonts/OFL-Anton.txt` (Copyright 2020 The Anton Project Authors)                | `ofl/anton/`        | `a4ba3a92350ebb031da0cb47630ac49eb265082ca1bc0450442f4a83ab947cab` |

## Rules

- Replacing a texture changes only what is drawn: the texture cache hashes the game's texture in
  RAM and swaps the host GPU texture; nothing Orca draws reaches emulated memory (Profile.cpp
  forces EFB copies to texture only, EFB access and bounding box off). The pack is therefore not
  in the compatibility key. `YG_HASHLOG` is identical with and without it in both games.
- In a session Orca's pack keeps the names it has: a player's own pack in `<user>/Load/Textures`
  (a Brawl HD pack holding the game's button or title, say) never brings back a texture Orca
  replaces, and has every other name (`CollectHiresTextureFiles` in
  `VideoCommon/HiresTextures.cpp`, unit test `OrcaOnlineMenuTextures`). Orca's is always
  preloaded, the player's follows their "Prefetch Custom Textures" setting. Outside a session
  Orca's pack isn't loaded.
- A new label: add an entry to `LABELS` with the texture's Dolphin name (dump it with
  `-C Graphics.Settings.DumpTextures=True`, then delete the dump: it is the game's art), run the
  script, and the unit test `OrcaOnlineMenuTextures` checks the name, the size and the mip levels.
