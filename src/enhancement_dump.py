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


OPL_BASE_CH_TO_MIDI = {
    "ch1": 1,
    "ch2": 2,
    "ch3": 3,
    "ch4": 9,
}


def resolve_opl_voices(resolved, doc=None):
    """[(midi_ch, volume, pan_bits, patch_bytes)] for the OPL3 device.

    Tier-1 melodic channels carrying opl_patch only (mirrors
    gen_enh_streams.py): the OPL3 device never plays tier 2/3, by design.
    Also handles root-level opl_base_channels overrides if present in doc.
    """
    out = []
    if doc and isinstance(doc.get("opl_base_channels"), dict):
        for ch_key, p_name in doc["opl_base_channels"].items():
            if ch_key in OPL_BASE_CH_TO_MIDI and p_name in PATCHES:
                mc = OPL_BASE_CH_TO_MIDI[ch_key]
                out.append((mc, 127, 0x30, list(PATCHES[p_name])))

    kept, assign = assign_channels(resolved)
    for c in kept:
        if c.tier != 1 or c.is_rhythm:
            continue
        if not isinstance(c.opl_patch, str) or c.opl_patch not in PATCHES:
            continue
        vol = getattr(c, "opl_volume", c.volume)
        out.append((assign[id(c)], vol,
                    OPL_PAN_BITS.get(c.pan, 0x30), list(PATCHES[c.opl_patch])))
    return out


