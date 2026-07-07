#!/usr/bin/env python3
"""Generate tools/emoji-data.json from Unicode's emoji-test.txt.

Filters out skin-tone, hair-style, gendered-role, family/couple/holding-hands
and subdivision-flag variants, keeping one default ("yellow") glyph per
concept. Matches each surviving sequence against the real Twemoji 72x72
filename list (tools/twemoji-filenames.txt) so the asset downloader knows
exactly which file to fetch.

Usage:
    python tools/gen-emoji-data.py emoji-test.txt twemoji-filenames.txt emoji-data.json
"""
import json
import re
import sys
from collections import Counter

SKIN = set(range(0x1F3FB, 0x1F3FF + 1))
HAIR = {0x1F9B0, 0x1F9B1, 0x1F9B2, 0x1F9B3}
GENDER_SIGNS = {0x2642, 0x2640}
ZWJ = 0x200D
VS16 = 0xFE0F
TAG_RANGE = set(range(0xE0020, 0xE007F + 1))
DIRECTION = {0x27A1, 0x2B05}
MAN = 0x1F468
WOMAN = 0x1F469

LINE_RE = re.compile(r"^([0-9A-Fa-f ]+)\s*;\s*([a-z-]+)\s*#\s*(\S+)\s+E[\d.]+\s+(.*)$")


def hexname(cps):
    return "-".join(format(c, "x") for c in cps)


def parse_emoji_test(path):
    group = subgroup = None
    entries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# group:"):
                group = line.split(":", 1)[1].strip()
                continue
            if line.startswith("# subgroup:"):
                subgroup = line.split(":", 1)[1].strip()
                continue
            if not line or line.startswith("#"):
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            cps_str, status, glyph, name = m.groups()
            if status != "fully-qualified":
                continue
            cps = [int(c, 16) for c in cps_str.strip().split()]
            entries.append({"cps": cps, "glyph": glyph, "name": name, "group": group, "subgroup": subgroup})
    return entries


def keep(entry):
    cps, group, subgroup = entry["cps"], entry["group"], entry["subgroup"]
    if group == "Component":
        return False, "component-group"
    if any(c in SKIN for c in cps):
        return False, "skin-tone"
    if any(c in HAIR for c in cps):
        return False, "hair-style"
    if any(c in TAG_RANGE for c in cps):
        return False, "subdivision-flag-tag"
    has_zwj = ZWJ in cps
    if has_zwj and any(c in GENDER_SIGNS for c in cps):
        return False, "gendered-zwj"
    if has_zwj and any(c in DIRECTION for c in cps):
        return False, "direction-zwj"
    if subgroup == "family":
        return False, "family-kiss-couple-holdinghands-zwj"
    if has_zwj and any(c in (MAN, WOMAN) for c in cps):
        return False, "gendered-role-zwj"
    return True, None


def main():
    test_path, twemoji_path, out_path = sys.argv[1:4]

    entries = parse_emoji_test(test_path)
    with open(twemoji_path, encoding="utf-8") as f:
        twemoji_files = {line.strip() for line in f if line.strip()}

    dropped = Counter()
    kept = []
    unmatched = []
    seen_keys = set()

    for e in entries:
        ok, reason = keep(e)
        if not ok:
            dropped[reason] += 1
            continue

        cps = e["cps"]
        key_cps = [c for c in cps if c != VS16]
        key = hexname(key_cps)

        candidates = [hexname(cps) + ".png", hexname(key_cps) + ".png"]
        twfile = next((c for c in candidates if c in twemoji_files), None)
        if twfile is None:
            unmatched.append((key, e["name"]))
            continue

        if key in seen_keys:
            continue
        seen_keys.add(key)

        if len(key) > 31:
            unmatched.append((key, e["name"] + " (name too long)"))
            continue

        kept.append({
            "key": key,
            "glyph": e["glyph"],
            "name": e["name"],
            "group": e["group"],
            "twemoji_file": twfile,
        })

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(kept, f, ensure_ascii=False, indent=1)

    print(f"kept: {len(kept)}")
    print(f"unmatched (no twemoji file found): {len(unmatched)}")
    for key, name in unmatched[:40]:
        print(f"  MISS {key}  {name}")
    print("dropped breakdown:")
    for reason, count in dropped.most_common():
        print(f"  {reason}: {count}")


if __name__ == "__main__":
    main()
