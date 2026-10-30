#!/usr/bin/env python3
"""Regenerates the UI copies of the amiibo metadata database.

The canonical, human-editable database is `amiibo.json` at the repository
root:

    {
      "0000000000000002": ["Mario", "Super Smash Bros.", "Figure",
                           "Super Mario", "2014"],
      ...
    }

Key = 16-hex-character amiibo ID (head + tail). Value = [name, amiiboSeries,
type, gameSeries, releaseYear]. Edit that file, then run this script to
update the two generated copies:

  * device/web-sync/amiibo-db.js            (web sync page, <script> loaded)
  * android-app/app/src/main/assets/amiibo.json  (Android app asset)

The initial content was generated from the community AmiiboAPI database
(https://github.com/N3evin/AmiiboAPI). Amiibo data and box art are
Nintendo's; this project ships only the ID->name mapping and references
images from AmiiboAPI's CDN at runtime.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "amiibo.json")
TARGETS = [
    os.path.join(ROOT, "device", "web-sync", "amiibo-db.js"),
    os.path.join(ROOT, "android-app", "app", "src", "main", "assets", "amiibo.json"),
]

HEADER = """// OpenTag amiibo metadata database.
// GENERATED from amiibo.json by tools/gen_amiibo_db.py - edit that file and
// re-run the script instead of changing this one.
// Source data: community AmiiboAPI database (https://github.com/N3evin/AmiiboAPI).
// Key = 16-hex-character amiibo ID (head + tail). Value:
// [name, amiiboSeries, type, gameSeries, releaseYear]. Amiibo data and box
// art are Nintendo's; only the ID->name mapping ships here, images are
// referenced from AmiiboAPI's CDN at runtime.
window.AMIIBO_IMAGE_BASE = 'https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/images/';
window.AMIIBO_DB = {
"""


def esc(s):
    return s.replace("\\", "\\\\").replace("'", "\\'")


def main():
    if not os.path.isfile(SOURCE):
        sys.exit(f"error: {SOURCE} not found")
    with open(SOURCE) as f:
        db = json.load(f)
    for k, v in db.items():
        if not (len(k) == 16 and all(c in "0123456789abcdefABCDEF" for c in k)):
            sys.exit(f"error: bad amiibo id {k!r}")
        if not (isinstance(v, list) and len(v) == 5 and all(isinstance(x, str) for x in v)):
            sys.exit(f"error: entry {k} must be [name, series, type, game, year] strings")

    js = [HEADER]
    for k in sorted(db, key=str.lower):
        name, series, typ, game, year = db[k]
        js.append("'%s':['%s','%s','%s','%s','%s'],\n"
                  % (k.lower(), esc(name), esc(series), esc(typ), esc(game), esc(year)))
    js.append("};\n")
    with open(TARGETS[0], "w") as f:
        f.write("".join(js))
    with open(TARGETS[1], "w") as f:
        json.dump({k.lower(): v for k, v in db.items()}, f, ensure_ascii=False,
                  separators=(",", ":"))
    print(f"amiibo database: {len(db)} entries")
    for t in TARGETS:
        print(f"  wrote {os.path.relpath(t, ROOT)} ({os.path.getsize(t)} bytes)")


if __name__ == "__main__":
    main()