def handle_sfx(repo_root: Path, song_name: str, target: str, no_dma: bool = False) -> int | None:
    audition_dir = repo_root / "dos_port" / "tools" / "audio" / "audition"
    sys.path.insert(0, str(audition_dir))
    try:
        from pret_audio import AudioROM
        from gb_to_midi import build_addr_map
        from opl_renderer import sfx_from_headers, simulate_sfx_events
    except ImportError:
        return None

    rom = AudioROM(repo_root)
    amap = build_addr_map(rom)
    headers = sfx_from_headers(rom)

    matched_label = None
    clean_target = song_name.lower().replace("sfx_", "").replace("sfx", "").replace("_", "")
    for h in headers:
        clean_h = h.lower().replace("sfx_", "").replace("sfx", "").replace("_", "")
        if h == song_name or clean_h == clean_target or clean_target in clean_h:
            matched_label = h
            break
    if not matched_label:
        return None

    ch_list = headers[matched_label]
    events, max_frame = simulate_sfx_events(rom, amap, ch_list)

    sfx_yaml_dir = repo_root / "dos_port" / "tools" / "audio" / "sfx"
    yaml_data = {}
    if sfx_yaml_dir.exists():
        for p in sfx_yaml_dir.glob("*.yaml"):
            p_clean = p.stem.lower().replace("sfx_", "").replace("sfx", "").replace("_", "")
            if p_clean == clean_target:
                import yaml
                try:
                    yaml_data = yaml.safe_load(p.read_text(encoding="utf-8")) or {}
                except Exception:
                    pass
                break

    import math
    active_notes: dict[int, tuple[int, int, int]] = {}
    notes: list[tuple[int, int, int, int, int]] = []
    for f in sorted(events.keys()):
        for ev in events[f]:
            ev_type, v = ev[0], ev[1]
            mc = 9 if v == 3 else (v + 1)
            if ev_type == "off":
                if mc in active_notes:
                    start_f, k, vel = active_notes.pop(mc)
                    dur = max(1, f - start_f)
                    notes.append((start_f, dur, mc, k, vel))
            elif ev_type == "noise":
                if mc in active_notes:
                    start_f, k, vel = active_notes.pop(mc)
                    dur = max(1, f - start_f)
                    notes.append((start_f, dur, mc, k, vel))
                v_note = ev[2][2]
                vel = min(127, max(1, v_note * 8))
                active_notes[mc] = (f, 38, vel)
            elif ev_type == "square":
                if mc in active_notes:
                    start_f, k, vel = active_notes.pop(mc)
                    dur = max(1, f - start_f)
                    notes.append((start_f, dur, mc, k, vel))
                freq = ev[3][0]
                vol = ev[3][2]
                hz = 131072.0 / max(1, 2048 - freq)
                k = int(round(69.0 + 12.0 * math.log2(max(1.0, hz) / 440.0)))
                k = max(0, min(127, k))
                vel = min(127, max(1, vol * 8))
                active_notes[mc] = (f, k, vel)
    for mc, (start_f, k, vel) in active_notes.items():
        dur = max(1, max_frame - start_f)
        notes.append((start_f, dur, mc, k, vel))

    used_channels = sorted(set(n[2] for n in notes))
    print(f"OK {song_name} {len(used_channels)} {len(notes)}")
    for idx, mc in enumerate(used_channels):
        print(f"CH {idx} ch{mc} 1 {mc} 0 100")
    for n in sorted(notes, key=lambda x: (x[0], x[2])):
        print(f"NOTE {n[0]} {n[1]} {n[2]} {n[3]} {n[4]}")

    if target == "opl3":
        hw_to_midi = {5: 1, 6: 2, 7: 3, 8: 9}
        ch_cfgs = yaml_data.get("channels", {})
        if isinstance(ch_cfgs, dict):
            for ch_num, cfg in ch_cfgs.items():
                if isinstance(cfg, dict):
                    mc = hw_to_midi.get(int(ch_num))
                    p_name = cfg.get("patch")
                    vol = cfg.get("volume", 100)
                    if mc and p_name in PATCHES:
                        pb = list(PATCHES[p_name])
                        print(f"OPLVOICE {mc} {vol} 48 {' '.join(map(str, pb))}")
    print("END")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("repo_root", type=Path)
    ap.add_argument("song")
    ap.add_argument("--target", choices=("mt32", "gm", "opl3", "gb"), default="mt32")
    ap.add_argument("--no-dma", "--no-pcm", "--fm", dest="no_dma", action="store_true")
    args = ap.parse_args()

    path = args.repo_root / "dos_port" / "tools" / "audio" / "enhancements" \
        / f"{args.song}.yaml"
    if not path.exists():
        sfx_res = handle_sfx(args.repo_root, args.song, args.target, args.no_dma)
        if sfx_res is not None:
            return sfx_res
        print(f"ERROR: no enhancement file: {path}", file=sys.stderr)
        return 1
    rep, resolved, _ = lint(path)
    for w in rep.warnings:
        print(f"warn: {w}", file=sys.stderr)
    if rep.errors:
        for e in rep.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    import yaml
    try:
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception:
        doc = {}

    if args.target == "opl3":
        # Tier-1 foundation only (mirrors gen_enh_streams.py): the OPL3
        # device never plays tier 2/3, by design.
        resolved = [c for c in resolved if c.tier == 1]
    elif args.target == "gb":
        resolved = []
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
        if args.target == "mt32":
            vol = getattr(c, "mt32_volume", c.volume)
        elif args.target == "gm":
            vol = getattr(c, "gm_volume", c.volume)
        else:
            vol = getattr(c, "opl_volume", c.volume)
        print(f"CH {idx} {c.name} {c.tier} {mc} {prog} {vol}")
        idx += 1
    for c in kept:
        mc = assign[id(c)]
        for n in sorted(c.notes, key=lambda n: (n.frame, n.key)):
            print(f"NOTE {n.frame} {n.dur} {mc} {n.key} {n.vel}")
    if args.target == "opl3":
        # Authored FM voices for the OPL3 device's tier-1 channels: 11 raw
        # OPL register bytes per channel (single source of truth:
        # gen_opl_patches.PATCHES). The C++ device applies them per note-on.
        for mc, vol, pan, pb in resolve_opl_voices(resolved, doc):
            print(f"OPLVOICE {mc} {vol} {pan} {' '.join(map(str, pb))}")
    print("END")
    return 0


if __name__ == "__main__":
    sys.exit(main())
