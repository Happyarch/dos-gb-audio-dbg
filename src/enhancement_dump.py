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
    OPLVOICE <midi_ch> <volume> <pan_bits> <b0> ... <b10>  (--target opl3)
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
from gen_opl_patches import PATCHES  # noqa: E402
from mt32_presets import resolve_program  # noqa: E402
from yaml_lint import lint  # noqa: E402

# C0 pan bits, mirroring audition/opl_renderer.py PAN_BITS.
OPL_PAN_BITS = {"left": 0x10, "right": 0x20, "center": 0x30}


def assign_channels(resolved):
    """(kept, assign): channel assignment mirroring gb_to_midi.

    Tier-sorted; melodic channels take the free parts [4,5,6,7,8] (extras
    dropped), rhythm channels go to 9. `assign` maps id(channel) -> MIDI ch.
    """
    chans = sorted((c for c in resolved if c.notes), key=lambda c: c.tier)
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
    return kept, assign


def resolve_opl_voices(resolved):
    """[(midi_ch, volume, pan_bits, patch_bytes)] for the OPL3 device.

    Tier-1 melodic channels carrying opl_patch only (mirrors
    gen_enh_streams.py): the OPL3 device never plays tier 2/3, by design.
    lint() guarantees tier-1 channels carry a valid opl_patch; anything
    else is skipped so the device keeps its default voice. patch_bytes are
    the 11 raw OPL register values from gen_opl_patches.PATCHES.
    """
    kept, assign = assign_channels(resolved)
    out = []
    for c in kept:
        if c.tier != 1 or c.is_rhythm:
            continue
        if not isinstance(c.opl_patch, str) or c.opl_patch not in PATCHES:
            continue
        out.append((assign[id(c)], c.volume,
                    OPL_PAN_BITS.get(c.pan, 0x30), list(PATCHES[c.opl_patch])))
    return out


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

    if args.target == "opl3":
        # Tier-1 foundation only (mirrors gen_enh_streams.py): the OPL3
        # device never plays tier 2/3, by design.
        resolved = [c for c in resolved if c.tier == 1]
    kept, assign = assign_channels(resolved)
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
    if args.target == "opl3":
        # Authored FM voices for the OPL3 device's tier-1 channels: 11 raw
        # OPL register bytes per channel (single source of truth:
        # gen_opl_patches.PATCHES). The C++ device applies them per note-on.
        for mc, vol, pan, pb in resolve_opl_voices(resolved):
            print(f"OPLVOICE {mc} {vol} {pan} {' '.join(map(str, pb))}")
    print("END")
    return 0


if __name__ == "__main__":
    sys.exit(main())
