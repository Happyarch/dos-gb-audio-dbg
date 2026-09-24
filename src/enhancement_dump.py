#!/usr/bin/env python3
"""enhancement_dump.py -- machine-readable dump of a resolved enhancement.

Used by the pkmn-audio-dbg viewer (EnhancementManager::compileEnhancement):
lints dos_port/tools/audio/enhancements/<Song>.yaml via yaml_lint and prints
the resolved frame-domain notes, one per line, so C++ never parses YAML.

Usage:
    enhancement_dump.py <repo_root> <SongLabel> [--target mt32|gm|opl3]

Output (stdout, parse-friendly):
    OK <song> <nchannels> <nnotes>
    CH <idx> <name> <tier> <midi_ch> <program> <volume>
    NOTE <frame> <dur> <midi_ch> <key> <vel>
    END

MIDI channel assignment mirrors gb_to_midi.enhancement_tracks: melodic
channels take the free parts [4,5,6,7,8] in tier-sorted order (extras
dropped), rhythm channels go to 9. Programs are 0-based for --target
(mt32 default; custom timbres fall back to gm like the merge does).
The opl3 target emits tier-1 channels only: the OPL3 device (like the
in-game OPL layer built by gen_enh_streams.py) plays the tier-1
foundation and nothing above it.

Exit status: 0 on success (even with lint warnings, printed to stderr),
1 when the song has no enhancement or lint reports errors.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
AUDIO_DIR = HERE.parents[3] / "tools" / "audio"
sys.path.insert(0, str(AUDIO_DIR))

from gb_to_midi import FREE_MELODIC_CH  # noqa: E402
from mt32_presets import resolve_program  # noqa: E402
from yaml_lint import lint  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("repo_root", type=Path)
    ap.add_argument("song")
    ap.add_argument("--target", choices=("mt32", "gm", "opl3"), default="mt32")
    args = ap.parse_args()

    path = args.repo_root / "dos_port" / "tools" / "audio" / "enhancements" \
        / f"{args.song}.yaml"
    if not path.exists():
        print(f"ERROR: no enhancement file: {path}", file=sys.stderr)
        return 1
    rep, resolved, _ = lint(path)
    for w in rep.warnings:
        print(f"warn: {w}", file=sys.stderr)
    if rep.errors:
        for e in rep.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    chans = sorted((c for c in resolved if c.notes), key=lambda c: c.tier)
    if args.target == "opl3":
        # Tier-1 foundation only (mirrors gen_enh_streams.py): the OPL3
        # device never plays tier 2/3, by design.
        chans = [c for c in chans if c.tier == 1]
    melodic = [c for c in chans if not c.is_rhythm]
    dropped = set(id(c) for c in melodic[len(FREE_MELODIC_CH):])

    assign: dict[int, int] = {}
    mel_idx = 0
    for c in chans:
        if id(c) in dropped:
            continue
        if c.is_rhythm:
            assign[id(c)] = 9
        else:
            assign[id(c)] = FREE_MELODIC_CH[mel_idx]
            mel_idx += 1

    kept = [c for c in chans if id(c) in assign]
    total_notes = sum(len(c.notes) for c in kept)
    print(f"OK {args.song} {len(kept)} {total_notes}")
    idx = 0
    for c in kept:
        mc = assign[id(c)]
        if c.is_rhythm:
            prog = -1
        elif args.target == "mt32" and isinstance(c.mt32_patch, str):
            try:
                prog = resolve_program(c.mt32_patch, "mt32", c.name)
            except ValueError:
                prog = c.gm_program - 1
        elif args.target == "mt32" and isinstance(c.mt32_patch, int):
            prog = c.mt32_patch - 1
        else:
            prog = c.gm_program - 1
        print(f"CH {idx} {c.name} {c.tier} {mc} {prog} {c.volume}")
        idx += 1
    for c in kept:
        mc = assign[id(c)]
        for n in sorted(c.notes, key=lambda n: (n.frame, n.key)):
            print(f"NOTE {n.frame} {n.dur} {mc} {n.key} {n.vel}")
    print("END")
    return 0


if __name__ == "__main__":
    sys.exit(main())
