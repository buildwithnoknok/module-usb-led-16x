#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) noknok
"""
mk_full_image.py -- the FULL flash image of a noknok module (DEV-197).

A module repo ships two images in firmware/bin/:
  * the APP image (what firmware/index.json points at) = the OTA payload the brain
    downloads. It starts at the app base, NOT at the flash start.
  * the FULL image (this script) = bootloader + app + app-valid metadata, covering
    the whole flash from 0x08000000. Use it for a first flash or a recovery with a
    WCH-LinkE (SWD) or WCHISPTool. The brain never downloads it.
Flashing the APP image at 0x08000000 wipes the bootloader: the module looks dead.

This is the same file in every module repo. Run it from the repo root:

  python tools/mk_full_image.py            # build firmware/bin/<module>_full.bin + firmware/full.json
  python tools/mk_full_image.py --check    # release check: full image == the app in index.json

Build mode takes RELEASED parts only and stops otherwise:
  * the app must match firmware/index.json (size + CRC) and be committed unchanged;
  * the bootloader parts come from the sibling clones (default ../module-I2C-bootloader
    and ../module-USB-bootloader, override with --bootloader-repo) and must be committed
    unchanged there; I2C stage-1 must also match that repo's firmware/index.json.
Check mode needs nothing but this repo (CI runs it): it re-reads firmware/full.json and
fails if the full image, its app bytes or its metadata no longer match index.json.

Layouts (must match module-I2C-bootloader port_<chip>.h + mk_full.py, and
module-USB-bootloader noknok_usb_bootloader.c):
  ch32v003  layout 2  16 KB  stage-0 @0x0000, stage-1 @0x0400, app @0x1400, meta @0x3FC0
  ch32x035  layout 3  62 KB  stage-0 @0x0000, stage-1 @0x0400, app @0x1400, meta @0xF500
  ch32v203  (USB)     32 KB  USB bootloader @0x0000, app @0x2000, meta @0x7F00
No dependencies beyond the Python 3 standard library.
"""
import json
import os
import struct
import subprocess
import sys
import zlib

INDEX = "firmware/index.json"
FULL_JSON = "firmware/full.json"

CHIPS = {
    # chip: flash size, app base, app end (first byte the app may not use), meta offset, meta magic,
    #       bootloader parts [(name, repo, file, offset, max size, version source)]
    "ch32v003": dict(flash=0x4000, app=0x1400, app_end=0x3F80, meta=0x3FC0, magic=0xB007C0DE,
                     parts=[("stage0", "module-I2C-bootloader", "firmware/bin/noknok_stage0.bin", 0x0000, 0x0400, None),
                            ("stage1", "module-I2C-bootloader", "firmware/bin/noknok_stage1.bin", 0x0400, 0x1000, "index")]),
    "ch32x035": dict(flash=0xF800, app=0x1400, app_end=0xF400, meta=0xF500, magic=0xB007C0DE,
                     parts=[("stage0", "module-I2C-bootloader", "firmware/bin/noknok_stage0_x035.bin", 0x0000, 0x0400, None),
                            ("stage1", "module-I2C-bootloader", "firmware/bin/noknok_stage1_x035.bin", 0x0400, 0x1000, None)]),
    "ch32v203": dict(flash=0x8000, app=0x2000, app_end=0x7F00, meta=0x7F00, magic=0x6E6B5542,
                     parts=[("usb_bootloader", "module-USB-bootloader", "firmware/bin/noknok_usb_bootloader.bin", 0x0000, 0x2000, "bl_source")]),
}


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def crc(data):
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def app_path(idx):
    # url = https://raw.githubusercontent.com/buildwithnoknok/<repo>/main/<path>
    return idx["url"].split("/main/", 1)[1]


def chip_for(idx):
    layout = idx.get("layout")
    if layout == 2:
        return "ch32v003"
    if layout == 3:
        return "ch32x035"
    if layout is None:
        return "ch32v203"       # USB modules: no I2C layout field (see check_index.py)
    fail("no full-image layout known for index layout %r" % layout)


