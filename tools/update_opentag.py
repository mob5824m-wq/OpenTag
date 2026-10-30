#!/usr/bin/env python3
"""OpenTag firmware updater.

Grabs the latest firmware image for an OpenTag device and installs it over
USB serial WITHOUT touching the LittleFS partition that holds your amiibo
library (or the NVS settings). Two devices share the releases page:
the handheld (default) and the screenless OpenTag Mini (--mini).

How it works
------------
1. Finds the latest firmware release. In order:
   a. --bin <file>        : use this local firmware.bin
   b. --tag <git tag>     : download that release from GitHub
   c. latest release      : query the GitHub API for mob5824m-wq/OpenTag
      and download its `firmware.bin` asset
   d. local build         : if no release asset exists, run `pio run
      -e opentag_s3` in the repo's device/ directory (requires PlatformIO)
2. Reads the app partition offset from device/partitions_16mb.csv and flashes
   ONLY that partition with esptool, leaving nvs / otadata / littlefs alone.
3. Resets the board into the new firmware.

Usage
-----
    python tools/update_opentag.py                 # latest release, handheld
    python tools/update_opentag.py --mini          # latest release, Mini
    python tools/update_opentag.py --port COM5     # explicit serial port
    python tools/update_opentag.py --tag v1.2      # specific release
    python tools/update_opentag.py --bin firmware.bin --port /dev/ttyACM0

Requirements: `pip install esptool requests` (or use --list to only show what
would be flashed). Plug the OpenTag in via its USB port; if the board does
not enter the bootloader automatically, hold BOOT while plugging in.

This file is part of OpenTag (MIT license).
"""

import argparse
import json
import os
import subprocess
import sys
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVICE_DIR = os.path.join(REPO, "device")
PARTITIONS = os.path.join(DEVICE_DIR, "partitions_16mb.csv")
GITHUB_API = "https://api.github.com/repos/mob5824m-wq/OpenTag"
GITHUB_DL = "https://github.com/mob5824m-wq/OpenTag/releases/download"

# Partition names that must never be touched by an update.
PROTECTED = {"nvs", "otadata", "littlefs", "spiffs", "coredump", "coredump_0"}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def info(msg):
    print(f"      {msg}")


def app_partition_offset():
    """Parse partitions_16mb.csv and return (name, offset, size) of the app."""
    if not os.path.isfile(PARTITIONS):
        die(f"partition table not found: {PARTITIONS}")
    with open(PARTITIONS) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            fields = [x.strip() for x in line.split(",")]
            if len(fields) < 5:
                continue
            name, ptype, subtype, offset = fields[0], fields[1], fields[2], fields[3]
            if ptype == "app":
                return name, int(offset, 0), int(fields[4], 0)
    die("no app partition found in the partition table")


def fetch_url(url, dest, token=None):
    """Download url -> dest with a progress line (stdlib urllib only)."""
    req = urllib.request.Request(url, headers={"User-Agent": "opentag-updater"})
    if token:
        req.add_header("Authorization", f"token {token}")
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
            total = int(r.headers.get("content-length", 0))
            done = 0
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if total:
                    print(f"\r      {os.path.basename(dest)}: "
                          f"{100 * done // total}% ({done // 1024} KiB)", end="")
        print()
    except urllib.error.HTTPError as e:
        die(f"download failed ({e.code}): {url}")
    except urllib.error.URLError as e:
        die(f"download failed: {e.reason}")
    return dest


