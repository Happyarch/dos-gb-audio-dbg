# pkmn-audio-dbg — Pokémon Yellow Audio Debugger

Standalone Dear ImGui + SDL2 C++ application for inspecting and batch-rendering
the Pokémon Yellow soundtrack. It replaces the legacy
`dos_port/tools/audio/audition.py` TUI: all playback, enhancement auditioning,
and verification now happen here (see §CLI below for the non-interactive
equivalents of the old audition flows).

Plan: `docs/current_plan_debug_frontend.md`. Sources live in `src/`, acceptance
tests in `tests/` (run with `ctest` — all suites must pass 100%).

## Architecture

```
Track selector / A-B slots / enhancement toggle (top bar)
Device tabs: [OPL3] [GB-APU] [MT-32] [General MIDI]
Transport bar: play/pause/stop, frame readout, seek scrubber, loop A/B,
               speed, slot + enhancement toggles, revision stepping
SessionEngine (deterministic 60 Hz scheduler: SongCatalog + EnhancementManager
               event stream -> active SoundDevice)
Device synthesizer layer (polymorphic SoundDevice backends, §Backends)
AudioMixer (SDL2 stereo float32 output, 48 kHz) + Recorder (WAV stems, .audiolog)
Headless CLI (batch render + log replay, no SDL/GL/ImGui)
```

Three intermediate device families share code across backends:

- `MidiDevice` / `MidiTab` — 16-channel MIDI state, piano-roll note history,
  pre-synth mute/solo filtering (MT-32, GM).
- `FmDevice` / `FmTab` — operator parameter matrix, envelope phases, carrier
  scopes (OPL3).
- `PsgDevice` / `PsgTab` — duty cycle, wave RAM, LFSR noise state (GB-APU).

Channels wake lazily on first use; dormant channels take a fast early-return
escape in render, scope, and UI paths. Adding a future device (SSI-2001, FB-01)
touches only its device/tab subclass plus factory registration.

## Synthesis backends

| Tab      | Backend class  | Core linked                              | Native rate |
|----------|----------------|------------------------------------------|-------------|
| OPL3     | `Opl3Device`   | NukedOPL3 (`../dosbox-x/src/hardware/`)  | 49716 Hz    |
| GB-APU   | `GbApuDevice`  | Gb_Snd_Emu (`../audio/.gb_snd_emu/`)     | 48000 Hz    |
| MT-32    | `Mt32Device`   | Munt `libmt32emu` (system, pkg-config)   | 32000 Hz    |
| Gen. MIDI| `GmDevice`     | FluidSynth `libfluidsynth` (pkg-config)  | 44100 Hz    |

MT-32 needs control + PCM ROMs (`MT32_ROM_DIR`, plus the usual system paths);
GM needs a SoundFont (`SOUNDFONT`). With `MT32_ROM_DIR=none` / `SOUNDFONT=none`
(or no files found) the device runs a deterministic mock sine mixer so dispatch,
rendering, and the headless test suite stay functional on any machine.

Track data comes from the repository itself: `SongCatalog` parses
`constants/music_constants.asm` + `audio/headers/*.asm`, and
`EnhancementManager` loads GB baselines from `dos_port/assets/midi/<target>/`
plus the compiled `dos_port/tools/audio/enhancements/*.yaml` layer.

## Build

```sh
cmake -B build && cmake --build build
ctest --test-dir build   # all suites (incl. cli_verification) must pass
```

Zero-warning policy: every target builds with `-Wall -Wextra`
(third-party cores use `-w` / SYSTEM includes).

## CLI (headless batch mode)

Interactive GUI is the default (no flags). Batch rendering needs no window,
no OpenGL context, and no audio hardware:

```sh
./build/pkmn-audio-dbg --headless --track MUSIC_PALLET_TOWN --out pal.wav
./build/pkmn-audio-dbg --headless --track routes1 --device gm --frames 600 \
    --out r1.wav --no-enh
./build/pkmn-audio-dbg --replay take.audiolog --device mt32 --out take.wav
./build/pkmn-audio-dbg --help
```

| Flag              | Meaning                                                      |
|-------------------|--------------------------------------------------------------|
| `--help`          | Print usage and exit 0.                                      |
| `--headless`      | Render without SDL window / OpenGL / ImGui. Required with `--track`. |
| `--track <ID>`    | Track constant or fuzzy name (`MUSIC_PALLET_TOWN`, `PalletTown`, `routes1`). |
| `--device <NAME>` | Backend: `mt32`, `gm`, `opl3`, `gbapu` (default `mt32`).    |
| `--frames <N>`    | 60 Hz frames to render. Default: track (or log) length, else 3600 (60 s). |
| `--out <WAV>`     | Stereo float32 WAV at the device native rate. Without `--out` the audio is rendered and discarded (smoke render). |
| `--no-enh`        | GB baseline only (disables the enhancement overlay).         |
| `--enhancement 0|1` | Explicit form of the same switch.                         |
| `--replay <log>`  | Replay a captured `.audiolog` frame-by-frame (implies `--headless`). |

Per-frame sample counts follow the device rate exactly
(`floor(((i+1)*rate)/60) - floor((i*rate)/60)`), so repeated renders of the
same track/log are bit-identical (verified by the `cli_verification` suite:
track re-render null difference, record/replay parity null difference).

## UI keyboard shortcuts

| Key         | Action                                              |
|-------------|-----------------------------------------------------|
| Space       | Play / pause                                        |
| Home        | Rewind to frame 0                                   |
| Left/Right  | Step one frame back / forward                       |
| E           | Toggle enhancement overlay ([E] vs [G] baseline)    |
| Tab         | Toggle A/B comparison slot (position-locked)        |
| `[` / `]`   | Step to previous / next enhancement revision        |
| Esc         | Quit                                                |

## Recording

- Transport-bar Rec arms WAV stem export (`Recorder`: master + per-channel
  stems; stems sum to master within float rounding).
- Raw `(frame, opcode, payload)` streams capture to `.audiolog`
  (`AudioLogWriter`) and replay deterministically via `--replay`
  (`AudioLogReader` + `dispatchReplayEvent`, same mapping the session engine
  uses for live dispatch).

## Retiring audition.py

`dos_port/tools/audio/audition.py` is superseded. Old flow -> new flow:

- `audition.py` interactive TUI -> run `pkmn-audio-dbg` (same track catalog,
  same YAML enhancements, per-backend tabs, transport + A/B + revisions).
- Reference render / diff listening -> `--headless --track ... --out ...`
  (deterministic WAV) or `--replay` for captured streams.
- `revisions.py` snapshots -> built-in revision manager (`[`, `]`, slot
  comparison), same `{id:04d}_{timestamp}_{note}.yaml` layout.