def git_clean_sha(repo_dir, rel):
    """The last commit that touched rel; fail if rel has local changes or is untracked."""
    def git(*a):
        return subprocess.run(["git", "-C", repo_dir] + list(a), capture_output=True, text=True)
    st = git("status", "--porcelain", "--", rel)
    if st.returncode != 0:
        fail("%s is not a git clone (%s)" % (repo_dir, st.stderr.strip()))
    if st.stdout.strip():
        fail("%s/%s has uncommitted changes - only released (committed) parts go into a full image"
             % (repo_dir, rel))
    sha = git("log", "-1", "--format=%h", "--", rel).stdout.strip()
    if not sha:
        fail("%s/%s is not committed" % (repo_dir, rel))
    return sha


def usb_bl_version(repo_dir):
    import re
    src = os.path.join(repo_dir, "firmware/src/noknok_usb_bootloader.c")
    text = open(src, "r", encoding="utf-8", errors="replace").read()
    v = [re.search(r"#define\s+BL_VERSION_%s\s+(\d+)" % p, text) for p in ("MAJOR", "MINOR", "PATCH")]
    if not all(v):
        fail("BL_VERSION_* not found in %s" % src)
    return ".".join(m.group(1) for m in v)


def build(args):
    idx = load_json(INDEX)
    chip = chip_for(idx)
    L = CHIPS[chip]
    here = os.getcwd()
    repos_dir = os.path.dirname(here)
    overrides = {}
    for a in args:
        if a.startswith("--bootloader-repo="):
            p = os.path.abspath(a.split("=", 1)[1])
            overrides[os.path.basename(p.rstrip("/\\"))] = p

    # --- the app: exactly what index.json describes, committed ---------------
    ap = app_path(idx)
    app = open(ap, "rb").read()
    if len(app) != idx["size"] or crc(app) != idx["crc32"]:
        fail("%s does not match %s (size %d/%d, crc %s/%s) - run check_index.py first"
             % (ap, INDEX, len(app), idx["size"], crc(app), idx["crc32"]))
    git_clean_sha(here, ap)
    git_clean_sha(here, INDEX)
    if len(app) > L["app_end"] - L["app"]:
        fail("app (%d B) does not fit between 0x%04X and 0x%04X" % (len(app), L["app"], L["app_end"]))

    img = bytearray(b"\xff" * L["flash"])
    parts = []
    for name, repo, rel, off, maxlen, vsrc in L["parts"]:
        rdir = overrides.get(repo, os.path.join(repos_dir, repo))
        path = os.path.join(rdir, rel)
        if not os.path.exists(path):
            fail("bootloader part %s not found at %s (clone %s next to this repo or pass --bootloader-repo=)"
                 % (name, path, repo))
        data = open(path, "rb").read()
        if len(data) > maxlen:
            fail("%s is %d B, more than its %d B slot" % (path, len(data), maxlen))
        sha = git_clean_sha(rdir, rel)
        rec = {"part": name, "source": "%s/%s" % (repo, rel), "commit": sha,
               "offset": "0x%04X" % off, "size": len(data), "crc32": crc(data)}
        if vsrc == "index":
            bidx = load_json(os.path.join(rdir, "firmware/index.json"))
            if bidx.get("size") != len(data) or bidx.get("crc32") != crc(data):
                fail("%s does not match %s/firmware/index.json - not the released stage-1" % (path, repo))
            rec["version"] = bidx["version"]
        elif vsrc == "bl_source":
            rec["version"] = usb_bl_version(rdir)
        img[off:off + len(data)] = data
        parts.append(rec)

    img[L["app"]:L["app"] + len(app)] = app
    img[L["meta"]:L["meta"] + 12] = struct.pack("<III", L["magic"], len(app), zlib.crc32(app) & 0xFFFFFFFF)

    full = load_json(FULL_JSON) if os.path.exists(FULL_JSON) else {}
    out = full.get("file") or "firmware/bin/%s_full.bin" % idx["module"]
    with open(out, "wb") as f:
        f.write(img)
    rec = {
        "format": 1,
        "note": "FULL image for a first flash or recovery over SWD (WCH-LinkE) / WCHISPTool: write it at "
                "0x08000000. It is not an OTA payload - firmware/index.json points at the app image.",
        "module": idx["module"],
        "chip": chip,
        "file": out,
        "flash_at": "0x08000000",
        "size": len(img),
        "crc32": crc(img),
        "app": {"source": ap, "version": idx["version"], "offset": "0x%04X" % L["app"],
                "size": len(app), "crc32": idx["crc32"]},
        "bootloader": parts,
        "metadata_offset": "0x%04X" % L["meta"],
    }
    if "layout" in idx:
        rec["layout"] = idx["layout"]
    with open(FULL_JSON, "w", encoding="utf-8", newline="\n") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")
    print("built %s: %s app v%s + %s -> %d B crc32 %s"
          % (out, chip, idx["version"],
             ", ".join("%s%s" % (p["part"], " v" + p["version"] if "version" in p else "") for p in parts),
             len(img), rec["crc32"]))


