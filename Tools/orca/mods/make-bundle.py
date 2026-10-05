#!/usr/bin/env python3
"""Turn one of Orca's legacy profiles into a mod bundle (YouGame docs/ORCA_MODS.md, Phase 1).

  make-bundle.py RSBE01 <out>
  make-bundle.py PPLUS32 <out> --pplus-user <Project+ release's user/ folder>

Reads Data/Sys/Orca/<profile>.ini, .patches, .jitwarm and Textures/RSBE01/ from this checkout (or
--sys), and for Project+ the official release's files: Launcher/Project+ Netplay Launcher.dol and the
files inside Wii/sd.raw (unpacked with 7-Zip, --7z), plus its license.txt. Writes <out>/ as the app
lays out a run folder: mod.json, launcher.dol, sd/..., orca/patches/<profile>.patches,
orca/textures/..., orca/jitwarm, and tree.json with the tree hash. <out> must not exist yet.

None of Project+'s files belong in this repository: write P+ bundles outside it.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MAX_PATH_BYTES = 160
DEVICES = {"con", "prn", "aux", "nul"}
# What each legacy profile becomes.
BUILTIN = {"RSBE01": "brawl-rev2", "PPLUS32": "brawl-rev2-pplus"}
RULESETS = {"RSBE01": ["brawl"], "PPLUS32": ["pplus-2024"]}
OFFERS = ["results", "locks", "queue2"]
# Project+'s official SD image: the card size the bundle pins.
PPLUS_SD_BYTES = 2_097_152_000
PPLUS_LAUNCHER = "Project+ Netplay Launcher.dol"
# sha256 of Project+ v3.2's Netplay Launcher (PPLUS32.ini names its XXH3-128).
PPLUS_LAUNCHER_SHA256_PREFIX = "74a0decf"


def path_problem(path):
    """Mod::PathProblem (Source/Core/Core/Orca/Mod.cpp)."""
    if not path:
        return "an empty path"
    if len(path.encode()) > MAX_PATH_BYTES:
        return f"longer than {MAX_PATH_BYTES} bytes"
    if any(ord(c) < 0x20 or ord(c) > 0x7E or c in '<>:"\\|?*' for c in path):
        return 'a character outside printable ASCII, or one of <>:"\\|?*'
    for part in path.split("/"):
        if not part:
            return "an empty component"
        if part[0] == ".":
            return "a component that starts with a dot"
        if part[-1] in ". ":
            return "a component that ends in a dot or a space"
        base = part.split(".")[0].lower().rstrip(" ")
        if base in DEVICES or (len(base) == 4 and base[:3] in ("com", "lpt") and base[3].isdigit()):
            return "a Windows device name"
    return None


def host_only(path):
    return path == "orca/jitwarm" or path.startswith("orca/test/")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def tree_hash(files):
    text = "".join(f"{f['path']}\t{f['sha256']}\t{f['bytes']}\n" for f in files if not host_only(f["path"]))
    return hashlib.sha256(text.encode()).hexdigest()


def read_profile(path):
    """Data/Sys/Orca/<profile>.ini: {"Profile": [(key, value)], "Forced": [(key, value)]}."""
    sections = {"Profile": [], "Forced": []}
    section = None
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\r\n")
        at = line.find("#")
        if at == 0 or (at > 0 and line[at - 1] in " \t"):
            line = line[:at]
        line = line.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section not in sections:
                sys.exit(f"{path}: unknown section [{section}]")
            continue
        key, eq, value = line.partition("=")
        if not eq or section is None:
            sys.exit(f"{path}: bad line {raw!r}")
        sections[section].append((key.strip(), value.strip()))
    return sections


def hex_word(value):
    return f"0x{int(value, 0):08X}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", choices=sorted(BUILTIN))
    ap.add_argument("out")
    ap.add_argument("--sys", default=os.path.join(REPO, "Data", "Sys"), help="the Sys folder (default: this checkout's)")
    ap.add_argument("--pplus-user", help="Project+ release's user/ folder (Launcher/, Wii/sd.raw)")
    ap.add_argument("--7z", dest="sevenzip", default=shutil.which("7zz") or "/opt/homebrew/bin/7zz")
    ap.add_argument("--min-orca", default="0.4.0")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    if os.path.exists(out):
        sys.exit(f"{out} exists; give a new folder")
    if os.path.commonpath([out, REPO]) == REPO and args.profile == "PPLUS32":
        sys.exit("Project+'s files are not ours to commit: write its bundle outside the repository")
    orca = os.path.join(args.sys, "Orca")
    ini = read_profile(os.path.join(orca, args.profile + ".ini"))
    p = dict()
    kept = []
    for key, value in ini["Profile"]:
        if key == "KeepGameCode":
            kept.append([hex_word(w) for w in value.split()])
        else:
            p[key] = value

    launcher = "Launcher" in p
    mod = {
        "format": 1,
        "engine": {"api": 1, "minOrca": args.min_orca},
    }
    if "Title" in p:
        mod["title"] = p["Title"]
    mod["disc"] = {"id": p.get("Disc", args.profile), "revision": int(p["Revision"], 0)}
    if launcher:
        mod["launcher"] = "launcher.dol"
    if "SDImage" in p:
        mod["sd"] = {"bytes": PPLUS_SD_BYTES}
    mod["frameHook"] = {"address": hex_word(p["FrameHook"]), "word": hex_word(p["FrameHookWord"])}
    if "BootNandFrame" in p:
        mod["bootNandFrame"] = int(p["BootNandFrame"], 0)
    mod["firstKeyframeFrame"] = 300
    if "JitClearFrame" in p:
        mod["jitClearFrame"] = int(p["JitClearFrame"], 0)
    if kept:
        mod["keepGameCode"] = kept
    if "SaveTitle" in p:
        mod["saveTitle"] = f"0x{int(p['SaveTitle'], 16):016X}"
    forced = {}
    for key, value in ini["Forced"]:
        if key == "Dolphin.Core.WiiSDCard":
            # In a mod the card follows "sd".
            if value != "True" or "sd" not in mod:
                sys.exit(f"{args.profile}: WiiSDCard={value} without an SD image")
            continue
        if key != "Dolphin.Core.CustomRTCValue":
            sys.exit(f"{args.profile}: {key} is not on the engine's allowlist")
        forced[key] = int(value, 0)
    if forced:
        mod["forced"] = forced
    mod["builtin"] = BUILTIN[args.profile]
    mod["rulesets"] = RULESETS[args.profile]
    mod["offers"] = OFFERS
    unknown = set(p) - {"Title", "Disc", "Revision", "Launcher", "SDImage", "SDImageHash", "SaveTitle", "FrameHook",
                        "FrameHookWord", "BootNandFrame", "JitClearFrame"}
    if unknown:
        sys.exit(f"{args.profile}: keys this tool doesn't map: {sorted(unknown)}")

    tmp = tempfile.mkdtemp(prefix="orca-bundle-", dir=os.path.dirname(out))
    try:
        def put(rel, src=None, data=None, move=False):
            dst = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if data is not None:
                with open(dst, "wb") as f:
                    f.write(data)
            elif move:
                os.rename(src, dst)
            else:
                shutil.copyfile(src, dst)

        put(f"orca/patches/{args.profile.lower()}.patches", os.path.join(orca, args.profile + ".patches"))
        jitwarm = os.path.join(orca, args.profile + ".jitwarm")
        if os.path.exists(jitwarm):
            put("orca/jitwarm", jitwarm)
        # Orca's texture pack is for the disc's game (RSBE01), which Project+ runs too.
        textures = os.path.join(orca, "Textures", mod["disc"]["id"])
        if os.path.isdir(textures):
            for name in sorted(os.listdir(textures)):
                if name.lower().endswith(".png"):
                    put(f"orca/textures/{name}", os.path.join(textures, name))

        if launcher:
            if not args.pplus_user:
                sys.exit("Project+: give --pplus-user <release's user/ folder>")
            dol = os.path.join(args.pplus_user, "Launcher", PPLUS_LAUNCHER)
            if not sha256_file(dol).startswith(PPLUS_LAUNCHER_SHA256_PREFIX):
                sys.exit(f"{dol} is not Project+ v3.2's Netplay Launcher")
            put("launcher.dol", dol)
            lic = os.path.join(os.path.dirname(os.path.abspath(args.pplus_user)), "license.txt")
            if os.path.exists(lic):
                put("license.txt", lic)
        if "sd" in mod:
            image = os.path.join(args.pplus_user, "Wii", p["SDImage"])
            if os.path.getsize(image) != PPLUS_SD_BYTES:
                sys.exit(f"{image} is not {PPLUS_SD_BYTES} bytes")
            unpacked = os.path.join(tmp, ".sd-unpack")
            subprocess.run([args.sevenzip, "x", "-y", "-bso0", "-bsp0", f"-o{unpacked}", image], check=True)
            for root, dirs, names in os.walk(unpacked):
                dirs.sort()
                for name in sorted(names):
                    src = os.path.join(root, name)
                    rel = os.path.relpath(src, unpacked).replace(os.sep, "/")
                    put("sd/" + rel, src, move=True)
            shutil.rmtree(unpacked)

        put("mod.json", data=(json.dumps(mod, indent=2) + "\n").encode())

        files = []
        for root, dirs, names in os.walk(tmp):
            for name in names:
                full = os.path.join(root, name)
                rel = os.path.relpath(full, tmp).replace(os.sep, "/")
                problem = path_problem(rel)
                if problem:
                    sys.exit(f"{rel}: {problem}")
                files.append({"path": rel, "sha256": sha256_file(full), "bytes": os.path.getsize(full)})
        files.sort(key=lambda f: f["path"].encode())
        lower = [f["path"].lower() for f in files]
        if len(set(lower)) != len(lower):
            sys.exit("two paths differ only in case")
        tree = {"tree": tree_hash(files), "files": files}
        with open(os.path.join(tmp, "tree.json"), "w") as f:
            json.dump(tree, f, indent=1)
            f.write("\n")
        os.rename(tmp, out)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    sd = sum(1 for f in files if f["path"].startswith("sd/"))
    print(f"{out}: {len(files)} files ({sd} on the SD card), tree {tree['tree']}")


if __name__ == "__main__":
    main()
