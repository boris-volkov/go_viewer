#!/usr/bin/env python3
"""
Rename SGF files to a standard format:
    BlackPlayer_vs_WhitePlayer_YYYY.sgf

Dry-run by default — prints what would change without touching anything.
Pass --apply to actually rename the files.

Usage:
    python rename_games.py [games_dir] [--apply]

games_dir defaults to 'games' relative to the current directory.
"""

import io
import os
import re
import sys


def _natural_key(s):
    """Sort key that orders _2, _3, ..., _9, _10, _11 numerically."""
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]

# Ensure stdout handles non-ASCII player names on Windows
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def sanitize(name: str) -> str:
    """Turn a player name into a safe filename component."""
    name = name.strip()
    name = re.sub(r"[^\w]", "_", name)   # non-alphanumeric/underscore → _
    name = re.sub(r"_+", "_", name)       # collapse runs
    return name.strip("_")


def extract_field(content: str, tag: str) -> str:
    m = re.search(re.escape(tag) + r"\[([^\]]*)\]", content)
    return m.group(1).strip() if m else ""


MAX_NAME_LEN = 80  # max chars for each player name component

def build_name(black: str, white: str, date: str) -> str:
    b = sanitize(black) or "Black"
    w = sanitize(white) or "White"
    # Truncate very long names (e.g. team/relay games with multiple players listed)
    b = b[:MAX_NAME_LEN].rstrip("_")
    w = w[:MAX_NAME_LEN].rstrip("_")
    # Use full date if available (YYYY-MM-DD), else just year — avoids same-year collisions
    d = date.strip()
    if len(d) >= 10 and d[:4].isdigit() and d[4] in "-/" and d[5:7].isdigit():
        date_part = d[:10].replace("/", "-")  # normalise separator
    elif len(d) >= 4 and d[:4].isdigit():
        date_part = d[:4]
    else:
        date_part = ""
    return f"{b}_vs_{w}_{date_part}.sgf" if date_part else f"{b}_vs_{w}.sgf"


def find_sgf_files(root: str):
    for dirpath, _, filenames in os.walk(root):
        for fn in sorted(filenames, key=_natural_key):
            if fn.lower().endswith(".sgf"):
                yield os.path.join(dirpath, fn)


def plan_renames(games_dir: str):
    used: dict[str, set] = {}   # dirpath -> set of lowercase names already claimed
    renames = []

    for path in find_sgf_files(games_dir):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
        except OSError as e:
            print(f"  WARNING: cannot read {path}: {e}", file=sys.stderr)
            continue

        black = extract_field(content, "PB") or extract_field(content, "pb")
        white = extract_field(content, "PW") or extract_field(content, "pw")
        date  = extract_field(content, "DT") or extract_field(content, "dt")

        target = build_name(black, white, date)
        dirpath = os.path.dirname(path)
        claimed = used.setdefault(dirpath, set())

        # Resolve collisions: append _2, _3, …
        base, ext = os.path.splitext(target)
        candidate = target
        counter = 2
        while candidate.lower() in claimed:
            candidate = f"{base}_{counter}{ext}"
            counter += 1
        claimed.add(candidate.lower())

        new_path = os.path.join(dirpath, candidate)
        if os.path.normcase(path) != os.path.normcase(new_path):
            renames.append((path, new_path))

    return renames


def main():
    apply   = "--apply" in sys.argv
    args    = [a for a in sys.argv[1:] if not a.startswith("--")]
    games_dir = args[0] if args else "games"

    if not os.path.isdir(games_dir):
        sys.exit(f"Directory not found: {games_dir}")

    renames = plan_renames(games_dir)

    if not renames:
        print("All files are already named correctly — nothing to do.")
        return

    for old, new in renames:
        print(f"  {os.path.relpath(old, games_dir)}")
        print(f"  -> {os.path.relpath(new, games_dir)}")
        print()

    total = len(renames)
    if apply:
        done, skipped = 0, 0
        for old, new in renames:
            if not os.path.exists(old):
                skipped += 1
                continue
            if os.path.exists(new):
                skipped += 1
                continue
            os.rename(old, new)
            done += 1
        print(f"Renamed {done} file{'s' if done != 1 else ''}." +
              (f" ({skipped} already moved, skipped.)" if skipped else ""))
    else:
        print(f"Would rename {total} file{'s' if total != 1 else ''}.")
        print("Run with --apply to perform the renames.")


if __name__ == "__main__":
    main()