def latest_release(token=None, tag=None, mini=False):
    """Return (tag, asset_url) for the newest (or given) release with a binary.

    Releases carry two firmwares (handheld + Mini); --mini picks the Mini's
    image. Older releases with a single generic firmware.bin still work.
    """
    hdrs = {"Accept": "application/vnd.github+json",
            "User-Agent": "opentag-updater"}
    if token:
        hdrs["Authorization"] = f"token {token}"
    if tag:
        url = f"{GITHUB_API}/releases/tags/{tag}"
    else:
        url = f"{GITHUB_API}/releases/latest"
    req = urllib.request.Request(url, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            rel = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None, None
        die(f"GitHub API error ({e.code}) for {url}")
    except urllib.error.URLError as e:
        die(f"GitHub API unreachable: {e.reason}")
    wanted = "opentag-mini-firmware-" if mini else "opentag-firmware-"
    fallback = None
    for asset in rel.get("assets", []):
        name = asset.get("name", "")
        if name.endswith(".bin") and "firmware" in name.lower():
            if name.startswith(wanted):
                return rel.get("tag_name", "?"), asset["browser_download_url"]
            if fallback is None:
                fallback = asset["browser_download_url"]
    if fallback is not None:
        return rel.get("tag_name", "?"), fallback
    # Some setups zip the firmware; accept a .zip asset too.
    for asset in rel.get("assets", []):
        if asset.get("name", "").endswith(".zip"):
            return rel.get("tag_name", "?"), asset["browser_download_url"]
    return rel.get("tag_name", "?"), None


def build_locally(mini=False):
    """Fall back to building with PlatformIO if no release asset exists."""
    print("  no release asset found - trying a local PlatformIO build")
    try:
        subprocess.run(["pio", "--version"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    env = "opentag_mini" if mini else "opentag_s3"
    r = subprocess.run(["pio", "run", "-e", env], cwd=DEVICE_DIR)
    if r.returncode != 0:
        return None
    fw = os.path.join(DEVICE_DIR, ".pio", "build", env, "firmware.bin")
    return fw if os.path.isfile(fw) else None


def extract_bin(path):
    """If path is a zip, extract firmware.bin next to it and return its path."""
    if not zipfile.is_zipfile(path):
        return path
    print("  release asset is a zip - extracting firmware.bin")
    out = os.path.join(os.path.dirname(path) or ".", "firmware.bin")
    with zipfile.ZipFile(path) as z:
        cand = [n for n in z.namelist()
                if n.endswith(".bin") and "firmware" in n.lower()]
        if not cand:
            die("zip contains no firmware*.bin")
        with z.open(cand[0]) as src, open(out, "wb") as dst:
            dst.write(src.read())
    return out


def find_esptool():
    """Prefer the esptool python module; fall back to the esptool command."""
    try:
        import esptool  # noqa: F401
        return ["python", "-m", "esptool"]
    except ImportError:
        pass
    for exe in ("esptool", "esptool.py", "esptool.exe"):
        try:
            subprocess.run([exe, "version"], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return [exe]
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    die("esptool not found: pip install esptool")


def pick_port(hint):
    if hint:
        return hint
    try:
        import serial.tools.list_ports
        ports = list(serial.tools.list_ports.comports())
    except ImportError:
        return None
    cands = []
    for p in ports:
        # ESP32-S3 native USB shows as a USB JTAG/serial debug unit; classic
        # boards use CP210x / CH34x bridges.
        desc = (p.description or "").lower()
        if any(k in desc for k in ("jtag", "cp210", "ch34", "ch910", "usb serial", "usb-serial")):
            cands.append(p.device)
    if len(cands) == 1:
        info(f"auto-detected serial port: {cands[0]}")
        return cands[0]
    if len(cands) > 1:
        print("  multiple candidate serial ports found:")
        for i, c in enumerate(cands):
            print(f"    [{i}] {c}")
        sel = input("  choose one (number): ").strip()
        try:
            return cands[int(sel)]
        except (ValueError, IndexError):
            return None
    return None


def flash(port, binpath, offset, baud):
    cmd = find_esptool() + [
        "--chip", "esp32s3", "--port", port, "--baud", str(baud),
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", hex(offset), binpath,
    ]
    print("  running:", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        die("esptool failed - hold BOOT while replugging the board and retry")


def main():
    ap = argparse.ArgumentParser(description="Update OpenTag firmware, keep amiibos.")
    ap.add_argument("--port", help="serial port of the OpenTag (auto-detected if omitted)")
    ap.add_argument("--bin", help="use a local firmware.bin instead of downloading")
    ap.add_argument("--tag", help="download a specific GitHub release tag")
    ap.add_argument("--baud", type=int, default=921600, help="flash baud rate (default 921600)")
    ap.add_argument("--list", action="store_true", help="only show what would be flashed")
    ap.add_argument("--mini", action="store_true",
                    help="update the screenless OpenTag Mini instead of the handheld")
    args = ap.parse_args()

    token = os.environ.get("GITHUB_TOKEN")  # optional, for private repos/rate limits

    name, offset, size = app_partition_offset()
    print(f"OpenTag updater")
    print(f"  app partition : {name} @ {offset:#x} ({size // 1024} KiB)")
    print(f"  protected     : {', '.join(sorted(PROTECTED))} (amiibo library lives there)")

    # 1. obtain firmware
    if args.bin:
        if not os.path.isfile(args.bin):
            die(f"file not found: {args.bin}")
        binpath = os.path.abspath(args.bin)
        print(f"  firmware      : local {binpath}")
    else:
        tag, url = latest_release(token=token, tag=args.tag, mini=args.mini)
        if url:
            print(f"  firmware      : GitHub release {tag}")
            dl = url if url.startswith("http") else f"{GITHUB_DL}/{tag}/{url}"
            dest = os.path.join(os.path.dirname(REPO) or ".", f"opentag-firmware-{tag}.bin")
            if url.endswith(".zip"):
                dest = dest[:-4] + ".zip"
            fetch_url(dl, dest, token=token)
            binpath = extract_bin(dest)
        else:
            built = build_locally(mini=args.mini)
            if not built:
                die("no release asset and no local PlatformIO build. "
                    "Push a release (CI attaches firmware.bin), install pio, "
                    "or pass --bin.")
            binpath = built
            print(f"  firmware      : local build {binpath}")

    if not os.path.isfile(binpath):
        die(f"firmware image not found: {binpath}")
    fwsize = os.path.getsize(binpath)
    print(f"  image         : {binpath} ({fwsize // 1024} KiB)")
    if fwsize > size:
        die(f"image ({fwsize} B) is larger than the app partition ({size} B)")

    if args.list:
        print("  --list: not flashing.")
        return

    # 2. find the board
    port = pick_port(args.port)
    if not port:
        die("no serial port found/selected - pass --port (e.g. --port COM5 "
            "or --port /dev/ttyACM0)")

    # 3. flash ONLY the app partition
    print("  flashing app partition only (amiibo library untouched)...")
    flash(port, binpath, offset, args.baud)
    print("  done. OpenTag reset into the new firmware; your amiibo library,")
    print("  NDEF note and settings are preserved.")


if __name__ == "__main__":
    main()