def check():
    if not os.path.exists(FULL_JSON):
        fail("%s missing - every module repo ships a full image (DEV-197); run tools/mk_full_image.py" % FULL_JSON)
    idx = load_json(INDEX)
    full = load_json(FULL_JSON)
    chip = chip_for(idx)
    L = CHIPS[chip]
    if full.get("chip") != chip:
        fail("%s says chip %s but index.json means %s" % (FULL_JSON, full.get("chip"), chip))
    if not os.path.exists(full["file"]):
        fail("%s names %s but the file is missing" % (FULL_JSON, full["file"]))
    img = open(full["file"], "rb").read()
    if len(img) != full["size"] or crc(img) != full["crc32"]:
        fail("%s is %d B crc32 %s, %s says %d B crc32 %s"
             % (full["file"], len(img), crc(img), FULL_JSON, full["size"], full["crc32"]))
    if len(img) != L["flash"]:
        fail("%s is %d B but a %s full image is %d B" % (full["file"], len(img), chip, L["flash"]))
    app = open(app_path(idx), "rb").read()
    a = full["app"]
    if a["version"] != idx["version"] or a["crc32"] != idx["crc32"] or a["size"] != idx["size"]:
        fail("full image holds app v%s crc32 %s but index.json is v%s crc32 %s - rebuild it with "
             "tools/mk_full_image.py" % (a["version"], a["crc32"], idx["version"], idx["crc32"]))
    if img[L["app"]:L["app"] + len(app)] != app:
        fail("the app bytes at 0x%04X in %s differ from %s" % (L["app"], full["file"], app_path(idx)))
    if any(b != 0xFF for b in img[L["app"] + len(app):L["meta"]]):
        fail("%s has stray bytes between the app end and the metadata" % full["file"])
    meta = struct.unpack("<III", img[L["meta"]:L["meta"] + 12])
    if meta != (L["magic"], len(app), zlib.crc32(app) & 0xFFFFFFFF):
        fail("metadata at 0x%04X is %s, expected magic 0x%08X len %d crc 0x%s"
             % (L["meta"], ["0x%08X" % m for m in meta], L["magic"], len(app), idx["crc32"]))
    for p in full["bootloader"]:
        off = int(p["offset"], 16)
        if crc(img[off:off + p["size"]]) != p["crc32"]:
            fail("bootloader part %s at %s does not match its recorded crc32" % (p["part"], p["offset"]))
    print("OK: %s full image %s  %d B crc32 %s  app v%s  %s"
          % (chip, full["file"], len(img), full["crc32"], idx["version"],
             ", ".join("%s %s" % (p["part"], p.get("version", p["commit"])) for p in full["bootloader"])))


if __name__ == "__main__":
    if not os.path.exists(INDEX):
        fail("%s not found (run from the repo root)" % INDEX)
    if "--check" in sys.argv[1:]:
        check()
    else:
        build(sys.argv[1:])
