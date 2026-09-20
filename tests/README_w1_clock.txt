W1 transport-clock gate harness (standalone, no CMake edit)
===========================================================

The ctest suite builds and runs, but its `session_engine` test cannot exercise
the song catalog / enhancement loader in this worktree: `CMakeLists.txt:407-414`
probes `../pokeyellow_msdos/constants` and `../../pokeyellow_msdos/constants`,
neither of which exists (this checkout IS `pokeyellow_msdos`), so
`PKMN_REPO_ROOT_ABS` is empty. That probe is byte-identical at HEAD (verified
with `git archive HEAD`) — it is the PRE-EXISTING session_engine failure, not a
W1 regression; all of its `engine ...` scheduler checks pass. This harness does
not touch CMakeLists.txt (not in the W1 file list) and proves the W1 gate
directly instead.

Build (run from the submodule root):

  c++ -std=c++17 -Wall -Wextra -O2 -Isrc \
      tests/test_w1_clock.cpp src/session_engine.cpp \
      -o build-w1/pkmn-audio-dbg-clock-tests

Run:

  ./build-w1/pkmn-audio-dbg-clock-tests

Expected: `PASS w1 clock: N/N checks`, exit 0.

What it proves (the W1 GATE):
  - one 60 Hz tick per (rate/60) rendered device samples at 1.0x, for every
    render block size from 1 sample to 4096;
  - the tick total over a fixed span of rendered samples is IDENTICAL under a
    60 Hz vsync pattern, a 240 Hz unbounded pattern and a stalled/blocky
    pattern -> tempo invariant to display refresh/vsync;
  - the speed multiplier is a sample divisor (4x -> 192 smp/tick, 0.25x ->
    3072 smp/tick);
  - no back-to-back zero-render burst: a single render-sized advance() never
    dispatches more than one tick, in any pattern (the MUNT silent-note
    condition);
  - pause/stop accrues no tick debt, so resume never bursts.

The live app adds an equivalent runtime check: `DGAD_CLOCK_LOG=1 ./dos-gb-audio-dbg`
prints `[clock] ticks=... max_ticks_per_chunk=1 zero_render_burst=no` every 120
UI frames from the real SDL callback path.
