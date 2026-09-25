#!/usr/bin/env python3
"""enhancement_sysex.py -- machine-readable MT-32 SysEx bridge.

Runtime bridge for the pkmn-audio-dbg viewer, the companion of
enhancement_dump.py. The C++ side NEVER encodes Roland DT1 SysEx or parses
timbres.yaml: it asks this script, which imports the repository's existing
encoders and prints the raw frames:

  * `gen_mt32_patches.load_custom_timbres` / `build_messages` -- the Timbre
    Memory upload blob (tools/audio/mt32/timbres.yaml: 246-byte timbre
    records + Patch Memory rewrites + rhythm setup).
  * `midi_to_stream.find_song_custom_patches` / `build_song_sysex` -- the
    per-song Patch Memory setup and factory-restore cleanup for a pret header
    label (the same calls audition.py makes).

Usage:
    enhancement_sysex.py timbres
    enhancement_sysex.py song <SongLabel>

Output (stdout, parse-friendly; one complete F0..F7 frame per SYSEX line):

    TIMBRES
    SYSEX f0 41 ...
    END

    SONG <SongLabel>
    SETUP
    SYSEX f0 ...
    CLEANUP
    SYSEX f0 ...
    END

An empty section simply has no SYSEX lines; `END` always terminates a
successful run. Exit status 0 on success, 1 on a bad mode / missing encoder.

The timbre blob is built with system=False, exactly like audition.py's host
`--setup` path: the system-area message (reverb/partial reserves/MIDI channel
table/master volume) is a full-boot upload, and MUNT mishandles it as a live
write (parts misroute until restart) while MUNT's default routing already
matches what the songs need. Timbre / Patch Memory / rhythm uploads are
unaffected (see gen_mt32_patches.build_messages).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
# <repo>/dos_port/tools/dos-gb-audio-dbg/src/enhancement_sysex.py:
# parents[0]=src, [1]=dos-gb-audio-dbg, [2]=tools, [3]=dos_port.
# Identical relative discovery to enhancement_dump.py.
AUDIO_DIR = HERE.parents[3] / "tools" / "audio"
sys.path.insert(0, str(AUDIO_DIR))


def _sysex_line(msg: bytes) -> str:
    return "SYSEX " + msg.hex(" ")


def emit_timbres() -> int:
    import yaml  # noqa: E402
    from gen_mt32_patches import DEFS, build_messages  # noqa: E402

    defs = yaml.safe_load(DEFS.read_text()) or {}
    msgs = build_messages(defs, system=False)
    print("TIMBRES")
    for m in msgs:
        print(_sysex_line(m))
    print("END")
    return 0


def emit_song(label: str) -> int:
    from gen_mt32_patches import load_custom_timbres  # noqa: E402
    from midi_to_stream import (  # noqa: E402
        build_song_sysex,
        find_song_custom_patches,
    )

    patches = find_song_custom_patches(label, load_custom_timbres())
    setup, cleanup = build_song_sysex(patches)
    print(f"SONG {label}")
    print("SETUP")
    for m in setup:
        print(_sysex_line(m))
    print("CLEANUP")
    for m in cleanup:
        print(_sysex_line(m))
    print("END")
    return 0


def emit_imfc_voices() -> int:
    from gen_imfc_custom_patches import build_imfc_sysex_messages  # noqa: E402

    msgs = build_imfc_sysex_messages()
    print("TIMBRES")
    for m in msgs:
        print(_sysex_line(m))
    print("END")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=("timbres", "song", "imfc-voices"))
    ap.add_argument("label", nargs="?")
    ap.add_argument("--target", choices=("mt32", "imfc"), default="mt32")
    args = ap.parse_args()
    try:
        if args.mode == "imfc-voices" or (args.mode == "timbres" and args.target == "imfc"):
            return emit_imfc_voices()
        if args.mode == "timbres":
            return emit_timbres()
        if not args.label:
            print("ERROR: song mode needs a <SongLabel>", file=sys.stderr)
            return 1
        return emit_song(args.label)
    except Exception as exc:  # noqa: BLE001 -- bridge surfaces any failure
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
